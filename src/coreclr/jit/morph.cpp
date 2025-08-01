// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

/*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
XX                                                                           XX
XX                          Morph                                            XX
XX                                                                           XX
XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
*/

#include "jitpch.h"
#ifdef _MSC_VER
#pragma hdrstop
#endif

#include "allocacheck.h" // for alloca

//-------------------------------------------------------------
// fgMorphInit: prepare for running the morph phases
//
// Returns:
//   suitable phase status
//
PhaseStatus Compiler::fgMorphInit()
{
    bool madeChanges = false;

    // We could allow ESP frames. Just need to reserve space for
    // pushing EBP if the method becomes an EBP-frame after an edit.
    // Note that requiring a EBP Frame disallows double alignment.  Thus if we change this
    // we either have to disallow double alignment for E&C some other way or handle it in EETwain.

    if (opts.compDbgEnC)
    {
        codeGen->setFramePointerRequired(true);

        // We don't care about localloc right now. If we do support it,
        // EECodeManager::FixContextForEnC() needs to handle it smartly
        // in case the localloc was actually executed.
        //
        // compLocallocUsed            = true;
    }

    fgAvailableOutgoingArgTemps = hashBv::Create(this);

    // Insert call to class constructor as the first basic block if
    // we were asked to do so.
    if (info.compCompHnd->initClass(nullptr /* field */, nullptr /* method */,
                                    impTokenLookupContextHandle /* context */) &
        CORINFO_INITCLASS_USE_HELPER)
    {
        fgNewStmtAtBeg(fgFirstBB, fgInitThisClass());
        madeChanges = true;
    }

#ifdef DEBUG
    if (opts.compGcChecks)
    {
        for (unsigned i = 0; i < info.compArgsCount; i++)
        {
            if (lvaGetDesc(i)->TypeIs(TYP_REF))
            {
                // confirm that the argument is a GC pointer (for debugging (GC stress))
                GenTree* op = gtNewLclvNode(i, TYP_REF);
                op          = gtNewHelperCallNode(CORINFO_HELP_CHECK_OBJ, TYP_VOID, op);

                fgNewStmtAtBeg(fgFirstBB, op);
                madeChanges = true;
                if (verbose)
                {
                    printf("\ncompGcChecks tree:\n");
                    gtDispTree(op);
                }
            }
        }
    }
#endif // DEBUG

#if defined(DEBUG) && defined(TARGET_XARCH)
    if (opts.compStackCheckOnRet)
    {
        lvaReturnSpCheck = lvaGrabTempWithImplicitUse(false DEBUGARG("ReturnSpCheck"));
        lvaSetVarDoNotEnregister(lvaReturnSpCheck, DoNotEnregisterReason::ReturnSpCheck);
        lvaGetDesc(lvaReturnSpCheck)->lvType = TYP_I_IMPL;
        madeChanges                          = true;
    }
#endif // defined(DEBUG) && defined(TARGET_XARCH)

#if defined(DEBUG) && defined(TARGET_X86)
    if (opts.compStackCheckOnCall)
    {
        lvaCallSpCheck = lvaGrabTempWithImplicitUse(false DEBUGARG("CallSpCheck"));
        lvaSetVarDoNotEnregister(lvaCallSpCheck, DoNotEnregisterReason::CallSpCheck);
        lvaGetDesc(lvaCallSpCheck)->lvType = TYP_I_IMPL;
        madeChanges                        = true;
    }
#endif // defined(DEBUG) && defined(TARGET_X86)

    return madeChanges ? PhaseStatus::MODIFIED_EVERYTHING : PhaseStatus::MODIFIED_NOTHING;
}

// Convert the given node into a call to the specified helper passing
// the given argument list.
//
// Tries to fold constants and also adds an edge for overflow exception
// returns the morphed tree
GenTree* Compiler::fgMorphCastIntoHelper(GenTree* tree, int helper, GenTree* oper)
{
    GenTree* result;

    /* If the operand is a constant, we'll try to fold it */
    if (oper->OperIsConst())
    {
        GenTree* oldTree = tree;

        tree = gtFoldExprConst(tree); // This may not fold the constant (NaN ...)

        if (tree != oldTree)
        {
            return fgMorphTree(tree);
        }
        else if (tree->OperIsConst())
        {
            return fgMorphConst(tree);
        }

        // assert that oper is unchanged and that it is still a GT_CAST node
        noway_assert(tree->AsCast()->CastOp() == oper);
        noway_assert(tree->OperIs(GT_CAST));
    }
    result = fgMorphIntoHelperCall(tree, helper, true /* morphArgs */, oper);
    assert(result == tree);
    return result;
}

class SharedTempsScope
{
    Compiler*             m_comp;
    ArrayStack<unsigned>  m_usedSharedTemps;
    ArrayStack<unsigned>* m_prevUsedSharedTemps;

public:
    SharedTempsScope(Compiler* comp)
        : m_comp(comp)
        , m_usedSharedTemps(comp->getAllocator(CMK_CallArgs))
        , m_prevUsedSharedTemps(comp->fgUsedSharedTemps)
    {
        comp->fgUsedSharedTemps = &m_usedSharedTemps;
    }

    ~SharedTempsScope()
    {
        m_comp->fgUsedSharedTemps = m_prevUsedSharedTemps;

        for (int i = 0; i < m_usedSharedTemps.Height(); i++)
        {
            m_comp->fgAvailableOutgoingArgTemps->setBit((indexType)m_usedSharedTemps.Top(i));
        }
    }
};

//------------------------------------------------------------------------
// fgMorphIntoHelperCall:
//   Morph a node into a helper call, specifying up to two args and whether to
//   call fgMorphArgs after.
//
// Parameters:
//   tree       - The node that is changed. This must be a large node.
//   helper     - The helper.
//   morphArgs  - Whether to call fgMorphArgs after adding the args.
//   arg1, arg2 - Optional arguments to add to the call.
//
// Return value:
//   The call (which is the same as `tree`).
//
GenTree* Compiler::fgMorphIntoHelperCall(GenTree* tree, int helper, bool morphArgs, GenTree* arg1, GenTree* arg2)
{
    // The helper call ought to be semantically equivalent to the original node, so preserve its VN.
    tree->ChangeOper(GT_CALL, GenTree::PRESERVE_VN);

    GenTreeCall* call = tree->AsCall();
    // Args are cleared by ChangeOper above
    call->gtCallType      = CT_HELPER;
    call->gtReturnType    = tree->TypeGet();
    call->gtCallMethHnd   = eeFindHelper(helper);
    call->gtRetClsHnd     = nullptr;
    call->gtCallMoreFlags = GTF_CALL_M_EMPTY;
    INDEBUG(call->gtCallDebugFlags = GTF_CALL_MD_EMPTY);
    call->gtControlExpr = nullptr;
    call->ClearInlineInfo();
#ifdef UNIX_X86_ABI
    call->gtFlags |= GTF_CALL_POP_ARGS;
#endif // UNIX_X86_ABI

#if DEBUG
    // Helper calls are never candidates.
    call->gtInlineObservation = InlineObservation::CALLSITE_IS_CALL_TO_HELPER;

    call->callSig = nullptr;
#endif // DEBUG

#ifdef FEATURE_READYTORUN
    call->gtEntryPoint.addr       = nullptr;
    call->gtEntryPoint.accessType = IAT_VALUE;
#endif

#if FEATURE_MULTIREG_RET
    call->ResetReturnType();
    call->ClearOtherRegs();
    call->ClearOtherRegFlags();
#ifndef TARGET_64BIT
    if (varTypeIsLong(tree))
    {
        call->InitializeLongReturnType();
    }
#endif // !TARGET_64BIT
#endif // FEATURE_MULTIREG_RET

    if (call->OperMayThrow(this))
    {
        call->gtFlags |= GTF_EXCEPT;
    }
    else
    {
        call->gtFlags &= ~GTF_EXCEPT;
    }
    call->gtFlags |= GTF_CALL;

    if (arg2 != nullptr)
    {
        call->gtArgs.PushFront(this, NewCallArg::Primitive(arg2));
        call->gtFlags |= arg2->gtFlags & GTF_ALL_EFFECT;
    }

    if (arg1 != nullptr)
    {
        call->gtArgs.PushFront(this, NewCallArg::Primitive(arg1));
        call->gtFlags |= arg1->gtFlags & GTF_ALL_EFFECT;
    }

    // Perform the morphing

    if (morphArgs)
    {
        SharedTempsScope scope(this);
        tree = fgMorphArgs(call);
    }

    tree->SetMorphed(this);

    return tree;
}

//------------------------------------------------------------------------
// fgMorphExpandCast: Performs the pre-order (required) morphing for a cast.
//
// Performs a rich variety of pre-order transformations (and some optimizations).
//
// Notably:
//  1. Splits long -> small type casts into long -> int -> small type
//     for 32 bit targets. Does the same for float/double -> small type
//     casts for all targets.
//  2. Morphs casts not supported by the target directly into helpers.
//     These mostly have to do with casts from and to floating point
//     types, especially checked ones. Refer to the implementation for
//     what specific casts need to be handled - it is a complex matrix.
//  3. "Casts away" the GC-ness of a tree (for CAST(nint <- byref)) via
//     storing the GC tree to an inline non-GC temporary.
//  3. "Pushes down" truncating long -> int casts for some operations:
//     CAST(int <- MUL(long, long)) => MUL(CAST(int <- long), CAST(int <- long)).
//     The purpose of this is to allow "optNarrowTree" in the post-order
//     traversal to fold the tree into a TYP_INT one, which helps 32 bit
//     targets (and AMD64 too since 32 bit instructions are more compact).
//     TODO-Arm64-CQ: Re-evaluate the value of this optimization for ARM64.
//
// Arguments:
//    tree - the cast tree to morph
//
// Return Value:
//    The fully morphed tree, or "nullptr" if it needs further morphing,
//    in which case the cast may be transformed into an unchecked one
//    and its operand changed (the cast "expanded" into two).
//
GenTree* Compiler::fgMorphExpandCast(GenTreeCast* tree)
{
    GenTree*  oper    = tree->CastOp();
    var_types srcType = genActualType(oper);
    var_types dstType = tree->CastToType();
    unsigned  dstSize = genTypeSize(dstType);

    // See if the cast has to be done in two steps.  R -> I
    if (varTypeIsFloating(srcType) && varTypeIsIntegral(dstType))
    {
        if (srcType == TYP_FLOAT
#ifdef TARGET_64BIT
            // 64-bit: src = float, dst is overflow conversion.
            // This goes through helper and hence src needs to be converted to double.
            && tree->gtOverflow()
#else
            // 32-bit: src = float, dst = int64/uint64 or overflow conversion.
            && (tree->gtOverflow() || varTypeIsLong(dstType))
#endif // TARGET_64BIT
        )
        {
            oper = gtNewCastNode(TYP_DOUBLE, oper, false, TYP_DOUBLE);
        }

        // Do we need to do it in two steps R -> I -> smallType?
        if (dstSize < genTypeSize(TYP_INT))
        {
            oper = gtNewCastNodeL(TYP_INT, oper, /* fromUnsigned */ false, TYP_INT);
            oper->gtFlags |= (tree->gtFlags & (GTF_OVERFLOW | GTF_EXCEPT));
            tree->AsCast()->CastOp() = oper;
            // We must not mistreat the original cast, which was from a floating point type,
            // as from an unsigned type, since we now have a TYP_INT node for the source and
            // CAST_OVF(BYTE <- INT) != CAST_OVF(BYTE <- UINT).
            assert(!tree->IsUnsigned());
        }
        else
        {
            if (!tree->gtOverflow())
            {
#ifdef TARGET_64BIT
                return nullptr;
#else
                if (!varTypeIsLong(dstType))
                {
                    return nullptr;
                }

                switch (dstType)
                {
                    case TYP_LONG:
                        return fgMorphCastIntoHelper(tree, CORINFO_HELP_DBL2LNG, oper);
                    case TYP_ULONG:
                        return fgMorphCastIntoHelper(tree, CORINFO_HELP_DBL2ULNG, oper);
                    default:
                        unreached();
                }
#endif // TARGET_64BIT
            }
            else
            {
                switch (dstType)
                {
                    case TYP_INT:
                        return fgMorphCastIntoHelper(tree, CORINFO_HELP_DBL2INT_OVF, oper);
                    case TYP_UINT:
                        return fgMorphCastIntoHelper(tree, CORINFO_HELP_DBL2UINT_OVF, oper);
                    case TYP_LONG:
                        return fgMorphCastIntoHelper(tree, CORINFO_HELP_DBL2LNG_OVF, oper);
                    case TYP_ULONG:
                        return fgMorphCastIntoHelper(tree, CORINFO_HELP_DBL2ULNG_OVF, oper);
                    default:
                        unreached();
                }
            }
        }
    }

    // If we have a double->float cast, and the double node is itself a cast,
    // look through it and see if we can cast directly to float. This is valid
    // only if the cast to double would have been lossless.
    //
    // This pattern most often appears as CAST(float <- CAST(double <- float)),
    // which is reduced to CAST(float <- float) and handled in codegen as an optional mov.
    else if ((srcType == TYP_DOUBLE) && (dstType == TYP_FLOAT) && oper->OperIs(GT_CAST) &&
             !varTypeIsLong(oper->AsCast()->CastOp()))
    {
        oper->gtType       = TYP_FLOAT;
        oper->CastToType() = TYP_FLOAT;

        return fgMorphTree(oper);
    }

#ifndef TARGET_64BIT
    // The code generation phase (for x86 & ARM32) does not handle casts
    // directly from [u]long to anything other than [u]int. Insert an
    // intermediate cast to native int.
    else if (varTypeIsLong(srcType) && varTypeIsSmall(dstType))
    {
        oper = gtNewCastNode(TYP_I_IMPL, oper, tree->IsUnsigned(), TYP_I_IMPL);
        oper->gtFlags |= (tree->gtFlags & (GTF_OVERFLOW | GTF_EXCEPT));
        tree->ClearUnsigned();
        tree->AsCast()->CastOp() = oper;
    }
#endif //! TARGET_64BIT

#ifdef TARGET_ARM
    // converts long/ulong --> float/double casts into helper calls.
    else if (varTypeIsFloating(dstType) && varTypeIsLong(srcType))
    {
        CorInfoHelpFunc helper = CORINFO_HELP_UNDEF;
        if (dstType == TYP_FLOAT)
        {
            helper = tree->IsUnsigned() ? CORINFO_HELP_ULNG2FLT : CORINFO_HELP_LNG2FLT;
        }
        else
        {
            helper = tree->IsUnsigned() ? CORINFO_HELP_ULNG2DBL : CORINFO_HELP_LNG2DBL;
        }
        return fgMorphCastIntoHelper(tree, helper, oper);
    }
#endif // TARGET_ARM

#ifdef TARGET_AMD64
    // Do we have to do two step U4 -> R4/8 ?
    // If we don't have the EVEX unsigned conversion instructions available,
    // we will widen to long and use signed conversion: U4 -> Long -> R4/8.
    // U8 -> R4/R8 is handled directly in codegen, so we ignore it here.
    else if (tree->IsUnsigned() && varTypeIsFloating(dstType))
    {
        srcType = varTypeToUnsigned(srcType);

        if (srcType == TYP_UINT && !canUseEvexEncoding())
        {
            oper = gtNewCastNode(TYP_LONG, oper, true, TYP_LONG);
            oper->gtFlags |= (tree->gtFlags & (GTF_OVERFLOW | GTF_EXCEPT));
            tree->ClearUnsigned();
            tree->CastOp() = oper;
        }
    }
#endif // TARGET_AMD64

#ifdef TARGET_X86
#ifdef FEATURE_HW_INTRINSICS
    else if (varTypeIsLong(srcType) && varTypeIsFloating(dstType) && canUseEvexEncoding())
    {
        // We can handle these casts directly using SIMD instructions.
        // The transform to SIMD is done in DecomposeLongs.
        return nullptr;
    }
#endif // FEATURE_HW_INTRINSICS

    // Do we have to do two step U4/8 -> R4/8 ?
    else if (tree->IsUnsigned() && varTypeIsFloating(dstType))
    {
        srcType = varTypeToUnsigned(srcType);

        if (srcType == TYP_ULONG)
        {
            CorInfoHelpFunc helper = (dstType == TYP_FLOAT) ? CORINFO_HELP_ULNG2FLT : CORINFO_HELP_ULNG2DBL;
            return fgMorphCastIntoHelper(tree, helper, oper);
        }
        else if (srcType == TYP_UINT && !canUseEvexEncoding())
        {
            oper = gtNewCastNode(TYP_LONG, oper, true, TYP_LONG);
            oper->gtFlags |= (tree->gtFlags & (GTF_OVERFLOW | GTF_EXCEPT));
            tree->ClearUnsigned();

            CorInfoHelpFunc helper = (dstType == TYP_FLOAT) ? CORINFO_HELP_LNG2FLT : CORINFO_HELP_LNG2DBL;
            return fgMorphCastIntoHelper(tree, helper, oper);
        }
    }
    else if (!tree->IsUnsigned() && (srcType == TYP_LONG) && varTypeIsFloating(dstType))
    {
        CorInfoHelpFunc helper = (dstType == TYP_FLOAT) ? CORINFO_HELP_LNG2FLT : CORINFO_HELP_LNG2DBL;
        return fgMorphCastIntoHelper(tree, helper, oper);
    }
#endif // TARGET_X86
    else if (varTypeIsGC(srcType) != varTypeIsGC(dstType))
    {
        // We are casting away GC information.  we would like to just
        // change the type to int, however this gives the emitter fits because
        // it believes the variable is a GC variable at the beginning of the
        // instruction group, but is not turned non-gc by the code generator
        // we fix this by copying the GC pointer to a non-gc pointer temp.
        noway_assert(!varTypeIsGC(dstType) && "How can we have a cast to a GCRef here?");

        // We generate a store to an int and then do the cast from an int. With this we avoid
        // the gc problem and we allow casts to bytes, longs,  etc...
        unsigned lclNum = lvaGrabTemp(true DEBUGARG("Cast away GC"));
        oper->gtType    = TYP_I_IMPL;
        GenTree* store  = gtNewTempStore(lclNum, oper);
        oper->gtType    = srcType;

        // do the real cast
        GenTree* cast = gtNewCastNode(tree->TypeGet(), gtNewLclvNode(lclNum, TYP_I_IMPL), false, dstType);

        // Generate the comma tree
        oper = gtNewOperNode(GT_COMMA, tree->TypeGet(), store, cast);

        return fgMorphTree(oper);
    }

    // Look for narrowing casts ([u]long -> [u]int) and try to push them
    // down into the operand before morphing it.
    //
    // It doesn't matter if this is cast is from ulong or long (i.e. if
    // GTF_UNSIGNED is set) because the transformation is only applied to
    // overflow-insensitive narrowing casts, which always silently truncate.
    //
    // Note that casts from [u]long to small integer types are handled above.
    if ((srcType == TYP_LONG) && ((dstType == TYP_INT) || (dstType == TYP_UINT)))
    {
        // As a special case, look for overflow-sensitive casts of an AND
        // expression, and see if the second operand is a small constant. Since
        // the result of an AND is bound by its smaller operand, it may be
        // possible to prove that the cast won't overflow, which will in turn
        // allow the cast's operand to be transformed.
        if (tree->gtOverflow() && oper->OperIs(GT_AND))
        {
            GenTree* andOp2 = oper->AsOp()->gtOp2;

            // Look for a constant less than 2^{32} for a cast to uint, or less
            // than 2^{31} for a cast to int.
            int maxWidth = (dstType == TYP_UINT) ? 32 : 31;

            if (andOp2->OperIs(GT_CNS_NATIVELONG) && ((andOp2->AsIntConCommon()->LngValue() >> maxWidth) == 0))
            {
                tree->ClearOverflow();
                tree->SetAllEffectsFlags(oper);
            }
        }

        // Only apply this transformation during global morph,
        // when neither the cast node nor the oper node may throw an exception
        // based on the upper 32 bits.
        //
        if (fgGlobalMorph && !tree->gtOverflow() && !oper->gtOverflowEx())
        {
            // For these operations the lower 32 bits of the result only depends
            // upon the lower 32 bits of the operands.
            //
            bool canPushCast = oper->OperIs(GT_ADD, GT_SUB, GT_MUL, GT_AND, GT_OR, GT_XOR, GT_NOT, GT_NEG);

            // For long LSH cast to int, there is a discontinuity in behavior
            // when the shift amount is 32 or larger.
            //
            // CAST(INT, LSH(1LL, 31)) == LSH(1, 31)
            // LSH(CAST(INT, 1LL), CAST(INT, 31)) == LSH(1, 31)
            //
            // CAST(INT, LSH(1LL, 32)) == 0
            // LSH(CAST(INT, 1LL), CAST(INT, 32)) == LSH(1, 32) == LSH(1, 0) == 1
            //
            // So some extra validation is needed.
            //
            if (oper->OperIs(GT_LSH))
            {
                GenTree* shiftAmount = oper->AsOp()->gtOp2;

                // Expose constant value for shift, if possible, to maximize the number
                // of cases we can handle.
                shiftAmount         = gtFoldExpr(shiftAmount);
                oper->AsOp()->gtOp2 = shiftAmount;

                if (shiftAmount->IsIntegralConst())
                {
                    const ssize_t shiftAmountValue = shiftAmount->AsIntCon()->IconValue();

                    if ((shiftAmountValue >= 64) || (shiftAmountValue < 0))
                    {
                        // Shift amount is large enough or negative so result is undefined.
                        // Don't try to optimize.
                        assert(!canPushCast);
                    }
                    else if (shiftAmountValue >= 32)
                    {
                        // We know that we have a narrowing cast ([u]long -> [u]int)
                        // and that we are casting to a 32-bit value, which will result in zero.
                        //
                        // Check to see if we have any side-effects that we must keep
                        //
                        if ((tree->gtFlags & GTF_ALL_EFFECT) == 0)
                        {
                            // Result of the shift is zero.
                            DEBUG_DESTROY_NODE(tree);
                            GenTree* zero = gtNewZeroConNode(TYP_INT);
                            return fgMorphTree(zero);
                        }
                        else // We do have a side-effect
                        {
                            // We could create a GT_COMMA node here to keep the side-effect and return a zero
                            // Instead we just don't try to optimize this case.
                            canPushCast = false;
                        }
                    }
                    else
                    {
                        // Shift amount is positive and small enough that we can push the cast through.
                        canPushCast = true;
                    }
                }
                else
                {
                    // Shift amount is unknown. We can't optimize this case.
                    assert(!canPushCast);
                }
            }

            if (canPushCast)
            {
                GenTree* op1 = oper->gtGetOp1();
                GenTree* op2 = oper->gtGetOp2IfPresent();

                canPushCast = !varTypeIsGC(op1) && ((op2 == nullptr) || !varTypeIsGC(op2));
            }

            if (canPushCast)
            {
                DEBUG_DESTROY_NODE(tree);

                // Insert narrowing casts for op1 and op2.
                oper->AsOp()->gtOp1 = gtNewCastNode(TYP_INT, oper->AsOp()->gtOp1, false, dstType);
                if (oper->AsOp()->gtOp2 != nullptr)
                {
                    oper->AsOp()->gtOp2 = gtNewCastNode(TYP_INT, oper->AsOp()->gtOp2, false, dstType);
                }

                // Clear the GT_MUL_64RSLT if it is set.
                if (oper->OperIs(GT_MUL) && (oper->gtFlags & GTF_MUL_64RSLT))
                {
                    oper->gtFlags &= ~GTF_MUL_64RSLT;
                }

                // The operation now produces a 32-bit result.
                oper->gtType = TYP_INT;

                // Remorph the new tree as the casts that we added may be folded away.
                return fgMorphTree(oper);
            }
        }
    }

    return nullptr;
}

#ifdef DEBUG

//------------------------------------------------------------------------
// getWellKnownArgName: Get a string representation of a WellKnownArg.
//
const char* getWellKnownArgName(WellKnownArg arg)
{
    switch (arg)
    {
        case WellKnownArg::None:
            return "None";
        case WellKnownArg::ThisPointer:
            return "ThisPointer";
        case WellKnownArg::VarArgsCookie:
            return "VarArgsCookie";
        case WellKnownArg::InstParam:
            return "InstParam";
        case WellKnownArg::AsyncContinuation:
            return "AsyncContinuation";
        case WellKnownArg::RetBuffer:
            return "RetBuffer";
        case WellKnownArg::PInvokeFrame:
            return "PInvokeFrame";
        case WellKnownArg::WrapperDelegateCell:
            return "WrapperDelegateCell";
        case WellKnownArg::ShiftLow:
            return "ShiftLow";
        case WellKnownArg::ShiftHigh:
            return "ShiftHigh";
        case WellKnownArg::VirtualStubCell:
            return "VirtualStubCell";
        case WellKnownArg::PInvokeCookie:
            return "PInvokeCookie";
        case WellKnownArg::PInvokeTarget:
            return "PInvokeTarget";
        case WellKnownArg::R2RIndirectionCell:
            return "R2RIndirectionCell";
        case WellKnownArg::ValidateIndirectCallTarget:
            return "ValidateIndirectCallTarget";
        case WellKnownArg::DispatchIndirectCallTarget:
            return "DispatchIndirectCallTarget";
        case WellKnownArg::SwiftError:
            return "SwiftError";
        case WellKnownArg::SwiftSelf:
            return "SwiftSelf";
        case WellKnownArg::X86TailCallSpecialArg:
            return "X86TailCallSpecialArg";
        case WellKnownArg::StackArrayLocal:
            return "StackArrayLocal";
        case WellKnownArg::RuntimeMethodHandle:
            return "RuntimeMethodHandle";
        case WellKnownArg::AsyncSuspendedIndicator:
            return "AsyncSuspendedIndicator";
    }

    return "N/A";
}

//------------------------------------------------------------------------
// Dump: Dump information about a CallArg to jitstdout.
//
void CallArg::Dump(Compiler* comp)
{
    printf("CallArg[[%06u].%s", comp->dspTreeID(GetNode()), GenTree::OpName(GetNode()->OperGet()));
    printf(" %s", varTypeName(m_signatureType));
    printf(" (%s)", AbiInfo.IsPassedByReference() ? "By ref" : "By value");
    printf(", %u segments:", AbiInfo.NumSegments);
    for (const ABIPassingSegment& segment : AbiInfo.Segments())
    {
        printf(" <");
        segment.Dump();
        printf(">");
    }
    if (m_needPlace)
    {
        printf(", needPlace");
    }
    if (m_processed)
    {
        printf(", processed");
    }
    if (m_wellKnownArg != WellKnownArg::None)
    {
        printf(", wellKnown[%s]", getWellKnownArgName(m_wellKnownArg));
    }
    printf("]\n");
}
#endif

//------------------------------------------------------------------------
// ArgsComplete: Make final decisions on which arguments to evaluate into temporaries.
//
void CallArgs::ArgsComplete(Compiler* comp, GenTreeCall* call)
{
    unsigned argCount = CountArgs();

    // Previous argument with GTF_EXCEPT
    GenTree* prevExceptionTree = nullptr;
    // Exceptions previous tree with GTF_EXCEPT may throw (computed lazily, may
    // be empty)
    ExceptionSetFlags prevExceptionFlags = ExceptionSetFlags::None;

    for (CallArg& arg : Args())
    {
        GenTree* argx = arg.GetEarlyNode();
        assert(argx != nullptr);

        bool canEvalToTemp = true;
#if !FEATURE_FIXED_OUT_ARGS
        if (!arg.AbiInfo.HasAnyRegisterSegment())
        {
            // Non-register arguments are evaluated and pushed in order; they
            // should never go in the late arg list.
            canEvalToTemp = false;
        }
#endif

        // If the argument tree contains a store (GTF_ASG) then the argument and
        // and every earlier argument (except constants) must be evaluated into temps
        // since there may be other arguments that follow and they may use the value being defined.
        //
        // EXAMPLE: ArgTab is "a, a=5, a"
        //          -> when we see the second arg "a=5"
        //             we know the first two arguments "a, a=5" have to be evaluated into temps
        //
        if ((argx->gtFlags & GTF_ASG) != 0)
        {
            // fgMakeOutgoingStructArgCopy can have introduced a temp already,
            // in which case it will have created a setup node in the early
            // node.
            if (!argx->IsValue())
            {
                assert(arg.m_needTmp);
            }
            else if (canEvalToTemp && (argCount > 1))
            {
                SetNeedsTemp(&arg);
            }

            // For all previous arguments that may interfere with the store we
            // require that they be evaluated into temps.
            for (CallArg& prevArg : Args())
            {
                if (&prevArg == &arg)
                {
                    break;
                }

#if !FEATURE_FIXED_OUT_ARGS
                if (!prevArg.AbiInfo.HasAnyRegisterSegment())
                {
                    // All stack args are already evaluated and placed in order
                    // in this case.
                    continue;
                }
#endif

                if ((prevArg.GetEarlyNode() == nullptr) || prevArg.m_needTmp)
                {
                    continue;
                }

                if (((prevArg.GetEarlyNode()->gtFlags & GTF_ALL_EFFECT) != 0) ||
                    comp->gtMayHaveStoreInterference(argx, prevArg.GetEarlyNode()))
                {
                    SetNeedsTemp(&prevArg);
                }
            }
        }

        bool treatLikeCall = ((argx->gtFlags & GTF_CALL) != 0);

        ExceptionSetFlags exceptionFlags = ExceptionSetFlags::None;
#if FEATURE_FIXED_OUT_ARGS
        // Like calls, if this argument has a tree that will do an inline throw,
        // a call to a jit helper, then we need to treat it like a call (but only
        // if there are/were any stack args).
        // This means unnesting, sorting, etc.  Technically this is overly
        // conservative, but I want to avoid as much special-case debug-only code
        // as possible, so leveraging the GTF_CALL flag is the easiest.
        //
        if (!treatLikeCall && (argx->gtFlags & GTF_EXCEPT) && (argCount > 1) && comp->opts.compDbgCode)
        {
            exceptionFlags = comp->gtCollectExceptions(argx);
            if ((exceptionFlags & (ExceptionSetFlags::IndexOutOfRangeException |
                                   ExceptionSetFlags::OverflowException)) != ExceptionSetFlags::None)
            {
                for (CallArg& otherArg : Args())
                {
                    if (&otherArg == &arg)
                    {
                        continue;
                    }

                    if (!otherArg.AbiInfo.HasAnyRegisterSegment())
                    {
                        treatLikeCall = true;
                        break;
                    }
                }
            }
        }
#endif // FEATURE_FIXED_OUT_ARGS

        // If it contains a call (GTF_CALL) then itself and everything before the call
        // with a GLOB_EFFECT must eval to temp (this is because everything with SIDE_EFFECT
        // has to be kept in the right order since we will move the call to the first position)

        // For calls we don't have to be quite as conservative as we are with stores
        // since the call won't be modifying any non-address taken LclVars.

        if (treatLikeCall)
        {
            if (canEvalToTemp)
            {
                if (argCount > 1) // If this is not the only argument
                {
                    SetNeedsTemp(&arg);
                }
                else if (varTypeIsFloating(argx->TypeGet()) && argx->OperIs(GT_CALL))
                {
                    // Spill all arguments that are floating point calls
                    SetNeedsTemp(&arg);
                }
            }

            // All previous arguments may need to be evaluated into temps
            for (CallArg& prevArg : Args())
            {
                if (&prevArg == &arg)
                {
                    break;
                }

#if !FEATURE_FIXED_OUT_ARGS
                if (!prevArg.AbiInfo.HasAnyRegisterSegment())
                {
                    // All stack args are already evaluated and placed in order
                    // in this case.
                    continue;
                }
#endif

                // For all previous arguments, if they have any GTF_ALL_EFFECT
                //  we require that they be evaluated into a temp
                if ((prevArg.GetEarlyNode() != nullptr) && ((prevArg.GetEarlyNode()->gtFlags & GTF_ALL_EFFECT) != 0))
                {
                    SetNeedsTemp(&prevArg);
                }
#if FEATURE_FIXED_OUT_ARGS
                // Or, if they are stored into the FIXED_OUT_ARG area
                // we require that they be moved to the late list
                else if (!prevArg.AbiInfo.HasAnyRegisterSegment())
                {
                    prevArg.m_needPlace = true;
                }
#if FEATURE_ARG_SPLIT
                else if (prevArg.AbiInfo.IsSplitAcrossRegistersAndStack())
                {
                    prevArg.m_needPlace = true;
                }
#endif // FEATURE_ARG_SPLIT
#endif
            }
        }
        else if ((argx->gtFlags & GTF_EXCEPT) != 0)
        {
            // If a previous arg may throw a different exception than this arg
            // then we evaluate all previous arguments with GTF_EXCEPT to temps
            // to avoid reordering them in our sort later.
            if (prevExceptionTree != nullptr)
            {
                if (prevExceptionFlags == ExceptionSetFlags::None)
                {
                    prevExceptionFlags = comp->gtCollectExceptions(prevExceptionTree);
                }

                if (exceptionFlags == ExceptionSetFlags::None)
                {
                    exceptionFlags = comp->gtCollectExceptions(argx);
                }

                bool exactlyOne       = isPow2(static_cast<unsigned>(exceptionFlags));
                bool throwsSameAsPrev = exactlyOne && (exceptionFlags == prevExceptionFlags);
                if (!throwsSameAsPrev)
                {
                    JITDUMP("Exception set for arg [%06u] interferes with previous tree [%06u]; must evaluate previous "
                            "trees with exceptions to temps\n",
                            Compiler::dspTreeID(argx), Compiler::dspTreeID(prevExceptionTree));

                    for (CallArg& prevArg : Args())
                    {
                        if (&prevArg == &arg)
                        {
                            break;
                        }

#if !FEATURE_FIXED_OUT_ARGS
                        if (!prevArg.AbiInfo.HasAnyRegisterSegment())
                        {
                            // All stack args are already evaluated and placed in order
                            // in this case.
                            continue;
                        }
#endif
                        // Invariant here is that all nodes that were not
                        // already evaluated into temps and that throw can only
                        // be throwing the same single exception as the
                        // previous tree, so all of them interfere in the same
                        // way with the current arg and must be evaluated
                        // early.
                        if ((prevArg.GetEarlyNode() != nullptr) &&
                            ((prevArg.GetEarlyNode()->gtFlags & GTF_EXCEPT) != 0))
                        {
                            SetNeedsTemp(&prevArg);
                        }
                    }
                }
            }

            prevExceptionTree  = argx;
            prevExceptionFlags = exceptionFlags;
        }
    }

#if FEATURE_FIXED_OUT_ARGS

    // For Arm/x64 we only care because we can't reorder a register
    // argument that uses GT_LCLHEAP.  This is an optimization to
    // save a check inside the below loop.
    //
    const bool hasStackArgsWeCareAbout = m_hasStackArgs && comp->compLocallocUsed;

#else

    const bool hasStackArgsWeCareAbout = m_hasStackArgs;

#endif // FEATURE_FIXED_OUT_ARGS

    // If we have any stack args we have to force the evaluation
    // of any arguments passed in registers that might throw an exception
    //
    // Technically we only a required to handle the following two cases:
    //     a GT_IND with GTF_IND_RNGCHK (only on x86) or
    //     a GT_LCLHEAP node that allocates stuff on the stack
    //
    if (hasStackArgsWeCareAbout)
    {
        for (CallArg& arg : EarlyArgs())
        {
            GenTree* argx = arg.GetEarlyNode();
            assert(!comp->gtTreeContainsOper(argx, GT_QMARK));

            // Examine the register args that are currently not marked needTmp
            //
            if (!arg.m_needTmp && arg.AbiInfo.HasAnyRegisterSegment())
            {
                if (hasStackArgsWeCareAbout)
                {
#if !FEATURE_FIXED_OUT_ARGS
                    // On x86 we previously recorded a stack depth of zero when
                    // morphing the register arguments of any GT_IND with a GTF_IND_RNGCHK flag
                    // Thus we can not reorder the argument after any stack based argument
                    // (Note that GT_LCLHEAP sets the GTF_EXCEPT flag so we don't need to
                    // check for it explicitly.)
                    //
                    if (argx->gtFlags & GTF_EXCEPT)
                    {
                        SetNeedsTemp(&arg);
                        continue;
                    }
#else
                    // For Arm/X64 we can't reorder a register argument that uses a GT_LCLHEAP
                    //
                    if (argx->gtFlags & GTF_EXCEPT)
                    {
                        assert(comp->compLocallocUsed);

                        if (comp->gtTreeContainsOper(argx, GT_LCLHEAP))
                        {
                            SetNeedsTemp(&arg);
                            continue;
                        }
                    }
#endif
                }
            }
        }
    }

    // When CFG is enabled and this is a delegate call or vtable call we must
    // compute the call target before all late args. However this will
    // effectively null-check 'this', which should happen only after all
    // arguments are evaluated. Thus we must evaluate all args with side
    // effects to a temp.
    if (comp->opts.IsCFGEnabled() && (call->IsVirtualVtable() || call->IsDelegateInvoke()))
    {
        // Always evaluate 'this' to temp.
        assert(HasThisPointer());
        SetNeedsTemp(GetThisArg());

        for (CallArg& arg : EarlyArgs())
        {
            if ((arg.GetEarlyNode()->gtFlags & GTF_ALL_EFFECT) != 0)
            {
                SetNeedsTemp(&arg);
            }
        }
    }

    m_argsComplete = true;
}

//------------------------------------------------------------------------
// SortArgs: Sort arguments into a better passing order.
//
// Parameters:
//   comp       - The compiler object.
//   call       - The call that contains this CallArgs instance.
//   sortedArgs - A table of at least `CountArgs()` entries where the sorted
//                arguments are written into.
//
void CallArgs::SortArgs(Compiler* comp, GenTreeCall* call, CallArg** sortedArgs)
{
    assert(m_argsComplete);

    JITDUMP("\nSorting the arguments:\n");

    // Shuffle the arguments around before we build the late args list. The
    // idea is to move all "simple" arguments like constants and local vars to
    // the end, and move the complex arguments towards the beginning. This will
    // help prevent registers from being spilled by allowing us to evaluate the
    // more complex arguments before the simpler arguments. We use the late
    // list to keep the sorted result at this point, and the ordering ends up
    // looking like:
    //     +------------------------------------+  <--- end of sortedArgs
    //     |          constants                 |
    //     +------------------------------------+
    //     |    local var / local field         |
    //     +------------------------------------+
    //     | remaining arguments sorted by cost |
    //     +------------------------------------+
    //     | temps (CallArg::m_needTmp == true) |
    //     +------------------------------------+
    //     |  args with calls (GTF_CALL)        |
    //     +------------------------------------+  <--- start of sortedArgs
    //

    unsigned argCount = 0;
    for (CallArg& arg : Args())
    {
        sortedArgs[argCount++] = &arg;
    }

    // Set the beginning and end for the new argument table
    unsigned curInx;
    int      regCount      = 0;
    unsigned begTab        = 0;
    unsigned endTab        = argCount - 1;
    unsigned argsRemaining = argCount;

    // First take care of arguments that are constants.
    // [We use a backward iterator pattern]
    //
    curInx = argCount;
    do
    {
        curInx--;

        CallArg* arg = sortedArgs[curInx];

        if (arg->AbiInfo.HasAnyRegisterSegment())
        {
            regCount++;
        }

        // Skip any already processed args
        //
        if (!arg->m_processed)
        {
            GenTree* argx = arg->GetEarlyNode();

            assert(argx != nullptr);
            // put constants at the end of the table
            //
            if (argx->OperIs(GT_CNS_INT))
            {
                noway_assert(curInx <= endTab);

                arg->m_processed = true;

                // place curArgTabEntry at the endTab position by performing a swap
                //
                if (curInx != endTab)
                {
                    sortedArgs[curInx] = sortedArgs[endTab];
                    sortedArgs[endTab] = arg;
                }

                endTab--;
                argsRemaining--;
            }
        }
    } while (curInx > 0);

    if (argsRemaining > 0)
    {
        // Next take care of arguments that are calls.
        // [We use a forward iterator pattern]
        //
        for (curInx = begTab; curInx <= endTab; curInx++)
        {
            CallArg* arg = sortedArgs[curInx];

            // Skip any already processed args
            //
            if (!arg->m_processed)
            {
                GenTree* argx = arg->GetEarlyNode();
                assert(argx != nullptr);

                // put calls at the beginning of the table
                //
                if (argx->gtFlags & GTF_CALL)
                {
                    arg->m_processed = true;

                    // place curArgTabEntry at the begTab position by performing a swap
                    //
                    if (curInx != begTab)
                    {
                        sortedArgs[curInx] = sortedArgs[begTab];
                        sortedArgs[begTab] = arg;
                    }

                    begTab++;
                    argsRemaining--;
                }
            }
        }
    }

    if (argsRemaining > 0)
    {
        // Next take care arguments that are temps.
        // These temps come before the arguments that are
        // ordinary local vars or local fields
        // since this will give them a better chance to become
        // enregistered into their actual argument register.
        // [We use a forward iterator pattern]
        //
        for (curInx = begTab; curInx <= endTab; curInx++)
        {
            CallArg* arg = sortedArgs[curInx];

            // Skip any already processed args
            //
            if (!arg->m_processed)
            {
                if (arg->m_needTmp)
                {
                    arg->m_processed = true;

                    // place curArgTabEntry at the begTab position by performing a swap
                    //
                    if (curInx != begTab)
                    {
                        sortedArgs[curInx] = sortedArgs[begTab];
                        sortedArgs[begTab] = arg;
                    }

                    begTab++;
                    argsRemaining--;
                }
            }
        }
    }

    if (argsRemaining > 0)
    {
        // Next take care of local var and local field arguments.
        // These are moved towards the end of the argument evaluation.
        // [We use a backward iterator pattern]
        //
        curInx = endTab + 1;
        do
        {
            curInx--;

            CallArg* arg = sortedArgs[curInx];

            // Skip any already processed args
            //
            if (!arg->m_processed)
            {
                GenTree* argx = arg->GetEarlyNode();
                assert(argx != nullptr);

                // As a CQ heuristic, sort TYP_STRUCT args using the cost estimation below.
                if (!argx->TypeIs(TYP_STRUCT) && argx->OperIs(GT_LCL_VAR, GT_LCL_FLD))
                {
                    noway_assert(curInx <= endTab);

                    arg->m_processed = true;

                    // place curArgTabEntry at the endTab position by performing a swap
                    //
                    if (curInx != endTab)
                    {
                        sortedArgs[curInx] = sortedArgs[endTab];
                        sortedArgs[endTab] = arg;
                    }

                    endTab--;
                    argsRemaining--;
                }
            }
        } while (curInx > begTab);
    }

    // Finally, take care of all the remaining arguments.
    // Note that we fill in one arg at a time using a while loop.
    bool costsPrepared = false; // Only prepare tree costs once, the first time through this loop
    while (argsRemaining > 0)
    {
        /* Find the most expensive arg remaining and evaluate it next */

        CallArg* expensiveArg      = nullptr;
        unsigned expensiveArgIndex = UINT_MAX;
        unsigned expensiveArgCost  = 0;

        // [We use a forward iterator pattern]
        //
        for (curInx = begTab; curInx <= endTab; curInx++)
        {
            CallArg* arg = sortedArgs[curInx];

            // Skip any already processed args
            //
            if (!arg->m_processed)
            {
                GenTree* argx = arg->GetEarlyNode();
                assert(argx != nullptr);

                // We should have already handled these kinds of args
                assert((!argx->OperIs(GT_LCL_VAR, GT_LCL_FLD) || argx->TypeIs(TYP_STRUCT)) &&
                       !argx->OperIs(GT_CNS_INT));

                // This arg should either have no persistent side effects or be the last one in our table
                // assert(((argx->gtFlags & GTF_PERSISTENT_SIDE_EFFECTS) == 0) || (curInx == (argCount-1)));

                if (argsRemaining == 1)
                {
                    // This is the last arg to place
                    expensiveArgIndex = curInx;
                    expensiveArg      = arg;
                    assert(begTab == endTab);
                    break;
                }
                else if (comp->opts.OptimizationEnabled())
                {
                    if (!costsPrepared)
                    {
                        /* We call gtPrepareCost to measure the cost of evaluating this tree */
                        comp->gtPrepareCost(argx);
                    }

                    if (argx->GetCostEx() > expensiveArgCost)
                    {
                        // Remember this arg as the most expensive one that we have yet seen
                        expensiveArgCost  = argx->GetCostEx();
                        expensiveArgIndex = curInx;
                        expensiveArg      = arg;
                    }
                }
                else
                {
                    // We don't have cost information in MinOpts
                    expensiveArgIndex = curInx;
                    expensiveArg      = arg;
                }
            }
        }

        noway_assert(expensiveArgIndex != UINT_MAX);

        // put the most expensive arg towards the beginning of the table

        expensiveArg->m_processed = true;

        // place expensiveArgTabEntry at the begTab position by performing a swap
        //
        if (expensiveArgIndex != begTab)
        {
            sortedArgs[expensiveArgIndex] = sortedArgs[begTab];
            sortedArgs[begTab]            = expensiveArg;
        }

        begTab++;
        argsRemaining--;

        costsPrepared = true; // If we have more expensive arguments, don't re-evaluate the tree cost on the next loop
    }

    // The table should now be completely filled and thus begTab should now be adjacent to endTab
    // and regArgsRemaining should be zero
    assert(begTab == (endTab + 1));
    assert(argsRemaining == 0);
}

//------------------------------------------------------------------------------
// EvalArgsToTemps: Handle arguments that were marked as requiring temps.
//
// Remarks:
//   This is the main function responsible for assigning late nodes in arguments.
//   After this function we may have the following shapes of early and late
//   nodes in arguments:
//   1. Early: GT_STORE_LCL_VAR, Late: GT_LCL_VAR.
//        When the argument needs to be evaluated early (e.g. because it has
//        side effects, or because it is a struct copy that requires it) it
//        will be assigned to a temp in the early node and passed as the local
//        in the late node. This can happen for both register and stack args.
//
//   2. Early: nullptr, Late: <any node>
//        All arguments that are placed in registers need to appear as a late
//        node. Some stack arguments may also require this pattern, for example
//        if a later argument trashes the outgoing arg area by requiring a
//        call.
//        If the argument does not otherwise need to be evaluated into a temp
//        we just move it into the late list.
//
//   3. Early: <any node>, Late: nullptr
//        Arguments that are passed on stack and that do not need an explicit
//        temp store in the early node list do not require any late node.
//
void CallArgs::EvalArgsToTemps(Compiler* comp, GenTreeCall* call)
{
    CallArg*  inlineTable[32];
    size_t    numArgs = call->gtArgs.CountArgs();
    CallArg** sortedArgs =
        numArgs <= ARRAY_SIZE(inlineTable) ? inlineTable : new (comp, CMK_CallArgs) CallArg*[numArgs];
    SortArgs(comp, call, sortedArgs);

    unsigned regArgInx = 0;
    // Now go through the sorted argument table and perform the necessary evaluation into temps.
    CallArg** lateTail = &m_lateHead;
    for (size_t i = 0; i < numArgs; i++)
    {
        CallArg& arg = *(sortedArgs[i]);

        if (arg.GetLateNode() != nullptr)
        {
            // We may already have created the temp as part of
            // fgMakeOutgoingStructArgCopy. In that case there is no work to be
            // done.
            *lateTail = &arg;
            lateTail  = &arg.LateNextRef();
            continue;
        }

        GenTree* argx = arg.GetEarlyNode();
        assert(argx != nullptr);

        GenTree* setupArg = nullptr;
        GenTree* defArg;

#if !FEATURE_FIXED_OUT_ARGS
        // Only ever set for FEATURE_FIXED_OUT_ARGS
        assert(!arg.m_needPlace);

        // On x86 and other archs that use push instructions to pass arguments:
        //   Only the register arguments need to be replaced with placeholder nodes.
        //   Stacked arguments are evaluated and pushed (or stored into the stack) in order.
        //
        if (!arg.AbiInfo.HasAnyRegisterSegment())
            continue;
#endif

        if (arg.m_needTmp)
        {
            // Create a temp store for the argument
            // Put the temp in the late arg list

#ifdef DEBUG
            if (comp->verbose)
            {
                printf("Argument with 'side effect'...\n");
                comp->gtDispTree(argx);
            }
#endif

            GenTree* argxEffectiveVal = argx->gtEffectiveVal();
            if (argxEffectiveVal->OperIs(GT_FIELD_LIST))
            {
                GenTreeFieldList* fieldList = argxEffectiveVal->AsFieldList();
                fieldList->gtFlags &= ~GTF_ALL_EFFECT;

                auto appendEffect = [=, &setupArg](GenTree* effect) {
                    if (setupArg == nullptr)
                    {
                        setupArg = effect;
                    }
                    else
                    {
                        setupArg = comp->gtNewOperNode(GT_COMMA, TYP_VOID, setupArg, effect);
                        setupArg->SetMorphed(comp);
                    }
                };

                for (GenTree* comma = argx; comma->OperIs(GT_COMMA); comma = comma->gtGetOp2())
                {
                    appendEffect(comma->gtGetOp1());
                }

                for (GenTreeFieldList::Use& use : fieldList->Uses())
                {
                    unsigned tmpVarNum = comp->lvaGrabTemp(true DEBUGARG("argument with side effect"));
                    GenTree* store     = comp->gtNewTempStore(tmpVarNum, use.GetNode());
                    store->SetMorphed(comp);

                    appendEffect(store);

                    GenTree* setupUse = comp->gtNewLclvNode(tmpVarNum, genActualType(use.GetNode()));
                    setupUse->SetMorphed(comp);
                    use.SetNode(setupUse);
                    fieldList->AddAllEffectsFlags(use.GetNode());
                }

                // Keep the field list in the late list
                defArg = fieldList;
            }
            else
            {
                unsigned tmpVarNum = comp->lvaGrabTemp(true DEBUGARG("argument with side effect"));

                setupArg = comp->gtNewTempStore(tmpVarNum, argx);
                setupArg->SetMorphed(comp, /* doChildren */ true);

                LclVarDsc* varDsc     = comp->lvaGetDesc(tmpVarNum);
                var_types  lclVarType = genActualType(argx->gtType);

                if (setupArg->OperIsCopyBlkOp())
                {
                    setupArg = comp->fgMorphCopyBlock(setupArg);
                }

                // Create a copy of the temp to go to the late argument list
                defArg = comp->gtNewLclvNode(tmpVarNum, lclVarType);
                defArg->SetMorphed(comp);
            }

#ifdef DEBUG
            if (comp->verbose)
            {
                printf("\n  Evaluate to a temp:\n");
                comp->gtDispTree(setupArg);
            }
#endif
        }
        else // curArgTabEntry->needTmp == false
        {
            //   On x86 -
            //      Only register args are replaced with placeholder nodes
            //      and the stack based arguments are evaluated and pushed in order.
            //
            //   On Arm/x64 - When needTmp is false and needPlace is false,
            //      the non-register arguments are evaluated and stored in order.
            //      When needPlace is true we have a nested call that comes after
            //      this argument so we have to replace it in the gtCallArgs list
            //      (the initial argument evaluation list) with a placeholder.
            //
            if (!arg.AbiInfo.HasAnyRegisterSegment() && !arg.m_needPlace)
            {
                continue;
            }

            // No temp needed - move the whole node to the late list

            defArg = argx;

#ifdef DEBUG
            if (comp->verbose)
            {
                if (arg.AbiInfo.HasAnyRegisterSegment())
                {
                    printf("Deferred argument:\n");
                }
                else
                {
                    printf("Deferred stack argument:\n");
                }

                comp->gtDispTree(argx);
                printf("Moved to late list\n");
            }
#endif

            arg.SetEarlyNode(nullptr);
        }

        if (setupArg != nullptr)
        {
            arg.SetEarlyNode(setupArg);
            call->gtFlags |= setupArg->gtFlags & GTF_SIDE_EFFECT;

            // Make sure we do not break recognition of retbuf-as-local
            // optimization here. If this is hit it indicates that we are
            // unnecessarily creating temps for some ret buf addresses, and
            // gtCallGetDefinedRetBufLclAddr relies on this not to happen.
            noway_assert((arg.GetWellKnownArg() != WellKnownArg::RetBuffer) || !call->IsOptimizingRetBufAsLocal());
        }

        arg.SetLateNode(defArg);
        *lateTail = &arg;
        lateTail  = &arg.LateNextRef();
    }

#ifdef DEBUG
    if (comp->verbose)
    {
        printf("\nRegister placement order:");
        for (CallArg& arg : LateArgs())
        {
            for (const ABIPassingSegment& segment : arg.AbiInfo.Segments())
            {
                if (segment.IsPassedInRegister())
                {
                    printf(" %s", getRegName(segment.GetRegister()));
                }
            }
        }
        printf("\n");
    }
#endif
}

//------------------------------------------------------------------------------
// SetNeedsTemp: Set the specified argument as requiring evaluation into a temp.
//
void CallArgs::SetNeedsTemp(CallArg* arg)
{
    arg->m_needTmp = true;
    m_needsTemps   = true;
}

//------------------------------------------------------------------------------
// fgMakeTemp: Make a temp variable and store 'value' into it.
//
// Arguments:
//    value - The expression to store to a temp.
//
// Return Value:
//    'TempInfo' data that contains the GT_STORE_LCL_VAR and GT_LCL_VAR nodes for
//    store and variable load respectively.
//
TempInfo Compiler::fgMakeTemp(GenTree* value)
{
    unsigned lclNum = lvaGrabTemp(true DEBUGARG("fgMakeTemp is creating a new local variable"));
    GenTree* store  = gtNewTempStore(lclNum, value);
    GenTree* load   = gtNewLclvNode(lclNum, genActualType(value));

    TempInfo tempInfo{};
    tempInfo.store = store;
    tempInfo.load  = load;

    return tempInfo;
}

//------------------------------------------------------------------------------
// fgMakeMultiUse : If the node is an unaliased local or constant clone it,
//    otherwise insert a comma form temp
//
// Arguments:
//    ppTree     - a pointer to the child node we will be replacing with the comma expression that
//                 evaluates ppTree to a temp and returns the result
//
// Return Value:
//    A fresh GT_LCL_VAR node referencing the temp which has not been used
//
// Notes:
//    This function will clone invariant nodes and locals, so this function
//    should only be used in situations where no interference between the
//    original use and new use is possible. Otherwise, fgInsertCommaFormTemp
//    should be used directly.
//
GenTree* Compiler::fgMakeMultiUse(GenTree** pOp)
{
    GenTree* const tree = *pOp;

    if (tree->IsInvariant() || tree->OperIsLocal())
    {
        return gtCloneExpr(tree);
    }

    return fgInsertCommaFormTemp(pOp);
}

//------------------------------------------------------------------------------
// fgInsertCommaFormTemp: Create a new temporary variable to hold the result of *ppTree,
//                        and replace *ppTree with comma(store<newLcl>(*ppTree)), newLcl)
//
// Arguments:
//    ppTree     - a pointer to the child node we will be replacing with the comma expression that
//                 evaluates ppTree to a temp and returns the result
//
// Return Value:
//    A fresh GT_LCL_VAR node referencing the temp which has not been used
//

GenTree* Compiler::fgInsertCommaFormTemp(GenTree** ppTree)
{
    GenTree* subTree = *ppTree;

    TempInfo tempInfo = fgMakeTemp(subTree);
    GenTree* store    = tempInfo.store;
    GenTree* load     = tempInfo.load;

    *ppTree = gtNewOperNode(GT_COMMA, subTree->TypeGet(), store, load);

    return gtClone(load);
}

//------------------------------------------------------------------------
// AddFinalArgsAndDetermineABIInfo:
//   Add final arguments and determine the argument ABI information.
//
// Parameters:
//   comp - The compiler object.
//   call - The call to which the CallArgs belongs.
//
// Remarks:
//   This adds the final "non-standard" arguments to the call and categorizes
//   all the ABI information required for downstream JIT phases. This function
//   modifies IR by adding certain non-standard arguments. For more information
//   see CallArg::IsArgAddedLate and CallArgs::ResetFinalArgsAndABIInfo.
//
void CallArgs::AddFinalArgsAndDetermineABIInfo(Compiler* comp, GenTreeCall* call)
{
    assert(&call->gtArgs == this);

    if (m_hasAddedFinalArgs)
    {
        return;
    }
    JITDUMP("Adding final args and determining ABI info for [%06u]:\n", Compiler::dspTreeID(call));

    m_hasRegArgs   = false;
    m_hasStackArgs = false;
    // At this point, we should not have any late args, as this needs to be done before those are determined.
    assert(m_lateHead == nullptr);

    if (TargetOS::IsUnix && IsVarArgs())
    {
        // Currently native varargs is not implemented on non windows targets.
        //
        // Note that some targets like Arm64 Unix should not need much work as
        // the ABI is the same. While other targets may only need small changes
        // such as amd64 Unix, which just expects RAX to pass numFPArguments.
        NYI("Morphing Vararg call not yet implemented on non Windows targets.");
    }

    // Insert or mark non-standard args. These are either outside the normal calling convention, or
    // arguments registers that don't follow the normal progression of argument registers in the calling
    // convention (such as for the ARM64 fixed return buffer argument x8).
    //
    // *********** NOTE *************
    // The logic here must remain in sync with GetNonStandardAddedArgCount(), which is used to map arguments
    // in the implementation of fast tail call.
    // *********** END NOTE *********

#if defined(TARGET_ARM)
    // A non-standard calling convention using wrapper delegate invoke is used on ARM, only, for wrapper
    // delegates. It is used for VSD delegate calls where the VSD custom calling convention ABI requires passing
    // R4, a callee-saved register, with a special value. Since R4 is a callee-saved register, its value needs
    // to be preserved. Thus, the VM uses a wrapper delegate IL stub, which preserves R4 and also sets up R4
    // correctly for the VSD call. The VM is simply reusing an existing mechanism (wrapper delegate IL stub)
    // to achieve its goal for delegate VSD call. See COMDelegate::NeedsWrapperDelegate() in the VM for details.
    if (call->gtCallMoreFlags & GTF_CALL_M_WRAPPER_DELEGATE_INV)
    {
        CallArg* thisArg = GetThisArg();
        assert((thisArg != nullptr) && (thisArg->GetEarlyNode() != nullptr));

        GenTree* cloned;
        if (thisArg->GetEarlyNode()->OperIsLocal())
        {
            cloned = comp->gtClone(thisArg->GetEarlyNode(), true);
        }
        else
        {
            cloned = comp->fgInsertCommaFormTemp(&thisArg->EarlyNodeRef());
            call->gtFlags |= GTF_ASG;
        }
        noway_assert(cloned != nullptr);

        GenTree* offsetNode = comp->gtNewIconNode(comp->eeGetEEInfo()->offsetOfWrapperDelegateIndirectCell, TYP_I_IMPL);
        GenTree* newArg     = comp->gtNewOperNode(GT_ADD, TYP_BYREF, cloned, offsetNode);

        newArg->SetMorphed(comp, /* doChildren */ true);

        // Append newArg as the last arg
        PushBack(comp, NewCallArg::Primitive(newArg).WellKnown(WellKnownArg::WrapperDelegateCell));
    }
#endif // defined(TARGET_ARM)

    bool addStubCellArg = true;

#ifdef TARGET_X86
    // TODO-X86-CQ: Currently RyuJIT/x86 passes args on the stack, so this is not needed.
    // If/when we change that, the following code needs to be changed to correctly support the (TBD) managed calling
    // convention for x86/SSE.

    addStubCellArg = comp->IsTargetAbi(CORINFO_NATIVEAOT_ABI);
#endif

    // We are allowed to have a ret buffer argument combined
    // with any of the remaining non-standard arguments

    if (call->IsVirtualStub() && addStubCellArg)
    {
        if (!call->IsTailCallViaJitHelper())
        {
            GenTree* stubAddrArg = comp->fgGetStubAddrArg(call);
            // And push the stub address onto the list of arguments
            NewCallArg stubAddrNewArg = NewCallArg::Primitive(stubAddrArg).WellKnown(WellKnownArg::VirtualStubCell);
            InsertAfterThisOrFirst(comp, stubAddrNewArg);
        }
        else
        {
            // If it is a VSD call getting dispatched via tail call helper,
            // fgMorphTailCallViaJitHelper() would materialize stub addr as an additional
            // parameter added to the original arg list and hence no need to
            // add as a non-standard arg.
        }
    }
    else if ((call->gtCallType == CT_INDIRECT) && !call->IsVirtualStub() && (call->gtCallCookie != nullptr))
    {
        assert(!call->IsUnmanaged());

        GenTree* arg = call->gtCallCookie;
        noway_assert(arg != nullptr);
        call->gtCallCookie = nullptr;

        // All architectures pass the cookie in a register.
        InsertAfterThisOrFirst(comp, NewCallArg::Primitive(arg).WellKnown(WellKnownArg::PInvokeCookie));
        // put destination into R10/EAX
        arg = comp->gtClone(call->gtCallAddr, true);
        InsertAfterThisOrFirst(comp, NewCallArg::Primitive(arg).WellKnown(WellKnownArg::PInvokeTarget));

        // finally change this call to a helper call
        call->gtCallType    = CT_HELPER;
        call->gtCallMethHnd = comp->eeFindHelper(CORINFO_HELP_PINVOKE_CALLI);
    }
#if defined(FEATURE_READYTORUN)
    // For arm/arm64, we dispatch code same as VSD using virtualStubParamInfo->GetReg()
    // for indirection cell address, which ZapIndirectHelperThunk expects.
    // For x64/x86 we use return address to get the indirection cell by disassembling the call site.
    // That is not possible for fast tailcalls, so we only need this logic for fast tailcalls on xarch.
    // Note that we call this before we know if something will be a fast tailcall or not.
    // That's ok; after making something a tailcall, we will invalidate this information
    // and reconstruct it if necessary. The tailcalling decision does not change since
    // this is a non-standard arg in a register.
    bool needsIndirectionCell = call->IsR2RRelativeIndir() && !call->IsDelegateInvoke();
#if defined(TARGET_XARCH)
    needsIndirectionCell &= call->IsFastTailCall();
#endif

    if (needsIndirectionCell)
    {
        assert(call->gtEntryPoint.addr != nullptr);

        size_t   addrValue           = (size_t)call->gtEntryPoint.addr;
        GenTree* indirectCellAddress = comp->gtNewIconHandleNode(addrValue, GTF_ICON_FTN_ADDR);
        INDEBUG(indirectCellAddress->AsIntCon()->gtTargetHandle = (size_t)call->gtCallMethHnd);

#ifdef TARGET_ARM
        // TODO-ARM: We currently do not properly kill this register in LSRA
        // (see getKillSetForCall which does so only for VSD calls).
        // We should be able to remove these two workarounds once we do so,
        // however when this was tried there were significant regressions.
        indirectCellAddress->SetRegNum(REG_R2R_INDIRECT_PARAM);
        indirectCellAddress->SetDoNotCSE();
#endif

        // Push the stub address onto the list of arguments.
        NewCallArg indirCellAddrArg =
            NewCallArg::Primitive(indirectCellAddress).WellKnown(WellKnownArg::R2RIndirectionCell);
        InsertAfterThisOrFirst(comp, indirCellAddrArg);
    }
#endif

    ClassifierInfo info;
    info.CallConv = call->GetUnmanagedCallConv();
    // X86 tailcall helper is considered varargs, but not for ABI classification purposes.
    info.IsVarArgs  = call->IsVarargs() && !call->IsTailCallViaJitHelper();
    info.HasThis    = call->gtArgs.HasThisPointer();
    info.HasRetBuff = call->gtArgs.HasRetBuffer();
    PlatformClassifier classifier(info);

    // Morph the user arguments
    for (CallArg& arg : Args())
    {
        assert(arg.GetEarlyNode() != nullptr);
        GenTree* argx = arg.GetEarlyNode();

        // TODO-Cleanup: this is duplicative with the code in args morphing, however, also kicks in for
        // "non-standard" (return buffer on ARM64) arguments. Fix args morphing and delete this code.
        if (argx->OperIs(GT_LCL_ADDR))
        {
            argx->gtType = TYP_I_IMPL;
        }

        // Note we must use the signature types for making ABI decisions. This is especially important for structs,
        // where the "argx" node can legally have a type that is not ABI-compatible with the one in the signature.
        const var_types            argSigType  = arg.GetSignatureType();
        const CORINFO_CLASS_HANDLE argSigClass = arg.GetSignatureClassHandle();
        ClassLayout* argLayout = argSigClass == NO_CLASS_HANDLE ? nullptr : comp->typGetObjLayout(argSigClass);

        // Some well known args have custom register assignment.
        // These should not affect the placement of any other args or stack space required.
        // Example: on AMD64 R10 and R11 are used for indirect VSD (generic interface) and cookie calls.
        // TODO-Cleanup: Integrate this into the new style ABI classifiers.
        regNumber nonStdRegNum = GetCustomRegister(comp, call->GetUnmanagedCallConv(), arg.GetWellKnownArg());

        if (nonStdRegNum == REG_NA)
        {
            if (arg.GetWellKnownArg() == WellKnownArg::AsyncSuspendedIndicator)
            {
                // Represents definition of a local. Expanded out by async transformation.
                arg.AbiInfo = ABIPassingInformation(comp, 0);
            }
            else
            {
                arg.AbiInfo = classifier.Classify(comp, argSigType, argLayout, arg.GetWellKnownArg());
            }
        }
        else
        {
            ABIPassingSegment segment = ABIPassingSegment::InRegister(nonStdRegNum, 0, TARGET_POINTER_SIZE);
            arg.AbiInfo               = ABIPassingInformation::FromSegmentByValue(comp, segment);
        }

        JITDUMP("Argument %u ABI info: ", GetIndex(&arg));
        DBEXEC(VERBOSE, arg.AbiInfo.Dump());

        for (const ABIPassingSegment& segment : arg.AbiInfo.Segments())
        {
            if (segment.IsPassedOnStack())
            {
                m_hasStackArgs = true;
            }
            else
            {
                m_hasRegArgs = true;
                comp->compFloatingPointUsed |= genIsValidFloatReg(segment.GetRegister());
            }
        }
    }

    m_argsStackSize = classifier.StackSize();

#ifdef DEBUG
    if (VERBOSE)
    {
        JITDUMP("Args for call [%06u] %s after AddFinalArgsAndDetermineABIInfo:\n", comp->dspTreeID(call),
                GenTree::OpName(call->gtOper));
        for (CallArg& arg : Args())
        {
            arg.Dump(comp);
        }
        JITDUMP("\n");
    }
#endif

    m_abiInformationDetermined = true;
    m_hasAddedFinalArgs        = true;
}

//------------------------------------------------------------------------
// DetermineNewABIInfo:
//   Determine the new ABI info for all call args without making any IR
//   changes.
//
// Parameters:
//   comp - The compiler object.
//   call - The call to which the CallArgs belongs.
//
void CallArgs::DetermineABIInfo(Compiler* comp, GenTreeCall* call)
{
    ClassifierInfo info;
    info.CallConv = call->GetUnmanagedCallConv();
    // X86 tailcall helper is considered varargs, but not for ABI classification purposes.
    info.IsVarArgs  = call->IsVarargs() && !call->IsTailCallViaJitHelper();
    info.HasThis    = call->gtArgs.HasThisPointer();
    info.HasRetBuff = call->gtArgs.HasRetBuffer();
    PlatformClassifier classifier(info);

    for (CallArg& arg : Args())
    {
        if (arg.GetWellKnownArg() == WellKnownArg::AsyncSuspendedIndicator)
        {
            // Represents definition of a local. Expanded out by async transformation.
            arg.AbiInfo = ABIPassingInformation(comp, 0);
            continue;
        }

        const var_types            argSigType  = arg.GetSignatureType();
        const CORINFO_CLASS_HANDLE argSigClass = arg.GetSignatureClassHandle();
        ClassLayout* argLayout = argSigClass == NO_CLASS_HANDLE ? nullptr : comp->typGetObjLayout(argSigClass);

        // Some well known args have custom register assignment.
        // These should not affect the placement of any other args or stack space required.
        // Example: on AMD64 R10 and R11 are used for indirect VSD (generic interface) and cookie calls.
        // TODO-Cleanup: Integrate this into the new style ABI classifiers.
        regNumber nonStdRegNum = GetCustomRegister(comp, call->GetUnmanagedCallConv(), arg.GetWellKnownArg());

        if (nonStdRegNum == REG_NA)
        {
            arg.AbiInfo = classifier.Classify(comp, argSigType, argLayout, arg.GetWellKnownArg());
        }
        else
        {
            ABIPassingSegment segment = ABIPassingSegment::InRegister(nonStdRegNum, 0, TARGET_POINTER_SIZE);
            arg.AbiInfo               = ABIPassingInformation::FromSegmentByValue(comp, segment);
        }
    }

    m_argsStackSize            = classifier.StackSize();
    m_abiInformationDetermined = true;
}

//------------------------------------------------------------------------
// OutgoingArgsStackSize:
//   Compute the number of bytes allocated on the stack for arguments to this call.
//
// Remarks:
//   Note that even with no arguments, some ABIs may still allocate stack
//   space, which will be returned by this function.
//
unsigned CallArgs::OutgoingArgsStackSize() const
{
    unsigned aligned = Compiler::GetOutgoingArgByteSize(m_argsStackSize);
    return max(aligned, (unsigned)MIN_ARG_AREA_FOR_CALL);
}

//------------------------------------------------------------------------
// CountArgs: Count the number of arguments.
//
unsigned CallArgs::CountArgs()
{
    unsigned numArgs = 0;
    for (CallArg& arg : Args())
    {
        numArgs++;
    }

    return numArgs;
}

//------------------------------------------------------------------------
// CountArgs: Count the number of arguments ignoring non-user ones, e.g.
//    r2r cell argument in a user function.
//
// Remarks:
//   See IsUserArg's comments
//
unsigned CallArgs::CountUserArgs()
{
    unsigned numArgs = 0;
    for (CallArg& arg : Args())
    {
        if (arg.IsUserArg())
        {
            numArgs++;
        }
    }
    return numArgs;
}

//------------------------------------------------------------------------
// fgMorphArgs: Walk and transform (morph) the arguments of a call
//
// Arguments:
//    call - the call for which we are doing the argument morphing
//
// Return Value:
//    Like most morph methods, this method returns the morphed node,
//    though in this case there are currently no scenarios where the
//    node itself is re-created.
//
// Notes:
//    This calls CallArgs::AddFinalArgsAndDetermineABIInfo to determine ABI
//    information for the call. If it has already been determined, that method
//    will simply return.
//
//    This method changes the state of the call node. It may be called even
//    after it has already done the first round of morphing.
//
//    The first time it is called (i.e. during global morphing), this method
//    computes the "late arguments". This is when it determines which arguments
//    need to be evaluated to temps prior to the main argument setup, and which
//    can be directly evaluated into the argument location. It also creates a
//    second argument list (the late args) that does the final placement of the
//    arguments, e.g. into registers or onto the stack.
//
//    The "non-late arguments", are doing the in-order evaluation of the
//    arguments that might have side-effects, such as embedded stores, calls
//    or possible throws. In these cases, it and earlier arguments must be
//    evaluated to temps.
//
//    On targets with a fixed outgoing argument area (FEATURE_FIXED_OUT_ARGS),
//    if we have any nested calls, we need to defer the copying of the argument
//    into the fixed argument area until after the call. If the argument did
//    not otherwise need to be computed into a temp, it is moved to late
//    argument and replaced in the "early" arg list with a placeholder node.
//    Also see `CallArgs::EvalArgsToTemps`.
//
GenTreeCall* Compiler::fgMorphArgs(GenTreeCall* call)
{
    GenTreeFlags flagsSummary = GTF_EMPTY;

    bool reMorphing = call->gtArgs.AreArgsComplete();

    call->gtArgs.AddFinalArgsAndDetermineABIInfo(this, call);
    JITDUMP("%sMorphing args for %d.%s:\n", (reMorphing) ? "Re" : "", call->gtTreeID, GenTree::OpName(call->gtOper));

    // If we are remorphing, process the late arguments (which were determined by a previous caller).
    if (reMorphing)
    {
        for (CallArg& arg : call->gtArgs.LateArgs())
        {
            arg.SetLateNode(fgMorphTree(arg.GetLateNode()));
            flagsSummary |= arg.GetLateNode()->gtFlags;
        }
    }

    // First we morph the argument subtrees ('this' pointer, arguments, etc.).
    // During the first call to fgMorphArgs we also record the
    // information about late arguments in CallArgs.
    // This information is used later to construct the late args

    for (CallArg& arg : call->gtArgs.Args())
    {
        GenTree** parentArgx = &arg.EarlyNodeRef();

        // Morph the arg node and update the node pointer.
        GenTree* argx = *parentArgx;
        if (argx == nullptr)
        {
            // Skip node that was moved to late args during remorphing, no work to be done.
            assert(reMorphing);
            continue;
        }

        argx        = fgMorphTree(argx);
        *parentArgx = argx;

        if (arg.GetWellKnownArg() == WellKnownArg::ThisPointer)
        {
            // We may want to force 'this' into a temp because we want to use
            // it to expand the call target in morph so that CSE can pick it
            // up.
            if (!reMorphing && call->IsExpandedEarly() && call->IsVirtualVtable() && !argx->OperIsLocal())
            {
                call->gtArgs.SetNeedsTemp(&arg);
            }
        }

        // For pointers to locals we can skip reporting GC info and also skip zero initialization.
        // NOTE: We deferred this from the importer because of the inliner.
        if (argx->OperIs(GT_LCL_ADDR))
        {
            argx->gtType = TYP_I_IMPL;
        }

        if (varTypeIsStruct(arg.GetSignatureType()) && !reMorphing)
        {
            bool makeOutArgCopy = false;
            if (arg.AbiInfo.IsPassedByReference())
            {
                makeOutArgCopy = true;
            }
            else if (fgTryMorphStructArg(&arg))
            {
                argx = *parentArgx;
            }
            else
            {
                makeOutArgCopy = true;
            }

            if (makeOutArgCopy)
            {
                fgMakeOutgoingStructArgCopy(call, &arg);

                if (arg.GetEarlyNode() != nullptr)
                {
                    flagsSummary |= arg.GetEarlyNode()->gtFlags;
                }
            }
        }

        flagsSummary |= arg.GetEarlyNode()->gtFlags;

    } // end foreach argument loop

    if (!reMorphing)
    {
        call->gtArgs.ArgsComplete(this, call);
    }

    // Process the function address, if indirect call

    if (call->gtCallType == CT_INDIRECT)
    {
        call->gtCallAddr = fgMorphTree(call->gtCallAddr);
        // Const CSE may create a store node here
        flagsSummary |= call->gtCallAddr->gtFlags;
    }

#if FEATURE_FIXED_OUT_ARGS && defined(UNIX_AMD64_ABI)
    if (!call->IsFastTailCall())
    {
        // This is currently required for the UNIX ABI to work correctly.
        opts.compNeedToAlignFrame = true;
    }
#endif // FEATURE_FIXED_OUT_ARGS && UNIX_AMD64_ABI

    // Clear the ASG and EXCEPT (if possible) flags on the call node
    call->gtFlags &= ~GTF_ASG;
    if (!call->OperMayThrow(this))
    {
        call->gtFlags &= ~GTF_EXCEPT;
    }

    // Union in the side effect flags from the call's operands
    call->gtFlags |= flagsSummary & GTF_ALL_EFFECT;

    // If we are remorphing or don't have any register arguments or other arguments that need
    // temps, then we don't need to call SortArgs() and EvalArgsToTemps().
    //
    if (!reMorphing && (call->gtArgs.HasRegArgs() || call->gtArgs.NeedsTemps()))
    {
        // Do the 'defer or eval to temp' analysis.
        call->gtArgs.EvalArgsToTemps(this, call);
    }

#ifdef DEBUG
    if (verbose)
    {
        JITDUMP("Args for [%06u].%s after fgMorphArgs:\n", dspTreeID(call), GenTree::OpName(call->gtOper));
        for (CallArg& arg : call->gtArgs.Args())
        {
            arg.Dump(this);
        }
        printf("OutgoingArgsStackSize is %u\n\n", call->gtArgs.OutgoingArgsStackSize());
    }
#endif
    return call;
}

//-----------------------------------------------------------------------------
// fgTryMorphStructArg:
//   Given a varTypeIsStruct argument, try to morph it into a shape that the
//   backend supports.
//
// Arguments:
//   arg - The argument
//
// Returns:
//   False if the argument cannot be put into a shape supported by the backend.
//
// Remarks:
//   The backend requires register-passed arguments to be of FIELD_LIST shape.
//   For split arguments it is additionally required that registers and stack
//   slots have clean mappings to fields.
//   For stack-passed arguments the backend supports struct-typed arguments
//   directly.
//
bool Compiler::fgTryMorphStructArg(CallArg* arg)
{
    GenTree** use     = GenTree::EffectiveUse(&arg->NodeRef());
    GenTree*  argNode = *use;
    assert(varTypeIsStruct(argNode));

    bool isSplit = arg->AbiInfo.IsSplitAcrossRegistersAndStack();
#ifdef TARGET_ARM
    if ((isSplit && (arg->AbiInfo.CountRegsAndStackSlots() > 4)) || (!isSplit && arg->AbiInfo.HasAnyStackSegment()))
#else
    if (!arg->AbiInfo.HasAnyRegisterSegment())
#endif
    {
        if (argNode->OperIs(GT_LCL_VAR) &&
            (lvaGetPromotionType(argNode->AsLclVar()->GetLclNum()) == PROMOTION_TYPE_INDEPENDENT))
        {
            // TODO-Arm-CQ: support decomposing "large" promoted structs into field lists.
            if (!isSplit)
            {
                GenTreeFieldList* fieldList = fgMorphLclToFieldList(argNode->AsLclVar());
                // TODO-Cleanup: The containment/reg optionality for x86 is
                // conservative in the "no field list" case.
#ifdef TARGET_X86
                *use = fieldList;
#else
                *use = fieldList->SoleFieldOrThis();
#endif
                *use = fgMorphTree(*use);
            }
            else
            {
                // Set DNER to block independent promotion.
                lvaSetVarDoNotEnregister(argNode->AsLclVar()->GetLclNum() DEBUGARG(DoNotEnregisterReason::IsStructArg));
            }
        }
        else if (argNode->OperIs(GT_LCL_FLD))
        {
            lvaSetVarDoNotEnregister(argNode->AsLclFld()->GetLclNum() DEBUGARG(DoNotEnregisterReason::LocalField));
        }
        else if (argNode->OperIs(GT_BLK))
        {
            ClassLayout* layout = argNode->AsBlk()->GetLayout();

            var_types primitiveType = layout->GetRegisterType();
            if (primitiveType != TYP_UNDEF)
            {
                JITDUMP("Converting argument [%06u] to primitive indirection\n", dspTreeID(argNode));

                argNode->SetOper(GT_IND);
                argNode->gtType = primitiveType;
            }
        }

        // Potentially update commas
        arg->GetNode()->ChangeType((*use)->TypeGet());
        return true;
    }

    GenTree* newArg = nullptr;

    if (argNode->OperIs(GT_LCL_VAR))
    {
        GenTreeLclVar* lclNode = argNode->AsLclVar();
        unsigned       lclNum  = lclNode->GetLclNum();
        LclVarDsc*     varDsc  = lvaGetDesc(lclNum);

        if (!arg->AbiInfo.HasExactlyOneRegisterSegment())
        {
            varDsc->lvIsMultiRegArg = true;
        }

        JITDUMP("Struct argument V%02u: ", lclNum);
        JITDUMPEXEC(arg->Dump(this));

        // Try to see if we can and should use promoted fields to pass this
        // argument.
        //
        if (varDsc->lvPromoted && !varDsc->lvDoNotEnregister && (!isSplit || FieldsMatchAbi(varDsc, arg->AbiInfo)))
        {
            newArg = fgMorphLclToFieldList(lclNode)->SoleFieldOrThis();
            newArg = fgMorphTree(newArg);
        }
    }
    else if (argNode->OperIsFieldList())
    {
        // We can already see a field list here if physical promotion created it.
        // Physical promotion will also create single-field field lists which
        // not everything treats the same as a single node, so fix that here.
        newArg = argNode->AsFieldList()->SoleFieldOrThis();
        if (newArg == argNode)
        {
            return true;
        }
    }

    // If we were not able to use the promoted fields...
    //
    if (newArg == nullptr)
    {
        if (!argNode->TypeIs(TYP_STRUCT) && arg->AbiInfo.HasExactlyOneRegisterSegment())
        {
            // This can be treated primitively. Leave it alone.
            return true;
        }

        if (!argNode->OperIsLocalRead() && !argNode->OperIsLoad())
        {
            // A node we do not know how to turn into multiple registers.
            // Usually HWINTRINSIC. Bail.
            return false;
        }

        ClassLayout* layout     = argNode->TypeIs(TYP_STRUCT) ? argNode->GetLayout(this) : nullptr;
        unsigned     structSize = argNode->TypeIs(TYP_STRUCT) ? layout->GetSize() : genTypeSize(argNode);

        if (layout != nullptr)
        {
            assert(ClassLayout::AreCompatible(typGetObjLayout(arg->GetSignatureClassHandle()), layout));
        }
        else
        {
            assert(varTypeIsSIMD(argNode) && varTypeIsSIMD(arg->GetSignatureType()));
        }

        if (argNode->OperIsLoad())
        {
            unsigned lastLoadSize = structSize % TARGET_POINTER_SIZE;
            if ((lastLoadSize != 0) && !isPow2(lastLoadSize))
            {
                // Cannot read this size from a non-local. Bail.
                return false;
            }

            GenTree* indirAddr = argNode->AsIndir()->Addr();
            if (((indirAddr->gtFlags & GTF_PERSISTENT_SIDE_EFFECTS) != 0) &&
                (arg->AbiInfo.CountRegsAndStackSlots() > 1))
            {
                // Cannot create multiple uses of the address. Bail.
                return false;
            }
        }

        auto createSlotAccess = [=](unsigned offset, var_types type) -> GenTree* {
            assert(offset < structSize);

            if (type == TYP_UNDEF)
            {
                unsigned sizeLeft = structSize - offset;
                if (sizeLeft < TARGET_POINTER_SIZE)
                {
                    switch (sizeLeft)
                    {
                        case 1:
                            type = TYP_UBYTE;
                            break;
                        case 2:
                            type = TYP_USHORT;
                            break;
                        case 3:
                        case 4:
                            type = TYP_INT;
                            break;
                        case 5:
                        case 6:
                        case 7:
                        case 8:
                            type = TYP_LONG;
                            break;
                        default:
                            unreached();
                    }

#ifdef TARGET_ARM64
                    if ((offset > 0) && argNode->OperIsLocalRead())
                    {
                        // For arm64 it's beneficial to consider all tails to
                        // be TYP_I_IMPL to allow more ldp's.
                        type = TYP_I_IMPL;
                    }
#endif
                }
                else if ((layout != nullptr) && ((offset % TARGET_POINTER_SIZE) == 0))
                {
                    type = layout->GetGCPtrType(offset / TARGET_POINTER_SIZE);
                }
                else
                {
                    type = TYP_I_IMPL;
                }
            }

            if (argNode->OperIsLocalRead())
            {
                GenTreeLclVarCommon* lclVar = argNode->AsLclVarCommon();
                LclVarDsc*           dsc    = lvaGetDesc(lclVar);
                GenTree*             result;
                // We sometimes end up with struct reinterpretations where the
                // retyping into a primitive allows us to replace by a scalar
                // local here, so make sure we do that if possible.
                if ((lclVar->GetLclOffs() == 0) && (offset == 0) && (genTypeSize(type) == genTypeSize(dsc)))
                {
                    result = gtNewLclVarNode(lclVar->GetLclNum());
                }
                else
                {
                    result = gtNewLclFldNode(lclVar->GetLclNum(), type, lclVar->GetLclOffs() + offset);

                    if (!dsc->lvDoNotEnregister)
                    {
                        lvaSetVarDoNotEnregister(lclVar->GetLclNum() DEBUGARG(DoNotEnregisterReason::LocalField));
                    }
                }
                result = fgMorphTree(result);
                return result;
            }
            else
            {
                assert(argNode->OperIsLoad());
                GenTree* indirAddr = argNode->AsIndir()->Addr();
                GenTree* addr;

                if (offset == 0)
                {
                    addr = indirAddr;
                }
                else
                {
                    GenTree* indirAddrDup = gtCloneExpr(indirAddr);
                    GenTree* offsetNode   = gtNewIconNode(offset, TYP_I_IMPL);
                    addr                  = gtNewOperNode(GT_ADD, indirAddr->TypeGet(), indirAddrDup, offsetNode);
                }

                GenTree* indir = gtNewIndir(type, addr);
                indir->SetMorphed(this, /* doChildren */ true);
                return indir;
            }
        };

        newArg = new (this, GT_FIELD_LIST) GenTreeFieldList();
        newArg->SetMorphed(this);

        for (const ABIPassingSegment& seg : arg->AbiInfo.Segments())
        {
            if (seg.IsPassedInRegister())
            {
                var_types regType = seg.GetRegisterType();
                // If passed in a float reg then keep that type; otherwise let
                // createSlotAccess get the type from the layout.
                var_types slotType = varTypeUsesFloatReg(regType) ? regType : TYP_UNDEF;
                GenTree*  access   = createSlotAccess(seg.Offset, slotType);

                newArg->AsFieldList()->AddField(this, access, seg.Offset, access->TypeGet());
            }
            else
            {
                for (unsigned slotOffset = 0; slotOffset < seg.Size; slotOffset += TARGET_POINTER_SIZE)
                {
                    unsigned layoutOffset = seg.Offset + slotOffset;
                    GenTree* access       = createSlotAccess(layoutOffset, TYP_UNDEF);

                    newArg->AsFieldList()->AddField(this, access, layoutOffset, access->TypeGet());
                }
            }
        }

        newArg = newArg->AsFieldList()->SoleFieldOrThis();
    }

    JITDUMP("fgTryMorphStructArg created tree:\n");
    DISPTREE(newArg);

    *use = newArg;
    // Potentially update commas
    arg->GetNode()->ChangeType((*use)->TypeGet());
    return true;
}

//-----------------------------------------------------------------------------
// FieldsMatchAbi:
//   Check if the fields of a local map cleanly (in terms of offsets) to the
//   specified ABI info.
//
// Arguments:
//   varDsc  - promoted local
//   abiInfo - ABI information
//
// Returns:
//   True if it does. In that case FIELD_LIST usage is allowed for split args
//   by the backend.
//
bool Compiler::FieldsMatchAbi(LclVarDsc* varDsc, const ABIPassingInformation& abiInfo)
{
    if (varDsc->lvFieldCnt != abiInfo.CountRegsAndStackSlots())
    {
        return false;
    }

    for (const ABIPassingSegment& seg : abiInfo.Segments())
    {
        if (seg.IsPassedInRegister())
        {
            unsigned fieldLclNum = lvaGetFieldLocal(varDsc, seg.Offset);
            if (fieldLclNum == BAD_VAR_NUM)
            {
                return false;
            }
        }
        else
        {
            for (unsigned offset = 0; offset < seg.Size; offset += TARGET_POINTER_SIZE)
            {
                if (lvaGetFieldLocal(varDsc, seg.Offset + offset) == BAD_VAR_NUM)
                {
                    return false;
                }
            }
        }
    }

    return true;
}

//------------------------------------------------------------------------
// fgMorphLclToFieldList: Morph a GT_LCL_VAR node to a GT_FIELD_LIST of its promoted fields
//
// Arguments:
//    lcl  - The GT_LCL_VAR node we will transform
//
// Return value:
//    The new GT_FIELD_LIST that we have created.
//
GenTreeFieldList* Compiler::fgMorphLclToFieldList(GenTreeLclVar* lcl)
{
    LclVarDsc* varDsc = lvaGetDesc(lcl);
    assert(varDsc->lvPromoted);
    unsigned fieldCount  = varDsc->lvFieldCnt;
    unsigned fieldLclNum = varDsc->lvFieldLclStart;

    GenTreeFieldList* fieldList = new (this, GT_FIELD_LIST) GenTreeFieldList();

    for (unsigned i = 0; i < fieldCount; i++)
    {
        LclVarDsc* fieldVarDsc = lvaGetDesc(fieldLclNum);
        GenTree*   lclVar      = gtNewLclvNode(fieldLclNum, fieldVarDsc->TypeGet());
        fieldList->AddField(this, lclVar, fieldVarDsc->lvFldOffset, fieldVarDsc->TypeGet());
        fieldLclNum++;
    }

    return fieldList;
}

//------------------------------------------------------------------------
// fgMakeOutgoingStructArgCopy: make a copy of a struct variable if necessary,
//   to pass to a callee.
//
// Arguments:
//    call - call being processed
//    arg - arg for the call
//
// The arg is updated if necessary with the copy.
//
void Compiler::fgMakeOutgoingStructArgCopy(GenTreeCall* call, CallArg* arg)
{
    GenTree* argx = arg->GetEarlyNode();

#if FEATURE_IMPLICIT_BYREFS
    // If we're optimizing, see if we can avoid making a copy.
    //
    // We don't need a copy if this is the last use of the local.
    //
    if (opts.OptimizationEnabled() && arg->AbiInfo.IsPassedByReference())
    {
        GenTree*             implicitByRefLclAddr;
        target_ssize_t       implicitByRefLclOffs;
        GenTreeLclVarCommon* implicitByRefLcl =
            argx->IsImplicitByrefParameterValuePostMorph(this, &implicitByRefLclAddr, &implicitByRefLclOffs);

        GenTreeLclVarCommon* lcl = implicitByRefLcl;
        if ((lcl == nullptr) && argx->OperIsLocal())
        {
            lcl                  = argx->AsLclVarCommon();
            implicitByRefLclOffs = lcl->GetLclOffs();
        }

        if (lcl != nullptr)
        {
            const unsigned   varNum = lcl->GetLclNum();
            LclVarDsc* const varDsc = lvaGetDesc(varNum);

            // We generally use liveness to figure out if we can omit creating
            // this copy. However, even without liveness (e.g. due to too many
            // tracked locals), we also handle some other cases:
            //
            // * (must not copy) If the call is a tail call, the use is a last use.
            //   We must skip the copy if we have a fast tail call.
            //
            // * (may not copy) if the call is noreturn, the use is a last use.
            //   We also check for just one reference here as we are not doing
            //   alias analysis of the call's parameters, or checking if the call
            //   site is not within some try region.
            //
            bool omitCopy = call->IsTailCall();

            if (!omitCopy && fgGlobalMorph)
            {
                omitCopy = (varDsc->lvIsLastUseCopyOmissionCandidate || (implicitByRefLcl != nullptr)) &&
                           !varDsc->lvPromoted && !varDsc->lvIsStructField && ((lcl->gtFlags & GTF_VAR_DEATH) != 0);
            }

            // Disallow the argument from potentially aliasing the return
            // buffer.
            if (omitCopy)
            {
                GenTreeLclVarCommon* retBuffer = gtCallGetDefinedRetBufLclAddr(call);
                if ((retBuffer != nullptr) && (retBuffer->GetLclNum() == varNum))
                {
                    unsigned       retBufferSize  = typGetObjLayout(call->gtRetClsHnd)->GetSize();
                    target_ssize_t retBufferStart = retBuffer->GetLclOffs();
                    target_ssize_t retBufferEnd   = retBufferStart + static_cast<target_ssize_t>(retBufferSize);

                    unsigned       argSize        = arg->GetSignatureType() == TYP_STRUCT
                                                        ? typGetObjLayout(arg->GetSignatureClassHandle())->GetSize()
                                                        : genTypeSize(arg->GetSignatureType());
                    target_ssize_t implByrefStart = implicitByRefLclOffs;
                    target_ssize_t implByrefEnd   = implByrefStart + static_cast<target_ssize_t>(argSize);

                    bool disjoint = (retBufferEnd <= implByrefStart) || (implByrefEnd <= retBufferStart);
                    omitCopy      = disjoint;
                }
            }

            if (omitCopy)
            {
                if (implicitByRefLcl != nullptr)
                {
                    arg->SetEarlyNode(implicitByRefLclAddr);
                }
                else
                {
                    uint16_t offs = lcl->GetLclOffs();
                    lcl->ChangeOper(GT_LCL_ADDR);
                    lcl->AsLclFld()->SetLclOffs(offs);
                    lcl->gtType = TYP_I_IMPL;
                    lcl->gtFlags &= ~GTF_ALL_EFFECT;
                    lvaSetVarAddrExposed(varNum DEBUGARG(AddressExposedReason::ESCAPE_ADDRESS));

                    // Copy prop could allow creating another later use of lcl if there are live assertions about it.
                    fgKillDependentAssertions(varNum DEBUGARG(lcl));
                }

                JITDUMP("did not need to make outgoing copy for last use of V%02d\n", varNum);
                return;
            }
        }
    }
#endif

    JITDUMP("making an outgoing copy for struct arg\n");
    assert(!call->IsTailCall() || !arg->AbiInfo.IsPassedByReference());

    CORINFO_CLASS_HANDLE copyBlkClass = arg->GetSignatureClassHandle();
    unsigned             tmp          = 0;
    bool                 found        = false;

    // Attempt to find a local we have already used for an outgoing struct and reuse it.
    // We do not reuse within a statement and we don't reuse if we're in LIR
    if (!opts.MinOpts() && (fgOrder == FGOrderTree))
    {
        found = ForEachHbvBitSet(*fgAvailableOutgoingArgTemps, [&](indexType lclNum) {
            LclVarDsc*   varDsc = lvaGetDesc((unsigned)lclNum);
            ClassLayout* layout = varDsc->GetLayout();
            if (!layout->IsCustomLayout() && (layout->GetClassHandle() == copyBlkClass))
            {
                tmp = (unsigned)lclNum;
                JITDUMP("reusing outgoing struct arg V%02u\n", tmp);
                fgAvailableOutgoingArgTemps->clearBit(lclNum);
                return HbvWalk::Abort;
            }

            return HbvWalk::Continue;
        }) == HbvWalk::Abort;
    }

    // Create the CopyBlk tree and insert it.
    if (!found)
    {
        // Get a new temp
        // Here We don't need unsafe value cls check, since the addr of this temp is used only in copyblk.
        tmp = lvaGrabTemp(true DEBUGARG("by-value struct argument"));
        lvaSetStruct(tmp, copyBlkClass, false);
    }

    if (fgUsedSharedTemps != nullptr)
    {
        fgUsedSharedTemps->Push(tmp);
    }
    else
    {
        assert(!fgGlobalMorph);
    }

    call->gtArgs.SetNeedsTemp(arg);

    // Copy the valuetype to the temp
    GenTree* copyBlk = gtNewStoreLclVarNode(tmp, argx);
    copyBlk          = fgMorphCopyBlock(copyBlk);

    GenTree* argNode;
    if (arg->AbiInfo.IsPassedByReference())
    {
        argNode = gtNewLclVarAddrNode(tmp);
        lvaSetVarAddrExposed(tmp DEBUGARG(AddressExposedReason::ESCAPE_ADDRESS));
    }
    else
    {
        argNode = gtNewLclvNode(tmp, lvaGetDesc(tmp)->TypeGet());
    }
    argNode->SetMorphed(this);

#if FEATURE_FIXED_OUT_ARGS

    // For fixed out args we create the setup node here; EvalArgsToTemps knows
    // to handle the case of "already have a setup node" properly.
    arg->SetEarlyNode(copyBlk);
    arg->SetLateNode(argNode);

#else  // !FEATURE_FIXED_OUT_ARGS

    // Structs are always on the stack, and thus never need temps
    // so we have to put the copy and temp all into one expression.
    // Change the expression to "(tmp=val),tmp"
    argNode = gtNewOperNode(GT_COMMA, argNode->TypeGet(), copyBlk, argNode);
    argNode->SetMorphed(this);

    arg->SetEarlyNode(argNode);
#endif // !FEATURE_FIXED_OUT_ARGS

    if (!arg->AbiInfo.IsPassedByReference())
    {
        bool morphed = fgTryMorphStructArg(arg);
        // Should always succeed for an unpromoted local.
        assert(morphed);
    }
}

/*****************************************************************************
 *
 *  A little helper used to rearrange nested commutative operations. The
 *  effect is that nested associative, commutative operations are transformed
 *  into a 'left-deep' tree, i.e. into something like this:
 *
 *      (((a op b) op c) op d) op...
 */

#if REARRANGE_ADDS

void Compiler::fgMoveOpsLeft(GenTree* tree)
{
    GenTree*   op1;
    GenTree*   op2;
    genTreeOps oper;

    do
    {
        op1  = tree->AsOp()->gtOp1;
        op2  = tree->AsOp()->gtOp2;
        oper = tree->OperGet();

        noway_assert(GenTree::OperIsCommutative(oper));
        noway_assert(oper == GT_ADD || oper == GT_XOR || oper == GT_OR || oper == GT_AND || oper == GT_MUL);
        noway_assert(!varTypeIsFloating(tree->TypeGet()) || !opts.genFPorder);
        noway_assert(oper == op2->gtOper);

        // Commutativity doesn't hold if overflow checks are needed

        if (tree->gtOverflowEx() || op2->gtOverflowEx())
        {
            return;
        }

        if (oper == GT_MUL && (op2->gtFlags & GTF_MUL_64RSLT))
        {
            return;
        }

        // Check for GTF_ADDRMODE_NO_CSE flag on add/mul Binary Operators
        if (tree->IsPartOfAddressMode())
        {
            return;
        }

        noway_assert(!tree->gtOverflowEx() && !op2->gtOverflowEx());

        GenTree* ad1 = op2->AsOp()->gtOp1;
        GenTree* ad2 = op2->AsOp()->gtOp2;

        // Compiler::optOptimizeBools() can create GT_OR of two GC pointers yielding a GT_INT
        // We can not reorder such GT_OR trees
        //
        if (varTypeIsGC(ad1->TypeGet()) != varTypeIsGC(op2->TypeGet()))
        {
            break;
        }

        // Don't split up a byref calculation and create a new byref. E.g.,
        // [byref]+ (ref, [int]+ (int, int)) => [byref]+ ([byref]+ (ref, int), int).
        // Doing this transformation could create a situation where the first
        // addition (that is, [byref]+ (ref, int) ) creates a byref pointer that
        // no longer points within the ref object. If a GC happens, the byref won't
        // get updated. This can happen, for instance, if one of the int components
        // is negative. It also requires the address generation be in a fully-interruptible
        // code region.
        //
        if (varTypeIsGC(op1->TypeGet()) && op2->TypeIs(TYP_I_IMPL))
        {
            assert(varTypeIsGC(tree->TypeGet()) && (oper == GT_ADD));
            break;
        }

        /* Change "(x op (y op z))" to "(x op y) op z" */
        /* ie.    "(op1 op (ad1 op ad2))" to "(op1 op ad1) op ad2" */

        GenTree* new_op1 = op2;

        new_op1->AsOp()->gtOp1 = op1;
        new_op1->AsOp()->gtOp2 = ad1;

        /* Change the flags. */

        // Make sure we arent throwing away any flags
        noway_assert((new_op1->gtFlags &
                      ~(GTF_MAKE_CSE | GTF_DONT_CSE | // It is ok that new_op1->gtFlags contains GTF_DONT_CSE flag.
                        GTF_REVERSE_OPS |             // The reverse ops flag also can be set, it will be re-calculated
                        GTF_NODE_MASK | GTF_ALL_EFFECT | GTF_UNSIGNED)) == 0);

        new_op1->gtFlags =
            (new_op1->gtFlags & (GTF_NODE_MASK | GTF_DONT_CSE)) | // Make sure we propagate GTF_DONT_CSE flag.
            (op1->gtFlags & GTF_ALL_EFFECT) | (ad1->gtFlags & GTF_ALL_EFFECT);

        /* Retype new_op1 if it has not/become a GC ptr. */

        if (varTypeIsGC(op1->TypeGet()))
        {
            noway_assert(
                (varTypeIsGC(tree->TypeGet()) && op2->TypeIs(TYP_I_IMPL) && oper == GT_ADD) || // byref(ref + (int+int))
                (varTypeIsI(tree->TypeGet()) && op2->TypeIs(TYP_I_IMPL) && oper == GT_OR));    // int(gcref |
                                                                                               // int(gcref|intval))

            new_op1->gtType = tree->gtType;
        }
        else if (varTypeIsGC(ad2->TypeGet()))
        {
            // Neither ad1 nor op1 are GC. So new_op1 isnt either
            noway_assert(op1->TypeIs(TYP_I_IMPL) && ad1->TypeIs(TYP_I_IMPL));
            new_op1->gtType = TYP_I_IMPL;
        }

        // If new_op1 is a new expression. Assign it a new unique value number.
        // vnStore is null before the ValueNumber phase has run
        if (vnStore != nullptr)
        {
            // We can only keep the old value number on new_op1 if both op1 and ad2
            // have the same non-NoVN value numbers. Since op is commutative, comparing
            // only ad2 and op1 is enough.
            if ((op1->gtVNPair.GetLiberal() == ValueNumStore::NoVN) ||
                (ad2->gtVNPair.GetLiberal() == ValueNumStore::NoVN) ||
                (ad2->gtVNPair.GetLiberal() != op1->gtVNPair.GetLiberal()))
            {
                new_op1->gtVNPair.SetBoth(vnStore->VNForExpr(nullptr, new_op1->TypeGet()));
            }
        }

        tree->AsOp()->gtOp1 = new_op1;
        tree->AsOp()->gtOp2 = ad2;

        /* If 'new_op1' is now the same nested op, process it recursively */

        if ((ad1->gtOper == oper) && !ad1->gtOverflowEx())
        {
            fgMoveOpsLeft(new_op1);
        }

        /* If   'ad2'   is now the same nested op, process it
         * Instead of recursion, we set up op1 and op2 for the next loop.
         */

        op1 = new_op1;
        op2 = ad2;
    } while ((op2->gtOper == oper) && !op2->gtOverflowEx());

    return;
}

#endif

//------------------------------------------------------------------------
// fgMorphIndexAddr: Expand a GT_INDEX_ADDR node and fully morph the child operands.
//
// We expand the GT_INDEX_ADDR node into a larger tree that evaluates the array
// base and index. The simplest expansion is a GT_COMMA with a GT_BOUNDS_CHECK.
// For complex array or index expressions one or more GT_COMMA stores are inserted
// so that we only evaluate the array or index expressions once.
//
// The fully expanded tree is then morphed.  This causes gtFoldExpr to
// perform local constant prop and reorder the constants in the tree and
// fold them.
//
// Arguments:
//    indexAddr - The INDEX_ADRR tree to morph
//
// Return Value:
//    The resulting tree.
//
GenTree* Compiler::fgMorphIndexAddr(GenTreeIndexAddr* indexAddr)
{
    const int MAX_ARR_COMPLEXITY   = 4;
    const int MAX_INDEX_COMPLEXITY = 4;

    var_types            elemTyp        = indexAddr->gtElemType;
    unsigned             elemSize       = indexAddr->gtElemSize;
    uint8_t              elemOffs       = static_cast<uint8_t>(indexAddr->gtElemOffset);
    CORINFO_CLASS_HANDLE elemStructType = indexAddr->gtStructElemClass;

    noway_assert(!varTypeIsStruct(elemTyp) || (elemStructType != NO_CLASS_HANDLE));

    // In minopts, we will not be expanding GT_INDEX_ADDR in order to minimize the size of the IR. As minopts
    // compilation time is roughly proportional to the size of the IR, this helps keep compilation times down.
    // Furthermore, this representation typically saves on code size in minopts w.r.t. the complete expansion
    // performed when optimizing, as it does not require LclVar nodes (which are always stack loads/stores in
    // minopts).
    //
    // When we *are* optimizing, we fully expand GT_INDEX_ADDR to:
    // 1. Evaluate the array address expression and store the result in a temp if the expression is complex or
    //    side-effecting.
    // 2. Evaluate the array index expression and store the result in a temp if the expression is complex or
    //    side-effecting.
    // 3. Perform an explicit bounds check: GT_BOUNDS_CHECK(index, GT_ARR_LENGTH(array))
    // 4. Compute the address of the element that will be accessed:
    //    GT_ADD(GT_ADD(array, firstElementOffset), GT_MUL(index, elementSize)) OR
    //    GT_ADD(GT_ADD(array, GT_ADD(GT_MUL(index, elementSize), firstElementOffset)))
    // 5. Wrap the address in a GT_ADD_ADDR (the information saved there will later be used by VN).
    //
    // This expansion explicitly exposes the bounds check and the address calculation to the optimizer, which allows
    // for more straightforward bounds-check removal, CSE, etc.
    if (opts.MinOpts())
    {
        indexAddr->Arr()   = fgMorphTree(indexAddr->Arr());
        indexAddr->Index() = fgMorphTree(indexAddr->Index());
        indexAddr->AddAllEffectsFlags(indexAddr->Arr(), indexAddr->Index());

        // Mark the indirection node as needing a range check if necessary.
        // Note this will always be true unless JitSkipArrayBoundCheck() is used
        if (indexAddr->IsBoundsChecked())
        {
            fgAddCodeRef(compCurBB, SCK_RNGCHK_FAIL);
        }

        return indexAddr;
    }

#ifdef FEATURE_SIMD
    if (varTypeIsStruct(elemTyp) && structSizeMightRepresentSIMDType(elemSize))
    {
        elemTyp = impNormStructType(elemStructType);
    }
#endif // FEATURE_SIMD

    // TODO-CQ: support precise equivalence classes for SIMD-typed arrays in VN.
    if (elemTyp != TYP_STRUCT)
    {
        elemStructType = NO_CLASS_HANDLE;
    }

    GenTree*          arrRef      = indexAddr->Arr();
    GenTree*          index       = indexAddr->Index();
    GenTree*          arrRefDefn  = nullptr; // non-NULL if we need to allocate a temp for the arrRef expression
    GenTree*          indexDefn   = nullptr; // non-NULL if we need to allocate a temp for the index expression
    GenTreeBoundsChk* boundsCheck = nullptr;

    // If we're doing range checking, introduce a GT_BOUNDS_CHECK node for the address.
    if (indexAddr->IsBoundsChecked())
    {
        GenTree* arrRef2   = nullptr; // The second copy will be used in array address expression
        GenTree* index2    = nullptr;
        auto     countNode = [](GenTree* node) -> unsigned {
            return 1;
        };

        // If the arrRef or index expressions involves a store, a call, or reads from global memory,
        // then we *must* allocate a temporary in which to "localize" those values, to ensure that the
        // same values are used in the bounds check and the actual dereference.
        // Also we allocate the temporary when the expression is sufficiently complex/expensive. We special
        // case some simple nodes for which CQ analysis shows it is a little better to do that here than
        // leaving them to CSE.
        //
        // TODO-Bug: GLOB_REF is not yet set for all trees in pre-order morph.
        //
        if (((arrRef->gtFlags & (GTF_ASG | GTF_CALL | GTF_GLOB_REF)) != 0) ||
            gtComplexityExceeds(arrRef, MAX_ARR_COMPLEXITY, countNode) || arrRef->OperIs(GT_LCL_FLD) ||
            (arrRef->OperIs(GT_LCL_VAR) && lvaIsLocalImplicitlyAccessedByRef(arrRef->AsLclVar()->GetLclNum())))
        {
            unsigned arrRefTmpNum = lvaGrabTemp(true DEBUGARG("arr expr"));
            arrRefDefn            = gtNewTempStore(arrRefTmpNum, arrRef);
            arrRef                = gtNewLclvNode(arrRefTmpNum, lvaGetDesc(arrRefTmpNum)->TypeGet());
            arrRef2               = gtNewLclvNode(arrRefTmpNum, lvaGetDesc(arrRefTmpNum)->TypeGet());
        }
        else
        {
            arrRef2 = gtCloneExpr(arrRef);
            noway_assert(arrRef2 != nullptr);
        }

        if (((index->gtFlags & (GTF_ASG | GTF_CALL | GTF_GLOB_REF)) != 0) ||
            gtComplexityExceeds(index, MAX_INDEX_COMPLEXITY, countNode) || index->OperIs(GT_LCL_FLD) ||
            (index->OperIs(GT_LCL_VAR) && lvaIsLocalImplicitlyAccessedByRef(index->AsLclVar()->GetLclNum())))
        {
            unsigned indexTmpNum = lvaGrabTemp(true DEBUGARG("index expr"));
            indexDefn            = gtNewTempStore(indexTmpNum, index);
            index                = gtNewLclvNode(indexTmpNum, lvaGetDesc(indexTmpNum)->TypeGet());
            index2               = gtNewLclvNode(indexTmpNum, lvaGetDesc(indexTmpNum)->TypeGet());
        }
        else
        {
            index2 = gtCloneExpr(index);
            noway_assert(index2 != nullptr);
        }

        // Next introduce a GT_BOUNDS_CHECK node
        var_types bndsChkType = TYP_INT; // By default, try to use 32-bit comparison for array bounds check.

#ifdef TARGET_64BIT
        // The CLI Spec allows an array to be indexed by either an int32 or a native int.  In the case
        // of a 64 bit architecture this means the array index can potentially be a TYP_LONG, so for this case,
        // the comparison will have to be widened to 64 bits.
        if (index->TypeIs(TYP_I_IMPL))
        {
            bndsChkType = TYP_I_IMPL;
        }
#endif // TARGET_64BIT

        GenTree* arrLen = gtNewArrLen(TYP_INT, arrRef, (int)indexAddr->gtLenOffset);

        if (bndsChkType != TYP_INT)
        {
            arrLen = gtNewCastNode(bndsChkType, arrLen, true, bndsChkType);
        }

        boundsCheck            = new (this, GT_BOUNDS_CHECK) GenTreeBoundsChk(index, arrLen, SCK_RNGCHK_FAIL);
        boundsCheck->gtInxType = elemTyp;

        // Now we'll switch to using the second copies for arrRef and index
        // to compute the address expression
        arrRef = arrRef2;
        index  = index2;
    }

    // Create the "addr" which is "*(arrRef + ((index * elemSize) + elemOffs))"
    GenTree* addr;

#ifdef TARGET_64BIT
    // Widen 'index' on 64-bit targets
    if (!index->TypeIs(TYP_I_IMPL))
    {
        if (index->OperIs(GT_CNS_INT))
        {
            index->gtType = TYP_I_IMPL;
        }
        else
        {
            index = gtNewCastNode(TYP_I_IMPL, index, true, TYP_I_IMPL);
        }
    }
#endif // TARGET_64BIT

    /* Scale the index value if necessary */
    if (elemSize > 1)
    {
        GenTree* size = gtNewIconNode(elemSize, TYP_I_IMPL);

        // Fix 392756 WP7 Crossgen
        //
        // During codegen optGetArrayRefScaleAndIndex() makes the assumption that op2 of a GT_MUL node
        // is a constant and is not capable of handling CSE'ing the elemSize constant into a lclvar.
        // Hence to prevent the constant from becoming a CSE we mark it as NO_CSE.
        //
        size->gtFlags |= GTF_DONT_CSE;

        /* Multiply by the array element size */
        addr = gtNewOperNode(GT_MUL, TYP_I_IMPL, index, size);
    }
    else
    {
        addr = index;
    }

    // Be careful to only create the byref pointer when the full index expression is added to the array reference.
    // We don't want to create a partial byref address expression that doesn't include the full index offset:
    // a byref must point within the containing object. It is dangerous (especially when optimizations come into
    // play) to create a "partial" byref that doesn't point exactly to the correct object; there is risk that
    // the partial byref will not point within the object, and thus not get updated correctly during a GC.
    // This is mostly a risk in fully-interruptible code regions.

    // We can generate three types of trees for "addr":
    //
    //  1) "arrRef + (index + elemOffset)"
    //  2) "(arrRef + elemOffset) + index"
    //  3) "(arrRef + index) + elemOffset"
    //
    // XArch has powerful addressing modes such as [base + index*scale + offset] so it's fine with 1),
    // while for Arm we better try to make an invariant sub-tree as large as possible, which is usually
    // "(arrRef + elemOffset)" and is CSE/LoopHoisting friendly => produces better codegen.
    // 2) should still be safe from GC's point of view since both ADD operations are byref and point to
    // within the object so GC will be able to correctly track and update them.
    //
    // RISC-V has very minimal addressing mode: [base + offset] which won't benefit much from CSE/LoopHoisting. However,
    // RISC-V has the SH(X)ADD_(UW) instruction that represents [base + index] well. Therefore, 3) lends itself more
    // naturally to RISC-V addressing mode.

    bool groupArrayRefWithElemOffset = false;
#ifdef TARGET_ARMARCH
    groupArrayRefWithElemOffset = true;
    // TODO: in some cases even on ARM we better use 1) shape because if "index" is invariant and "arrRef" is not
    // we at least will be able to hoist/CSE "index + elemOffset" in some cases.
    // See https://github.com/dotnet/runtime/pull/61293#issuecomment-964146497

    // Don't use 2) for structs to reduce number of size regressions
    if (varTypeIsStruct(elemTyp))
    {
        groupArrayRefWithElemOffset = false;
    }
#endif
    bool groupArrayRefWithIndex = false;
#if defined(TARGET_RISCV64)
    groupArrayRefWithIndex = true;

    // Don't use 3) for structs to reduce number of size regressions
    if (varTypeIsStruct(elemTyp))
    {
        groupArrayRefWithIndex = false;
    }
#endif

    // Note the array reference may now be TYP_I_IMPL, TYP_BYREF, or TYP_REF
    //
    var_types const arrPtrType = arrRef->TypeIs(TYP_I_IMPL) ? TYP_I_IMPL : TYP_BYREF;

    // First element's offset
    GenTree* elemOffset = gtNewIconNode(elemOffs, TYP_I_IMPL);
    if (groupArrayRefWithElemOffset)
    {
        GenTree* basePlusOffset = gtNewOperNode(GT_ADD, arrPtrType, arrRef, elemOffset);
        addr                    = gtNewOperNode(GT_ADD, arrPtrType, basePlusOffset, addr);
    }
    else if (groupArrayRefWithIndex)
    {
        addr = gtNewOperNode(GT_ADD, TYP_BYREF, arrRef, addr);
        addr = gtNewOperNode(GT_ADD, TYP_BYREF, addr, elemOffset);
    }
    else
    {
        addr = gtNewOperNode(GT_ADD, TYP_I_IMPL, addr, elemOffset);
        addr = gtNewOperNode(GT_ADD, arrPtrType, arrRef, addr);
    }

    // TODO-Throughput: bash the INDEX_ADDR to ARR_ADDR here instead of creating a new node.
    addr = new (this, GT_ARR_ADDR) GenTreeArrAddr(addr, elemTyp, elemStructType, elemOffs);

    if (indexAddr->IsNotNull())
    {
        addr->gtFlags |= GTF_ARR_ADDR_NONNULL;
    }

    GenTree* tree = addr;

    // Prepend the bounds check and the store trees that were created (if any).
    if (boundsCheck != nullptr)
    {
        // This is changing a value dependency (INDEX_ADDR node) into a flow
        // dependency, so make sure this dependency remains visible. Also, the
        // JIT is not allowed to create arbitrary byrefs, so we must make sure
        // the address is not reordered with the bounds check.
        boundsCheck->SetHasOrderingSideEffect();
        addr->SetHasOrderingSideEffect();

        tree = gtNewOperNode(GT_COMMA, tree->TypeGet(), boundsCheck, tree);
        fgAddCodeRef(compCurBB, boundsCheck->gtThrowKind);
    }

    if (indexDefn != nullptr)
    {
        tree = gtNewOperNode(GT_COMMA, tree->TypeGet(), indexDefn, tree);
    }

    if (arrRefDefn != nullptr)
    {
        tree = gtNewOperNode(GT_COMMA, tree->TypeGet(), arrRefDefn, tree);
    }

    JITDUMP("fgMorphIndexAddr (before remorph):\n")
    DISPTREE(tree)

    tree = fgMorphTree(tree);

    JITDUMP("fgMorphIndexAddr (after remorph):\n")
    DISPTREE(tree)

    return tree;
}

//------------------------------------------------------------------------
// fgMorphLeafLocal: Fully morph a leaf local node.
//
// Arguments:
//    lclNode - The node to morph
//
// Return Value:
//    The fully morphed tree.
//
GenTree* Compiler::fgMorphLeafLocal(GenTreeLclVarCommon* lclNode)
{
    assert(lclNode->OperIs(GT_LCL_VAR, GT_LCL_FLD, GT_LCL_ADDR));

    GenTree* expandedTree = fgMorphExpandLocal(lclNode);
    if (expandedTree != nullptr)
    {
        expandedTree = fgMorphTree(expandedTree);
        return expandedTree;
    }

    if (lclNode->OperIs(GT_LCL_ADDR))
    {
        // No further morphing necessary.
        return lclNode;
    }

    LclVarDsc* varDsc = lvaGetDesc(lclNode);
    // For last-use copy omission candidates we will address expose them when
    // we get to the call that passes their address, but they are not actually
    // address exposed in the full sense, so we allow standard assertion prop
    // on them until that point. However, we must still mark them with
    // GTF_GLOB_REF to avoid illegal reordering with the call passing their
    // address.
    if (varDsc->IsAddressExposed()
#if FEATURE_IMPLICIT_BYREFS
        || varDsc->lvIsLastUseCopyOmissionCandidate
#endif
    )
    {
        lclNode->gtFlags |= GTF_GLOB_REF;
    }

    // Small-typed arguments and aliased locals are normalized on load. Other small-typed locals are
    // normalized on store. If this is one of the former, insert a narrowing cast on the load.
    //         ie. Convert: var-short --> cast-short(var-int)
    //
    if (fgGlobalMorph && lclNode->OperIs(GT_LCL_VAR) && varDsc->lvNormalizeOnLoad() &&
        /* TODO-ASG: delete this zero-diff quirk */ lclNode->CanCSE())
    {
        var_types lclVarType = varDsc->TypeGet();

        // Assertion prop can tell us to omit adding a cast here. This is useful when the local is a small-typed
        // parameter that is passed in a register: in that case, the ABI specifies that the upper bits might be
        // invalid, but the assertion guarantees us that we have normalized when we wrote it.
        if (optLocalAssertionProp &&
            optAssertionIsSubrange(lclNode, IntegralRange::ForType(lclVarType), apLocal) != NO_ASSERTION_INDEX)
        {
            // The previous assertion can guarantee us that if this node gets
            // assigned a register, it will be normalized already. It is still
            // possible that this node ends up being in memory, in which case
            // normalization will still be needed, so we better have the right
            // type.
            assert(lclNode->TypeGet() == varDsc->TypeGet());
            return lclNode;
        }

        lclNode->gtType = TYP_INT;
        fgMorphTreeDone(lclNode);
        GenTree* cast = gtNewCastNode(TYP_INT, lclNode, false, lclVarType);
        fgMorphTreeDone(cast);

        return cast;
    }

    return lclNode;
}

#ifdef TARGET_X86
//------------------------------------------------------------------------
// fgMorphExpandStackArgForVarArgs: Expand a stack arg node for varargs.
//
// Expands the node to use the varargs cookie as the base address, indirecting
// off of it if necessary, similar to how implicit by-ref parameters are morphed
// on non-x86 targets.
//
// Arguments:
//    lclNode - The local node to (possibly) morph
//
// Return Value:
//    The new tree for "lclNode", in which case the caller is expected to morph
//    it further, otherwise "nullptr".
//
GenTree* Compiler::fgMorphExpandStackArgForVarArgs(GenTreeLclVarCommon* lclNode)
{
    if (!lvaIsArgAccessedViaVarArgsCookie(lclNode->GetLclNum()))
    {
        return nullptr;
    }

    LclVarDsc*                   varDsc  = lvaGetDesc(lclNode);
    const ABIPassingInformation& abiInfo = lvaGetParameterABIInfo(lclNode->GetLclNum());
    assert(abiInfo.HasExactlyOneStackSegment());

    GenTree* argsBaseAddr = gtNewLclvNode(lvaVarargsBaseOfStkArgs, TYP_I_IMPL);
    ssize_t  offset       = (ssize_t)abiInfo.Segment(0).GetStackOffset() - lclNode->GetLclOffs();
    GenTree* offsetNode   = gtNewIconNode(offset, TYP_I_IMPL);
    GenTree* argAddr      = gtNewOperNode(GT_SUB, TYP_I_IMPL, argsBaseAddr, offsetNode);

    GenTree* argNode;
    if (lclNode->OperIsLocalStore())
    {
        GenTree* value = lclNode->Data();
        argNode        = lclNode->TypeIs(TYP_STRUCT) ? gtNewStoreBlkNode(lclNode->GetLayout(this), argAddr, value)
                                                     : gtNewStoreIndNode(lclNode->TypeGet(), argAddr, value)->AsIndir();
    }
    else if (lclNode->OperIsLocalRead())
    {
        argNode = lclNode->TypeIs(TYP_STRUCT) ? gtNewBlkIndir(lclNode->GetLayout(this), argAddr)
                                              : gtNewIndir(lclNode->TypeGet(), argAddr);
    }
    else
    {
        argNode = argAddr;
    }

    return argNode;
}
#endif

//------------------------------------------------------------------------
// fgMorphExpandImplicitByRefArg: Morph an implicit by-ref parameter.
//
// Arguments:
//    lclNode - The local node to morph
//
// Return Value:
//    The expanded tree for "lclNode", which the caller is expected to
//    morph further.
//
GenTree* Compiler::fgMorphExpandImplicitByRefArg(GenTreeLclVarCommon* lclNode)
{
    unsigned   lclNum         = lclNode->GetLclNum();
    LclVarDsc* varDsc         = lvaGetDesc(lclNum);
    unsigned   fieldOffset    = 0;
    unsigned   newLclNum      = BAD_VAR_NUM;
    bool       isStillLastUse = false;

    assert(lvaIsImplicitByRefLocal(lclNum) ||
           (varDsc->lvIsStructField && lvaIsImplicitByRefLocal(varDsc->lvParentLcl)));

    if (lvaIsImplicitByRefLocal(lclNum))
    {
        // The SIMD transformation to coalesce contiguous references to SIMD vector fields will re-invoke
        // the traversal to mark address-taken locals. So, we may encounter a tree that has already been
        // transformed to TYP_BYREF. If we do, leave it as-is.
        if (lclNode->OperIs(GT_LCL_VAR) && lclNode->TypeIs(TYP_BYREF))
        {
            return nullptr;
        }

        if (varDsc->lvPromoted)
        {
            // fgRetypeImplicitByRefArgs created a new promoted struct local to represent this arg.
            // Rewrite the node to refer to it.
            assert(varDsc->lvFieldLclStart != 0);

            lclNode->SetLclNum(varDsc->lvFieldLclStart);
            return lclNode;
        }

        newLclNum = lclNum;

        // As a special case, for implicit byref args where we undid promotion we
        // can still know whether the use of the implicit byref local is a last
        // use, and whether we can omit a copy when passed as an argument (the
        // common reason why promotion is undone).
        //
        // We skip this propagation for the fields of the promoted local. Those are
        // going to be transformed into accesses off of the parent and we cannot
        // know here if this is going to be the last use of the parent local (this
        // would require tracking a full life set on the side, which we do not do
        // in morph).
        //
        if (!varDsc->lvPromoted)
        {
            if (varDsc->lvFieldLclStart != 0)
            {
                // Reference to whole implicit byref parameter that was promoted
                // but isn't anymore. Check if all fields are dying.
                GenTreeFlags allFieldsDying = lvaGetDesc(varDsc->lvFieldLclStart)->AllFieldDeathFlags();
                isStillLastUse              = (lclNode->gtFlags & allFieldsDying) == allFieldsDying;
            }
            else
            {
                // Was never promoted, treated as single value.
                isStillLastUse = (lclNode->gtFlags & GTF_VAR_DEATH) != 0;
            }
        }
    }
    else
    {
        // This was a field reference to an implicit-by-reference struct parameter that was dependently promoted.
        newLclNum   = varDsc->lvParentLcl;
        fieldOffset = varDsc->lvFldOffset;
    }

    // Add a level of indirection to this node. The "base" will be a local node referring to "newLclNum".
    // We will also add an offset, and, if the original "lclNode" represents a location, a dereference.
    GenTree*     data          = lclNode->OperIsLocalStore() ? lclNode->Data() : nullptr;
    bool         isLoad        = lclNode->OperIsLocalRead();
    unsigned     offset        = lclNode->GetLclOffs() + fieldOffset;
    var_types    argNodeType   = lclNode->TypeGet();
    ClassLayout* argNodeLayout = (argNodeType == TYP_STRUCT) ? lclNode->GetLayout(this) : nullptr;

    JITDUMP("\nRewriting an implicit by-ref parameter reference:\n");
    DISPTREE(lclNode);

    lclNode->ChangeType(TYP_BYREF);
    lclNode->ChangeOper(GT_LCL_VAR);
    lclNode->SetLclNum(newLclNum);
    lclNode->SetAllEffectsFlags(GTF_EMPTY); // Implicit by-ref parameters cannot be address-exposed.

    if (isStillLastUse)
    {
        lclNode->gtFlags |= GTF_VAR_DEATH;
    }

    GenTree* addrNode = lclNode;
    if (offset != 0)
    {
        addrNode = gtNewOperNode(GT_ADD, TYP_BYREF, addrNode, gtNewIconNode(offset, TYP_I_IMPL));
    }

    // Note: currently, we have to conservatively treat all indirections off of implicit byrefs
    // as global. This is because we lose the information on whether the original local's address
    // was exposed when we retype it in "fgRetypeImplicitByRefArgs".
    //
    GenTree* newArgNode;
    if (data != nullptr)
    {
        newArgNode = (argNodeType == TYP_STRUCT) ? gtNewStoreBlkNode(argNodeLayout, addrNode, data)
                                                 : gtNewStoreIndNode(argNodeType, addrNode, data)->AsIndir();
    }
    else if (isLoad)
    {
        newArgNode =
            (argNodeType == TYP_STRUCT) ? gtNewBlkIndir(argNodeLayout, addrNode) : gtNewIndir(argNodeType, addrNode);
    }
    else
    {
        newArgNode = addrNode;
    }

    JITDUMP("Transformed into:\n");
    DISPTREE(newArgNode);
    JITDUMP("\n");

    return newArgNode;
}

GenTree* Compiler::fgMorphExpandLocal(GenTreeLclVarCommon* lclNode)
{
    GenTree* expandedTree = nullptr;
#ifdef TARGET_X86
    expandedTree = fgMorphExpandStackArgForVarArgs(lclNode);
#else
#if FEATURE_IMPLICIT_BYREFS
    if (fgGlobalMorph)
    {
        LclVarDsc* dsc = lvaGetDesc(lclNode);
        if (dsc->lvIsImplicitByRef || (dsc->lvIsStructField && lvaIsImplicitByRefLocal(dsc->lvParentLcl)))
        {
            expandedTree = fgMorphExpandImplicitByRefArg(lclNode);
        }
    }
#endif
#endif

    if (expandedTree != nullptr)
    {
        return expandedTree;
    }

    // Small-typed arguments and aliased locals are normalized on load. Other small-typed
    // locals are normalized on store. If it is the latter case, insert the cast on source.
    if (fgGlobalMorph && lclNode->OperIs(GT_STORE_LCL_VAR) && genActualTypeIsInt(lclNode))
    {
        LclVarDsc* varDsc = lvaGetDesc(lclNode);

        if (varDsc->lvNormalizeOnStore())
        {
            GenTree* value = lclNode->Data();
            noway_assert(genActualTypeIsInt(value));

            lclNode->gtType = TYP_INT;

            if (fgCastNeeded(value, varDsc->TypeGet()))
            {
                lclNode->Data() = gtNewCastNode(TYP_INT, value, false, varDsc->TypeGet());
                return lclNode;
            }
        }
    }

    return nullptr;
}

//------------------------------------------------------------------------
// fgGetFieldMorphingTemp: Get a local to use for field morphing.
//
// We will reuse locals created when morphing field addresses, as well as
// fields with large offsets.
//
// Arguments:
//    fieldNode - The field node
//
// Return Value:
//    The local number.
//
unsigned Compiler::fgGetFieldMorphingTemp(GenTreeFieldAddr* fieldNode)
{
    assert(fieldNode->IsInstance());

    unsigned lclNum = BAD_VAR_NUM;

    if (fieldNode->IsOffsetKnown() && (fieldNode->gtFldOffset == 0))
    {
        // Quirk: always use a fresh temp for zero-offset fields. This is
        // because temp reuse can create IR where some uses will be in
        // positions we do not support (i. e. [use...store...user]).
        lclNum = lvaGrabTemp(true DEBUGARG("Zero offset field obj"));
    }
    else
    {
        var_types type = genActualType(fieldNode->GetFldObj());
        lclNum         = fgBigOffsetMorphingTemps[type];

        if (lclNum == BAD_VAR_NUM)
        {
            // We haven't created a temp for this kind of type. Create one now.
            lclNum                         = lvaGrabTemp(false DEBUGARG("Field obj"));
            fgBigOffsetMorphingTemps[type] = lclNum;
        }
        else
        {
            // We better get the right type.
            noway_assert(lvaTable[lclNum].TypeGet() == type);
        }
    }

    assert(lclNum != BAD_VAR_NUM);
    return lclNum;
}

//------------------------------------------------------------------------
// fgMorphFieldAddr: Fully morph a FIELD_ADDR tree.
//
// Expands the field node into explicit additions.
//
// Arguments:
//    tree - The FIELD_ADDR tree
//    mac  - The morphing context, used to elide adding null checks
//
// Return Value:
//    The fully morphed "tree".
//
GenTree* Compiler::fgMorphFieldAddr(GenTree* tree, MorphAddrContext* mac)
{
    assert(tree->OperIs(GT_FIELD_ADDR));

    GenTreeFieldAddr* fieldNode = tree->AsFieldAddr();
    GenTree*          objRef    = fieldNode->GetFldObj();
    bool              isAddr    = ((tree->gtFlags & GTF_FLD_DEREFERENCED) == 0);

    if (fieldNode->IsInstance())
    {
        tree = fgMorphExpandInstanceField(tree, mac);
    }
    else if (fieldNode->IsTlsStatic())
    {
        tree = fgMorphExpandTlsFieldAddr(tree);
    }
    else
    {
        assert(!"Normal statics are expected to be handled in the importer");
    }

    // Pass down the current mac; if non null we are computing an address
    GenTree* result;
    if (tree->OperIsSimple())
    {
        result = fgMorphSmpOp(tree, mac);
        result->SetMorphed(this);

        // Quirk: preserve previous behavior with this NO_CSE.
        if (isAddr && result->OperIs(GT_COMMA))
        {
            result->SetDoNotCSE();
        }
    }
    else
    {
        result = fgMorphTree(tree, mac);
    }

    JITDUMP("\nFinal value of Compiler::fgMorphFieldAddr after morphing:\n");
    DISPTREE(result);

    return result;
}

//------------------------------------------------------------------------
// fgMorphExpandInstanceField: Expand an instance field address.
//
// Expands the field node into explicit additions and nullchecks.
//
// Arguments:
//    tree - The FIELD_ADDR tree
//    mac  - The morphing context, used to elide adding null checks
//
// Return Value:
//    The expanded "tree" of an arbitrary shape.
//
GenTree* Compiler::fgMorphExpandInstanceField(GenTree* tree, MorphAddrContext* mac)
{
    assert(tree->OperIs(GT_FIELD_ADDR) && tree->AsFieldAddr()->IsInstance());

    GenTree*             objRef      = tree->AsFieldAddr()->GetFldObj();
    CORINFO_FIELD_HANDLE fieldHandle = tree->AsFieldAddr()->gtFldHnd;
    unsigned             fieldOffset = tree->AsFieldAddr()->gtFldOffset;

    noway_assert(varTypeIsI(genActualType(objRef)));

    /* Now we have a tree like this:

                                  +--------------------+
                                  |    GT_FIELD_ADDR   |   tree
                                  +----------+---------+
                                             |
                              +--------------+-------------+
                              |     tree->GetFldObj()      |
                              +--------------+-------------+

            We want to make it like this (when fldOffset is <= MAX_UNCHECKED_OFFSET_FOR_NULL_OBJECT):

                                  +---------+----------+
                                  |       GT_ADD       |   addr
                                  +---------+----------+
                                            |
                                          /   \
                                        /       \
                                      /           \
                       +-------------------+  +----------------------+
                       |       objRef      |  |     fldOffset        |
                       |                   |  | (when fldOffset !=0) |
                       +-------------------+  +----------------------+


            or this (when fldOffset is > MAX_UNCHECKED_OFFSET_FOR_NULL_OBJECT):


                                  +--------------------+
                                  |   GT_IND/GT_BLK    |   tree (for FIELD)
                                  +----------+---------+
                                             |
                                  +----------+---------+
                                  |       GT_COMMA     |   comma2
                                  +----------+---------+
                                             |
                                            / \
                                          /     \
                                        /         \
                                      /             \
                 +---------+----------+              +---------+----------+
           comma |      GT_COMMA      |              |  "+" (i.e. GT_ADD) |   addr
                 +---------+----------+              +---------+----------+
                           |                                    |
                         /   \                                 /  \
                       /       \                             /      \
                     /           \                         /          \
  +------------+-----------|       +-----+-----+     +---------+   +-----------+
  |  STORE_LCL_VAR tmpLcl  |   ind |   GT_IND  |     |  tmpLcl |   | fldOffset |
  +------------+-----------|       +-----+-----+     +---------+   +-----------+
               |                         |
               |                         |
               |                         |
               |                         |
         +-----------+             +-----------+
         |   objRef  |             |   tmpLcl  |
         +-----------+             +-----------+

    */

    var_types objRefType           = objRef->TypeGet();
    GenTree*  addr                 = nullptr;
    GenTree*  comma                = nullptr;
    bool      addExplicitNullCheck = false;

    if (fgAddrCouldBeNull(objRef))
    {
        // A non-null context here implies our [+ some offset] parent is an indirection, one that
        // will implicitly null-check the produced address.
        addExplicitNullCheck = (mac == nullptr) || fgIsBigOffset(mac->m_totalOffset + fieldOffset);

        // The transformation here turns a value dependency (FIELD_ADDR being a
        // known non-null operand) into a control-flow dependency (introducing
        // explicit COMMA(NULLCHECK, ...)). This effectively "disconnects" the
        // null check from the parent of the FIELD_ADDR node. For the cases
        // where we made use of non-nullness we need to make the dependency
        // explicit now.
        if (addExplicitNullCheck)
        {
            if (mac != nullptr)
            {
                mac->m_user->SetHasOrderingSideEffect();
            }
        }
        else
        {
            // We can elide the null check only by letting it happen as part of
            // the consuming indirection, so it is no longer non-faulting.
            mac->m_user->gtFlags &= ~GTF_IND_NONFAULTING;
        }
    }

    if (addExplicitNullCheck)
    {
        JITDUMP("Before explicit null check morphing:\n");
        DISPTREE(tree);

        // Create the "comma" subtree.
        GenTree* store = nullptr;
        unsigned lclNum;

        if (!objRef->OperIs(GT_LCL_VAR) || lvaIsLocalImplicitlyAccessedByRef(objRef->AsLclVar()->GetLclNum()))
        {
            lclNum = fgGetFieldMorphingTemp(tree->AsFieldAddr());
            store  = gtNewTempStore(lclNum, objRef);
        }
        else
        {
            lclNum = objRef->AsLclVarCommon()->GetLclNum();
        }

        GenTree* lclVar  = gtNewLclvNode(lclNum, objRefType);
        GenTree* nullchk = gtNewNullCheck(lclVar);

        nullchk->SetHasOrderingSideEffect();

        if (store != nullptr)
        {
            // Create the "comma" node.
            comma = gtNewOperNode(GT_COMMA, TYP_VOID, store, nullchk);
        }
        else
        {
            comma = nullchk;
        }

        addr = gtNewLclvNode(lclNum, objRefType); // Use "tmpLcl" to create "addr" node.
    }
    else
    {
        addr = objRef;
    }

#ifdef FEATURE_READYTORUN
    if (tree->AsFieldAddr()->gtFieldLookup.addr != nullptr)
    {
        GenTree* offsetNode = nullptr;
        if (tree->AsFieldAddr()->gtFieldLookup.accessType == IAT_PVALUE)
        {
            offsetNode = gtNewIndOfIconHandleNode(TYP_I_IMPL, (size_t)tree->AsFieldAddr()->gtFieldLookup.addr,
                                                  GTF_ICON_CONST_PTR);
#ifdef DEBUG
            offsetNode->gtGetOp1()->AsIntCon()->gtTargetHandle = (size_t)fieldHandle;
#endif
        }
        else
        {
            noway_assert(!"unexpected accessType for R2R field access");
        }

        addr = gtNewOperNode(GT_ADD, (objRefType == TYP_I_IMPL) ? TYP_I_IMPL : TYP_BYREF, addr, offsetNode);

        // We cannot form and GC report an invalid byref, so this must preserve
        // its ordering with the null check.
        if (addExplicitNullCheck && addr->TypeIs(TYP_BYREF))
        {
            addr->SetHasOrderingSideEffect();
        }
    }
#endif

    // We only need to attach the field offset information for class fields.
    FieldSeq* fieldSeq = nullptr;
    if ((objRefType == TYP_REF) && !tree->AsFieldAddr()->gtFldMayOverlap)
    {
        fieldSeq = GetFieldSeqStore()->Create(fieldHandle, fieldOffset, FieldSeq::FieldKind::Instance);
    }

    // Add the member offset to the object's address.
    if (fieldOffset != 0)
    {
        addr = gtNewOperNode(GT_ADD, (objRefType == TYP_I_IMPL) ? TYP_I_IMPL : TYP_BYREF, addr,
                             gtNewIconNode(fieldOffset, fieldSeq));

        // We cannot form and GC report an invalid byref, so this must preserve
        // its ordering with the null check.
        if (addExplicitNullCheck && addr->TypeIs(TYP_BYREF))
        {
            addr->SetHasOrderingSideEffect();
        }

        if (addr->gtGetOp1()->OperIsConst() && addr->gtGetOp2()->OperIsConst())
        {
            // Fold it if we have const-handle + const-offset
            addr = gtFoldExprConst(addr);
        }
    }

    if (addExplicitNullCheck)
    {
        // Create the "comma2" tree.
        addr = gtNewOperNode(GT_COMMA, addr->TypeGet(), comma, addr);

        JITDUMP("After adding explicit null check:\n");
        DISPTREE(addr);
    }

    return addr;
}

//------------------------------------------------------------------------
// fgMorphExpandTlsFieldAddr: Expand a TLS field address.
//
// Expands ".tls"-style statics, produced by the C++/CLI compiler for
// "__declspec(thread)" variables. An overview of the underlying native
// mechanism can be found here: http://www.nynaeve.net/?p=180.
//
// Arguments:
//    tree - The GT_FIELD_ADDR tree
//
// Return Value:
//    The expanded tree - a GT_ADD.
//
GenTree* Compiler::fgMorphExpandTlsFieldAddr(GenTree* tree)
{
    assert(tree->OperIs(GT_FIELD_ADDR) && tree->AsFieldAddr()->IsTlsStatic());

    CORINFO_FIELD_HANDLE fieldHandle = tree->AsFieldAddr()->gtFldHnd;
    int                  fieldOffset = tree->AsFieldAddr()->gtFldOffset;

    // Thread Local Storage static field reference
    //
    // Field ref is a TLS 'Thread-Local-Storage' reference
    //
    // Build this tree:  ADD(I_IMPL) #
    //                   / \.
    //                  /  CNS(fldOffset)
    //                 /
    //                /
    //               /
    //             IND(I_IMPL) == [Base of this DLL's TLS]
    //              |
    //             ADD(I_IMPL)
    //             / \.
    //            /   CNS(IdValue*4) or MUL
    //           /                      / \.
    //          IND(I_IMPL)            /  CNS(4)
    //           |                    /
    //          CNS(TLS_HDL,0x2C)    IND
    //                                |
    //                               CNS(pIdAddr)
    //
    // # Denotes the original node
    //
    void**   pIdAddr = nullptr;
    unsigned IdValue = info.compCompHnd->getFieldThreadLocalStoreID(fieldHandle, (void**)&pIdAddr);

    //
    // If we can we access the TLS DLL index ID value directly
    // then pIdAddr will be NULL and
    //      IdValue will be the actual TLS DLL index ID
    //
    GenTree* dllRef = nullptr;
    if (pIdAddr == nullptr)
    {
        if (IdValue != 0)
        {
            dllRef = gtNewIconNode(IdValue * 4, TYP_I_IMPL);
        }
    }
    else
    {
        dllRef = gtNewIndOfIconHandleNode(TYP_I_IMPL, (size_t)pIdAddr, GTF_ICON_CONST_PTR);

        // Next we multiply by 4
        dllRef = gtNewOperNode(GT_MUL, TYP_I_IMPL, dllRef, gtNewIconNode(4, TYP_I_IMPL));
    }

#define WIN32_TLS_SLOTS (0x2C) // Offset from fs:[0] where the pointer to the slots resides

    // Mark this ICON as a TLS_HDL, codegen will use FS:[cns]
    GenTree* tlsRef = gtNewIconHandleNode(WIN32_TLS_SLOTS, GTF_ICON_TLS_HDL);

    tlsRef = gtNewIndir(TYP_I_IMPL, tlsRef, GTF_IND_NONFAULTING | GTF_IND_INVARIANT);

    if (dllRef != nullptr)
    {
        // Add the dllRef.
        tlsRef = gtNewOperNode(GT_ADD, TYP_I_IMPL, tlsRef, dllRef);
    }

    // indirect to have tlsRef point at the base of the DLLs Thread Local Storage.
    tlsRef = gtNewIndir(TYP_I_IMPL, tlsRef);

    // Add the TLS static field offset to the address.
    assert(!tree->AsFieldAddr()->gtFldMayOverlap);
    FieldSeq* fieldSeq   = GetFieldSeqStore()->Create(fieldHandle, fieldOffset, FieldSeq::FieldKind::SimpleStatic);
    GenTree*  offsetNode = gtNewIconNode(fieldOffset, fieldSeq);

    tree->ChangeOper(GT_ADD);
    tree->AsOp()->gtOp1 = tlsRef;
    tree->AsOp()->gtOp2 = offsetNode;

    return tree;
}

//------------------------------------------------------------------------
// fgCanFastTailCall: Check to see if this tail call can be optimized as epilog+jmp.
//
// Arguments:
//    callee - The callee to check
//    failReason - If this method returns false, the reason why. Can be nullptr.
//
// Return Value:
//    Returns true or false based on whether the callee can be fastTailCalled
//
// Notes:
//    This function is target specific and each target will make the fastTailCall
//    decision differently. See the notes below.
//
//    This function calls AddFinalArgsAndDetermineABIInfo to initialize the ABI
//    info, which is used to analyze the argument. This function can alter the
//    call arguments by adding argument IR nodes for non-standard arguments.
//
// Windows Amd64:
//    A fast tail call can be made whenever the number of callee arguments
//    is less than or equal to the number of caller arguments, or we have four
//    or fewer callee arguments. This is because, on Windows AMD64, each
//    argument uses exactly one register or one 8-byte stack slot. Thus, we only
//    need to count arguments, and not be concerned with the size of each
//    incoming or outgoing argument.
//
// Can fast tail call examples (amd64 Windows):
//
//    -- Callee will have all register arguments --
//    caller(int, int, int, int)
//    callee(int, int, float, int)
//
//    -- Callee requires stack space that is equal or less than the caller --
//    caller(struct, struct, struct, struct, struct, struct)
//    callee(int, int, int, int, int, int)
//
//    -- Callee requires stack space that is less than the caller --
//    caller(struct, double, struct, float, struct, struct)
//    callee(int, int, int, int, int)
//
//    -- Callee will have all register arguments --
//    caller(int)
//    callee(int, int, int, int)
//
// Cannot fast tail call examples (amd64 Windows):
//
//    -- Callee requires stack space that is larger than the caller --
//    caller(struct, double, struct, float, struct, struct)
//    callee(int, int, int, int, int, double, double, double)
//
//    -- Callee has a byref struct argument --
//    caller(int, int, int)
//    callee(struct(size 3 bytes))
//
// Unix Amd64 && Arm64:
//    A fastTailCall decision can be made whenever the callee's stack space is
//    less than or equal to the caller's stack space. There are many permutations
//    of when the caller and callee have different stack sizes if there are
//    structs being passed to either the caller or callee.
//
// Exceptions:
//    If the callee has a 9 to 16 byte struct argument and the callee has
//    stack arguments, the decision will be to not fast tail call. This is
//    because before fgMorphArgs is done, the struct is unknown whether it
//    will be placed on the stack or enregistered. Therefore, the conservative
//    decision of do not fast tail call is taken. This limitations should be
//    removed if/when fgMorphArgs no longer depends on fgCanFastTailCall.
//
// Can fast tail call examples (amd64 Unix):
//
//    -- Callee will have all register arguments --
//    caller(int, int, int, int)
//    callee(int, int, float, int)
//
//    -- Callee requires stack space that is equal to the caller --
//    caller({ long, long }, { int, int }, { int }, { int }, { int }, { int }) -- 6 int register arguments, 16 byte
//    stack
//    space
//    callee(int, int, int, int, int, int, int, int) -- 6 int register arguments, 16 byte stack space
//
//    -- Callee requires stack space that is less than the caller --
//    caller({ long, long }, int, { long, long }, int, { long, long }, { long, long }) 6 int register arguments, 32 byte
//    stack
//    space
//    callee(int, int, int, int, int, int, { long, long } ) // 6 int register arguments, 16 byte stack space
//
//    -- Callee will have all register arguments --
//    caller(int)
//    callee(int, int, int, int)
//
// Cannot fast tail call examples (amd64 Unix):
//
//    -- Callee requires stack space that is larger than the caller --
//    caller(float, float, float, float, float, float, float, float) -- 8 float register arguments
//    callee(int, int, int, int, int, int, int, int) -- 6 int register arguments, 16 byte stack space
//
//    -- Callee has structs which cannot be enregistered (Implementation Limitation) --
//    caller(float, float, float, float, float, float, float, float, { double, double, double }) -- 8 float register
//    arguments, 24 byte stack space
//    callee({ double, double, double }) -- 24 bytes stack space
//
//    -- Callee requires stack space and has a struct argument >8 bytes and <16 bytes (Implementation Limitation) --
//    caller(int, int, int, int, int, int, { double, double, double }) -- 6 int register arguments, 24 byte stack space
//    callee(int, int, int, int, int, int, { int, int }) -- 6 int registers, 16 byte stack space
//
//    -- Caller requires stack space and nCalleeArgs > nCallerArgs (Bug) --
//    caller({ double, double, double, double, double, double }) // 48 byte stack
//    callee(int, int) -- 2 int registers
//
bool Compiler::fgCanFastTailCall(GenTreeCall* callee, const char** failReason)
{
#if FEATURE_FASTTAILCALL

    // To reach here means that the return types of the caller and callee are tail call compatible.
    // In the case of structs that can be returned in a register, compRetNativeType is set to the actual return type.

#ifdef DEBUG
    if (callee->IsTailPrefixedCall())
    {
        var_types retType = info.compRetType;
        assert(impTailCallRetTypeCompatible(false, retType, info.compMethodInfo->args.retTypeClass, info.compCallConv,
                                            (var_types)callee->gtReturnType, callee->gtRetClsHnd,
                                            callee->GetUnmanagedCallConv()));
    }
#endif

    assert(!callee->gtArgs.AreArgsComplete());

    callee->gtArgs.AddFinalArgsAndDetermineABIInfo(this, callee);

    unsigned calleeArgStackSize = callee->gtArgs.OutgoingArgsStackSize();
    unsigned callerArgStackSize = roundUp(lvaParameterStackSize, TARGET_POINTER_SIZE);

    auto reportFastTailCallDecision = [&](const char* thisFailReason) {
        if (failReason != nullptr)
        {
            *failReason = thisFailReason;
        }

#ifdef DEBUG
        if ((JitConfig.JitReportFastTailCallDecisions()) == 1)
        {
            if (callee->gtCallType != CT_INDIRECT)
            {
                const char* methodName;

                methodName = eeGetMethodFullName(callee->gtCallMethHnd);

                printf("[Fast tailcall decision]: Caller: %s\n[Fast tailcall decision]: Callee: %s -- Decision: ",
                       info.compFullName, methodName);
            }
            else
            {
                printf("[Fast tailcall decision]: Caller: %s\n[Fast tailcall decision]: Callee: IndirectCall -- "
                       "Decision: ",
                       info.compFullName);
            }

            if (thisFailReason == nullptr)
            {
                printf("Will fast tailcall");
            }
            else
            {
                printf("Will not fast tailcall (%s)", thisFailReason);
            }

            printf(" (CallerArgStackSize: %d, CalleeArgStackSize: %d)\n\n", callerArgStackSize, calleeArgStackSize);
        }
        else
        {
            if (thisFailReason == nullptr)
            {
                JITDUMP("[Fast tailcall decision]: Will fast tailcall\n");
            }
            else
            {
                JITDUMP("[Fast tailcall decision]: Will not fast tailcall (%s)\n", thisFailReason);
            }
        }
#endif // DEBUG
    };

#if defined(TARGET_ARM) || defined(TARGET_RISCV64) || defined(TARGET_LOONGARCH64)
    for (CallArg& arg : callee->gtArgs.Args())
    {
        if (arg.AbiInfo.IsSplitAcrossRegistersAndStack())
        {
            reportFastTailCallDecision("Argument splitting in callee is not supported on " TARGET_READABLE_NAME);
            return false;
        }
    }

    for (unsigned lclNum = 0; lclNum < info.compArgsCount; lclNum++)
    {
        const ABIPassingInformation& abiInfo = lvaGetParameterABIInfo(lclNum);
        if (abiInfo.IsSplitAcrossRegistersAndStack())
        {
            reportFastTailCallDecision("Argument splitting in caller is not supported on " TARGET_READABLE_NAME);
            return false;
        }
    }
#endif // TARGET_ARM || TARGET_RISCV64 || defined(TARGET_LOONGARCH64)

#ifdef TARGET_ARM
    if (compIsProfilerHookNeeded())
    {
        reportFastTailCallDecision("Profiler is not supported on ARM32");
        return false;
    }

    // On ARM32 we have only one non-parameter volatile register and we need it
    // for the GS security cookie check. We could technically still tailcall
    // when the callee does not use all argument registers, but we keep the
    // code simple here.
    if (getNeedsGSSecurityCookie())
    {
        reportFastTailCallDecision("Not enough registers available due to the GS security cookie check");
        return false;
    }
#endif

    if (!opts.compFastTailCalls)
    {
        reportFastTailCallDecision("Configuration doesn't allow fast tail calls");
        return false;
    }

    if (callee->IsStressTailCall())
    {
        reportFastTailCallDecision("Fast tail calls are not performed under tail call stress");
        return false;
    }

#ifdef TARGET_ARM
    if (callee->IsR2RRelativeIndir() || callee->HasNonStandardAddedArgs(this))
    {
        reportFastTailCallDecision(
            "Method with non-standard args passed in callee saved register cannot be tail called");
        return false;
    }
#endif

    // Note on vararg methods:
    // If the caller is vararg method, we don't know the number of arguments passed by caller's caller.
    // But we can be sure that in-coming arg area of vararg caller would be sufficient to hold its
    // fixed args. Therefore, we can allow a vararg method to fast tail call other methods as long as
    // out-going area required for callee is bounded by caller's fixed argument space.
    //
    // Note that callee being a vararg method is not a problem since we can account the params being passed.
    //
    // We will currently decide to not fast tail call on Windows armarch if the caller or callee is a vararg
    // method. This is due to the ABI differences for native vararg methods for these platforms. There is
    // work required to shuffle arguments to the correct locations.

    if (TargetOS::IsWindows && TargetArchitecture::IsArmArch && (info.compIsVarArgs || callee->IsVarargs()))
    {
        reportFastTailCallDecision("Fast tail calls with varargs not supported on Windows ARM/ARM64");
        return false;
    }

    if (compLocallocUsed)
    {
        reportFastTailCallDecision("Localloc used");
        return false;
    }

#ifdef TARGET_AMD64
    // Needed for Jit64 compat.
    // In future, enabling fast tail calls from methods that need GS cookie
    // check would require codegen side work to emit GS cookie check before a
    // tail call.
    if (getNeedsGSSecurityCookie())
    {
        reportFastTailCallDecision("GS Security cookie check required");
        return false;
    }
#endif

    // If the NextCallReturnAddress intrinsic is used we should do normal calls.
    if (info.compHasNextCallRetAddr)
    {
        reportFastTailCallDecision("Uses NextCallReturnAddress intrinsic");
        return false;
    }

    if (callee->gtArgs.HasRetBuffer())
    {
        // If callee has RetBuf param, caller too must have it.
        // Otherwise go the slow route.
        if (info.compRetBuffArg == BAD_VAR_NUM)
        {
            reportFastTailCallDecision("Callee has RetBuf but caller does not.");
            return false;
        }
    }

    // For a fast tail call the caller will use its incoming arg stack space to place
    // arguments, so if the callee requires more arg stack space than is available here
    // the fast tail call cannot be performed. This is common to all platforms.
    // Note that the GC'ness of on stack args need not match since the arg setup area is marked
    // as non-interruptible for fast tail calls.
    if (calleeArgStackSize > callerArgStackSize)
    {
        reportFastTailCallDecision("Not enough incoming arg space");
        return false;
    }

    // For Windows some struct parameters are copied on the local frame
    // and then passed by reference. We cannot fast tail call in these situation
    // as we need to keep our frame around.
    if (fgCallHasMustCopyByrefParameter(callee))
    {
        reportFastTailCallDecision("Callee has a byref parameter");
        return false;
    }

    reportFastTailCallDecision(nullptr);
    return true;
#else // FEATURE_FASTTAILCALL
    if (failReason)
        *failReason = "Fast tailcalls are not supported on this platform";
    return false;
#endif
}

#if FEATURE_FASTTAILCALL
//------------------------------------------------------------------------
// fgCallHasMustCopyByrefParameter: Check to see if this call has a byref parameter that
//                                  requires a struct copy in the caller.
//
// Arguments:
//    call - The call to check
//
// Return Value:
//    Returns true or false based on whether this call has a byref parameter that
//    requires a struct copy in the caller.
//
bool Compiler::fgCallHasMustCopyByrefParameter(GenTreeCall* call)
{
#if FEATURE_IMPLICIT_BYREFS
    for (CallArg& arg : call->gtArgs.Args())
    {
        if (fgCallArgWillPointIntoLocalFrame(call, arg))
        {
            return true;
        }
    }
#endif

    return false;
}

//------------------------------------------------------------------------
// fgCallArgWillPointIntoLocalFrame:
//    Check to see if a call arg will end up pointing into the local frame after morph.
//
// Arguments:
//    call - The call to check
//
// Return Value:
//    True if the arg will be passed as an implicit byref pointing to a local
//    on this function's frame; otherwise false.
//
// Remarks:
//    The logic here runs before relevant nodes have been morphed.
//
bool Compiler::fgCallArgWillPointIntoLocalFrame(GenTreeCall* call, CallArg& arg)
{
    if (!arg.AbiInfo.IsPassedByReference())
    {
        return false;
    }

    // If we're optimizing, we may be able to pass our caller's byref to our callee,
    // and so still be able to avoid a struct copy.
    if (opts.OptimizationDisabled())
    {
        return true;
    }

    // First, see if this arg is an implicit byref param.
    GenTreeLclVarCommon* const lcl = arg.GetNode()->IsImplicitByrefParameterValuePreMorph(this);

    if (lcl == nullptr)
    {
        return true;
    }

    // Yes, the arg is an implicit byref param.
    const unsigned   lclNum = lcl->GetLclNum();
    LclVarDsc* const varDsc = lvaGetDesc(lcl);

    // The param must not be promoted; if we've promoted, then the arg will be
    // a local struct assembled from the promoted fields.
    if (varDsc->lvPromoted)
    {
        JITDUMP("Arg [%06u] is promoted implicit byref V%02u, so no tail call\n", dspTreeID(arg.GetNode()), lclNum);

        return true;
    }

    assert(!varDsc->lvIsStructField);

    JITDUMP("Arg [%06u] is unpromoted implicit byref V%02u, seeing if we can still tail call\n",
            dspTreeID(arg.GetNode()), lclNum);

    GenTreeFlags deathFlags;
    if (varDsc->lvFieldLclStart != 0)
    {
        // Undone promotion case.
        deathFlags = lvaGetDesc(varDsc->lvFieldLclStart)->AllFieldDeathFlags();
    }
    else
    {
        deathFlags = GTF_VAR_DEATH;
    }

    if ((lcl->gtFlags & deathFlags) == deathFlags)
    {
        JITDUMP("... yes, arg is a last use\n");
        return false;
    }

    JITDUMP("... no, arg is not a last use\n");
    return true;
}

#endif

//------------------------------------------------------------------------
// fgMorphPotentialTailCall: Attempt to morph a call that the importer has
// identified as a potential tailcall to an actual tailcall and return the
// placeholder node to use in this case.
//
// Arguments:
//    call - The call to morph.
//
// Return Value:
//    Returns a node to use if the call was morphed into a tailcall. If this
//    function returns a node the call is done being morphed and the new node
//    should be used. Otherwise the call will have been demoted to a regular call
//    and should go through normal morph.
//
// Notes:
//    This is called only for calls that the importer has already identified as
//    potential tailcalls. It will do profitability and legality checks and
//    classify which kind of tailcall we are able to (or should) do, along with
//    modifying the trees to perform that kind of tailcall.
//
GenTree* Compiler::fgMorphPotentialTailCall(GenTreeCall* call)
{
    // It should either be an explicit (i.e. tail prefixed) or an implicit tail call
    assert(call->IsTailPrefixedCall() ^ call->IsImplicitTailCall());

    // It cannot be an inline candidate
    assert(!call->IsInlineCandidate());

    auto failTailCall = [&](const char* reason, unsigned lclNum = BAD_VAR_NUM) {
#ifdef DEBUG
        if (verbose)
        {
            printf("\nRejecting tail call in morph for call ");
            printTreeID(call);
            printf(": %s", reason);
            if (lclNum != BAD_VAR_NUM)
            {
                printf(" V%02u", lclNum);
            }
            printf("\n");
        }
#endif

        // for non user funcs, we have no handles to report
        info.compCompHnd->reportTailCallDecision(nullptr,
                                                 (call->gtCallType == CT_USER_FUNC) ? call->gtCallMethHnd : nullptr,
                                                 call->IsTailPrefixedCall(), TAILCALL_FAIL, reason);

        // We have checked the candidate so demote.
        call->gtCallMoreFlags &= ~GTF_CALL_M_EXPLICIT_TAILCALL;
#if FEATURE_TAILCALL_OPT
        call->gtCallMoreFlags &= ~GTF_CALL_M_IMPLICIT_TAILCALL;
#endif
    };

    if (call->IsSpecialIntrinsic())
    {
        failTailCall("Might turn into an intrinsic");
        return nullptr;
    }

#ifdef TARGET_ARM
    if (call->gtCallMoreFlags & GTF_CALL_M_WRAPPER_DELEGATE_INV)
    {
        failTailCall("Non-standard calling convention");
        return nullptr;
    }
#endif

    if (call->IsNoReturn() && !call->IsTailPrefixedCall())
    {
        // Such tail calls always throw an exception and we won't be able to see current
        // Caller() in the stacktrace.
        failTailCall("Never returns");
        return nullptr;
    }

#ifdef DEBUG
    if (opts.compGcChecks && (info.compRetType == TYP_REF))
    {
        failTailCall("DOTNET_JitGCChecks or stress might have interposed a call to CORINFO_HELP_CHECK_OBJ, "
                     "invalidating tailcall opportunity");
        return nullptr;
    }
#endif

    if (compIsAsync() != call->IsAsync())
    {
        failTailCall("Caller and callee do not agree on async-ness");
        return nullptr;
    }

    // We have to ensure to pass the incoming retValBuf as the
    // outgoing one. Using a temp will not do as this function will
    // not regain control to do the copy. This can happen when inlining
    // a tailcall which also has a potential tailcall in it: the IL looks
    // like we can do a tailcall, but the trees generated use a temp for the inlinee's
    // result. TODO-CQ: Fix this.
    if (info.compRetBuffArg != BAD_VAR_NUM)
    {
        noway_assert(call->TypeIs(TYP_VOID));
        noway_assert(call->gtArgs.HasRetBuffer());
        GenTree* retValBuf = call->gtArgs.GetRetBufferArg()->GetNode();
        if (!retValBuf->OperIs(GT_LCL_VAR) || retValBuf->AsLclVarCommon()->GetLclNum() != info.compRetBuffArg)
        {
            failTailCall("Need to copy return buffer");
            return nullptr;
        }
    }

    // We are still not sure whether it can be a tail call. Because, when converting
    // a call to an implicit tail call, we must check that there are no locals with
    // their address taken.  If this is the case, we have to assume that the address
    // has been leaked and the current stack frame must live until after the final
    // call.

    // Verify that none of vars has lvHasLdAddrOp or IsAddressExposed() bit set. Note
    // that lvHasLdAddrOp is much more conservative.  We cannot just base it on
    // IsAddressExposed() alone since it is not guaranteed to be set on all VarDscs
    // during morph stage. The reason for also checking IsAddressExposed() is that in case
    // of vararg methods user args are marked as addr exposed but not lvHasLdAddrOp.
    // The combination of lvHasLdAddrOp and IsAddressExposed() though conservative allows us
    // never to be incorrect.
    //
    // TODO-Throughput: have a compiler level flag to indicate whether method has vars whose
    // address is taken. Such a flag could be set whenever lvHasLdAddrOp or IsAddressExposed()
    // is set. This avoids the need for iterating through all lcl vars of the current
    // method.  Right now throughout the code base we are not consistently using 'set'
    // method to set lvHasLdAddrOp and IsAddressExposed() flags.

    bool isImplicitOrStressTailCall = call->IsImplicitTailCall() || call->IsStressTailCall();
    if (isImplicitOrStressTailCall && compLocallocUsed)
    {
        failTailCall("Localloc used");
        return nullptr;
    }

#ifdef DEBUG
    // For explicit tailcalls the importer will avoid inserting stress
    // poisoning after them. However, implicit tailcalls are marked earlier and
    // we must filter those out here if we ended up adding any poisoning IR
    // after them.
    if (isImplicitOrStressTailCall && compPoisoningAnyImplicitByrefs)
    {
        failTailCall("STRESS_POISON_IMPLICIT_BYREFS has introduced IR after tailcall opportunity, invalidating");
        return nullptr;
    }
#endif

    bool hasStructParam = false;
    for (unsigned varNum = 0; varNum < lvaCount; varNum++)
    {
        LclVarDsc* varDsc = lvaGetDesc(varNum);

        // If the method is marked as an explicit tail call we will skip the
        // following three hazard checks.
        // We still must check for any struct parameters and set 'hasStructParam'
        // so that we won't transform the recursive tail call into a loop.
        //
        if (isImplicitOrStressTailCall)
        {
            if (varDsc->IsAddressExposed())
            {
                if (lvaIsImplicitByRefLocal(varNum))
                {
                    // The address of the implicit-byref is a non-address use of the pointer parameter.
                }
                else if (varDsc->lvIsStructField && lvaIsImplicitByRefLocal(varDsc->lvParentLcl))
                {
                    // The address of the implicit-byref's field is likewise a non-address use of the pointer
                    // parameter.
                }
                else if (varDsc->lvPromoted && (lvaTable[varDsc->lvFieldLclStart].lvParentLcl != varNum))
                {
                    // This temp was used for struct promotion bookkeeping.  It will not be used, and will have
                    // its ref count and address-taken flag reset in fgMarkDemotedImplicitByRefArgs.
                    assert(lvaIsImplicitByRefLocal(lvaTable[varDsc->lvFieldLclStart].lvParentLcl));
                    assert(fgGlobalMorph);
                }
                else if (varDsc->IsStackAllocatedObject())
                {
                    // Stack allocated objects currently cannot be passed to callees
                    // so won't be live at tail call sites.
                }
#if FEATURE_FIXED_OUT_ARGS
                else if (varNum == lvaOutgoingArgSpaceVar)
                {
                    // The outgoing arg space is exposed only at callees, which is ok for our purposes.
                }
#endif
                else
                {
                    failTailCall("Local address taken", varNum);
                    return nullptr;
                }
            }
            if (varDsc->lvPinned)
            {
                // A tail call removes the method from the stack, which means the pinning
                // goes away for the callee.  We can't allow that.
                failTailCall("Has Pinned Vars", varNum);
                return nullptr;
            }
        }

        if (varTypeIsStruct(varDsc->TypeGet()) && varDsc->lvIsParam)
        {
            hasStructParam = true;
            // This prevents transforming a recursive tail call into a loop
            // but doesn't prevent tail call optimization so we need to
            // look at the rest of parameters.
        }
    }

    const char* failReason      = nullptr;
    bool        canFastTailCall = fgCanFastTailCall(call, &failReason);

    CORINFO_TAILCALL_HELPERS tailCallHelpers;
    bool                     tailCallViaJitHelper = false;
    if (!canFastTailCall)
    {
        if (call->IsImplicitTailCall())
        {
            // Implicit or opportunistic tail calls are always dispatched via fast tail call
            // mechanism and never via tail call helper for perf.
            failTailCall(failReason);
            return nullptr;
        }

        assert(call->IsTailPrefixedCall());
        assert(call->tailCallInfo != nullptr);

        // We do not currently handle non-standard args except for VSD stubs.
        if (!call->IsVirtualStub() && call->HasNonStandardAddedArgs(this))
        {
            failTailCall(
                "Method with non-standard args passed in callee trash register cannot be tail called via helper");
            return nullptr;
        }

        // On x86 we have a faster mechanism than the general one which we use
        // in almost all cases. See fgCanTailCallViaJitHelper for more information.
        if (fgCanTailCallViaJitHelper(call))
        {
            tailCallViaJitHelper = true;
        }
        else
        {
            // Make sure we can get the helpers. We do this last as the runtime
            // will likely be required to generate these.
            CORINFO_RESOLVED_TOKEN* token = nullptr;
            CORINFO_SIG_INFO*       sig   = call->tailCallInfo->GetSig();
            unsigned                flags = 0;
            if (!call->tailCallInfo->IsCalli())
            {
                token = call->tailCallInfo->GetToken();
                if (call->tailCallInfo->IsCallvirt())
                {
                    flags |= CORINFO_TAILCALL_IS_CALLVIRT;
                }
            }

            if (call->gtArgs.HasThisPointer())
            {
                var_types thisArgType = call->gtArgs.GetThisArg()->GetNode()->TypeGet();
                if (thisArgType != TYP_REF)
                {
                    flags |= CORINFO_TAILCALL_THIS_ARG_IS_BYREF;
                }
            }

            if (!info.compCompHnd->getTailCallHelpers(token, sig, (CORINFO_GET_TAILCALL_HELPERS_FLAGS)flags,
                                                      &tailCallHelpers))
            {
                failTailCall("Tail call help not available");
                return nullptr;
            }
        }
    }

    // Check if we can make the tailcall a loop.
    bool fastTailCallToLoop = false;
#if FEATURE_TAILCALL_OPT
    // TODO-CQ: enable the transformation when the method has a struct parameter that can be passed in a register
    // or return type is a struct that can be passed in a register.
    //
    // TODO-CQ: if the method being compiled requires generic context reported in gc-info (either through
    // hidden generic context param or through keep alive thisptr), then while transforming a recursive
    // call to such a method requires that the generic context stored on stack slot be updated.  Right now,
    // fgMorphRecursiveFastTailCallIntoLoop() is not handling update of generic context while transforming
    // a recursive call into a loop.  Another option is to modify gtIsRecursiveCall() to check that the
    // generic type parameters of both caller and callee generic method are the same.
    //
    // For OSR, we prefer to tailcall for call counting + potential transition
    // into the actual tier1 version.
    //
    if (opts.compTailCallLoopOpt && canFastTailCall && !opts.IsOSR() && gtIsRecursiveCall(call) &&
        !lvaReportParamTypeArg() && !lvaKeepAliveAndReportThis() && !call->IsVirtual() && !hasStructParam &&
        !varTypeIsStruct(call->TypeGet()))
    {
        fastTailCallToLoop = true;
    }
#endif

    // Ok -- now we are committed to performing a tailcall. Report the decision.
    CorInfoTailCall tailCallResult;
    if (fastTailCallToLoop)
    {
        tailCallResult = TAILCALL_RECURSIVE;
    }
    else if (canFastTailCall)
    {
        tailCallResult = TAILCALL_OPTIMIZED;
    }
    else
    {
        tailCallResult = TAILCALL_HELPER;
    }

    info.compCompHnd->reportTailCallDecision(nullptr,
                                             (call->gtCallType == CT_USER_FUNC) ? call->gtCallMethHnd : nullptr,
                                             call->IsTailPrefixedCall(), tailCallResult, nullptr);

    // Do some profitability checks for whether we should expand a vtable call
    // target early. Note that we may already have expanded it due to GDV at
    // this point, so make sure we do not undo that work.
    //
    if (call->IsExpandedEarly() && call->IsVirtualVtable() && (call->gtControlExpr == nullptr))
    {
        assert(call->gtArgs.HasThisPointer());
        // It isn't always profitable to expand a virtual call early
        //
        // We always expand the TAILCALL_HELPER type late.
        // And we exapnd late when we have an optimized tail call
        // and the this pointer needs to be evaluated into a temp.
        //
        if (tailCallResult == TAILCALL_HELPER)
        {
            // We will always expand this late in lower instead.
            // (see LowerTailCallViaJitHelper as it needs some work
            // for us to be able to expand this earlier in morph)
            //
            call->ClearExpandedEarly();
        }
        else if ((tailCallResult == TAILCALL_OPTIMIZED) &&
                 ((call->gtArgs.GetThisArg()->GetNode()->gtFlags & GTF_SIDE_EFFECT) != 0))
        {
            // We generate better code when we expand this late in lower instead.
            //
            call->ClearExpandedEarly();
        }
    }

    // Now actually morph the call.
    compTailCallUsed = true;
    // This will prevent inlining this call.
    call->gtCallMoreFlags |= GTF_CALL_M_TAILCALL;
    if (tailCallViaJitHelper)
    {
        call->gtCallMoreFlags |= GTF_CALL_M_TAILCALL_VIA_JIT_HELPER;
    }

#if FEATURE_TAILCALL_OPT
    if (fastTailCallToLoop)
    {
        call->gtCallMoreFlags |= GTF_CALL_M_TAILCALL_TO_LOOP;
    }
#endif

    // Mark that this is no longer a pending tailcall. We need to do this before
    // we call fgMorphCall again (which happens in the fast tailcall case) to
    // avoid recursing back into this method.
    call->gtCallMoreFlags &= ~GTF_CALL_M_EXPLICIT_TAILCALL;
#if FEATURE_TAILCALL_OPT
    call->gtCallMoreFlags &= ~GTF_CALL_M_IMPLICIT_TAILCALL;
#endif

#ifdef DEBUG
    if (verbose)
    {
        printf("\nGTF_CALL_M_TAILCALL bit set for call ");
        printTreeID(call);
        printf("\n");
        if (fastTailCallToLoop)
        {
            printf("\nGTF_CALL_M_TAILCALL_TO_LOOP bit set for call ");
            printTreeID(call);
            printf("\n");
        }
    }
#endif

    // For R2R we might need a different entry point for this call if we are doing a tailcall.
    // The reason is that the normal delay load helper uses the return address to find the indirection
    // cell in xarch, but now the JIT is expected to leave the indirection cell in REG_R2R_INDIRECT_PARAM:
    // We optimize delegate invocations manually in the JIT so skip this for those.
    if (call->IsR2RRelativeIndir() && canFastTailCall && !fastTailCallToLoop && !call->IsDelegateInvoke())
    {
        info.compCompHnd->updateEntryPointForTailCall(&call->gtEntryPoint);

#ifdef TARGET_XARCH
        // We have already computed arg info to make the fast tailcall decision, but on X64 we now
        // have to pass the indirection cell, so redo arg info.
        call->gtArgs.ResetFinalArgsAndABIInfo();
#endif
    }

    fgValidateIRForTailCall(call);

    // If this block has a flow successor, make suitable updates.
    //
    if (compCurBB->KindIs(BBJ_ALWAYS))
    {
        BasicBlock* const curBlock    = compCurBB;
        BasicBlock* const targetBlock = curBlock->GetTarget();

        // Flow no longer reaches the target from here.
        //
        fgRemoveRefPred(curBlock->GetTargetEdge());

        // Adjust profile weights of the successor block.
        //
        // Note if this is a tail call to loop, further updates
        // are needed once we install the loop edge.
        //
        if (curBlock->hasProfileWeight() && targetBlock->hasProfileWeight())
        {
            targetBlock->decreaseBBProfileWeight(curBlock->bbWeight);

            if (targetBlock->NumSucc() > 0)
            {
                JITDUMP("Flow removal out of " FMT_BB " needs to be propagated. Data %s inconsistent.\n",
                        curBlock->bbNum, fgPgoConsistent ? "is now" : "was already");
                fgPgoConsistent = false;
            }
        }
    }
    else
    {
        // No unique successor. compCurBB should be a return.
        //
        assert(compCurBB->KindIs(BBJ_RETURN));
    }

#if !FEATURE_TAILCALL_OPT_SHARED_RETURN
    // We enable shared-ret tail call optimization for recursive calls even if
    // FEATURE_TAILCALL_OPT_SHARED_RETURN is not defined.
    if (gtIsRecursiveCall(call))
#endif
    {
        // Many tailcalls will have call and ret in the same block, and thus be
        // BBJ_RETURN, but if the call falls through to a ret, and we are doing a
        // tailcall, change it here.
        compCurBB->SetKindAndTargetEdge(BBJ_RETURN);
    }

    GenTree* stmtExpr = fgMorphStmt->GetRootNode();

#ifdef DEBUG
    // Tail call needs to be in one of the following IR forms
    //    Either a call stmt or
    //    GT_RETURN(GT_CALL(..)) or GT_RETURN(GT_CAST(GT_CALL(..)))
    //    var = GT_CALL(..) or var = (GT_CAST(GT_CALL(..)))
    //    GT_COMMA(GT_CALL(..), GT_NOP) or GT_COMMA(GT_CAST(GT_CALL(..)), GT_NOP)
    // In the above,
    //    GT_CASTS may be nested.
    genTreeOps stmtOper = stmtExpr->gtOper;
    if (stmtOper == GT_CALL)
    {
        assert(stmtExpr == call);
    }
    else
    {
        assert(stmtOper == GT_RETURN || stmtOper == GT_STORE_LCL_VAR || stmtOper == GT_COMMA);
        GenTree* treeWithCall;
        if (stmtOper == GT_RETURN)
        {
            treeWithCall = stmtExpr->gtGetOp1();
        }
        else if (stmtOper == GT_COMMA)
        {
            // Second operation must be nop.
            assert(stmtExpr->gtGetOp2()->IsNothingNode());
            treeWithCall = stmtExpr->gtGetOp1();
        }
        else
        {
            treeWithCall = stmtExpr->AsLclVar()->Data();
        }

        // Peel off casts
        while (treeWithCall->OperIs(GT_CAST))
        {
            assert(!treeWithCall->gtOverflow());
            treeWithCall = treeWithCall->gtGetOp1();
        }

        assert(treeWithCall == call);
    }
#endif
    // Store the call type for later to introduce the correct placeholder.
    var_types origCallType = call->TypeGet();

    GenTree* result;
    if (!canFastTailCall && !tailCallViaJitHelper)
    {
        // For tailcall via CORINFO_TAILCALL_HELPERS we transform into regular
        // calls with (to the JIT) regular control flow so we do not need to do
        // much special handling.
        result = fgMorphTailCallViaHelpers(call, tailCallHelpers);
    }
    else
    {
        // Otherwise we will transform into something that does not return. For
        // fast tailcalls a "jump" and for tailcall via JIT helper a call to a
        // JIT helper that does not return. So peel off everything after the
        // call.
        Statement* nextMorphStmt = fgMorphStmt->GetNextStmt();
        JITDUMP("Remove all stmts after the call.\n");
        while (nextMorphStmt != nullptr)
        {
            Statement* stmtToRemove = nextMorphStmt;
            nextMorphStmt           = stmtToRemove->GetNextStmt();
            fgRemoveStmt(compCurBB, stmtToRemove);
        }

        bool     isRootReplaced = false;
        GenTree* root           = fgMorphStmt->GetRootNode();

        if (root != call)
        {
            JITDUMP("Replace root node [%06d] with [%06d] tail call node.\n", dspTreeID(root), dspTreeID(call));
            isRootReplaced = true;
            fgMorphStmt->SetRootNode(call);
        }

        // Avoid potential extra work for the return (for example, vzeroupper)
        call->gtType = TYP_VOID;

        // The runtime requires that we perform a null check on the `this` argument before
        // tail calling to a virtual dispatch stub. This requirement is a consequence of limitations
        // in the runtime's ability to map an AV to a NullReferenceException if
        // the AV occurs in a dispatch stub that has unmanaged caller.
        if (call->IsVirtualStub())
        {
            call->gtFlags |= GTF_CALL_NULLCHECK;
        }

        // Do some target-specific transformations (before we process the args,
        // etc.) for the JIT helper case.
        if (tailCallViaJitHelper)
        {
            fgMorphTailCallViaJitHelper(call);

            // Force re-evaluating the argInfo. fgMorphTailCallViaJitHelper will modify the
            // argument list, invalidating the argInfo.
            call->gtArgs.ResetFinalArgsAndABIInfo();
        }

        // Tail call via JIT helper: The VM can't use return address hijacking
        // if we're not going to return and the helper doesn't have enough info
        // to safely poll, so we poll before the tail call, if the block isn't
        // already safe. Since tail call via helper is a slow mechanism it
        // doesn't matter whether we emit GC poll. This is done to be in parity
        // with Jit64. Also this avoids GC info size increase if all most all
        // methods are expected to be tail calls (e.g. F#).
        //
        // Note that we can avoid emitting GC-poll if we know that the current
        // BB is dominated by a Gc-SafePoint block. But we don't have dominator
        // info at this point. One option is to just add a place holder node for
        // GC-poll (e.g. GT_GCPOLL) here and remove it in lowering if the block
        // is dominated by a GC-SafePoint. For now it not clear whether
        // optimizing slow tail calls is worth the effort. As a low cost check,
        // we check whether the first and current basic blocks are
        // GC-SafePoints.
        //
        // Fast Tail call as epilog+jmp - No need to insert GC-poll. Instead,
        // fgSetBlockOrder() is going to mark the method as fully interruptible
        // if the block containing this tail call is reachable without executing
        // any call.
        if (canFastTailCall || fgFirstBB->HasFlag(BBF_GC_SAFE_POINT) || compCurBB->HasFlag(BBF_GC_SAFE_POINT))
        {
            // No gc poll needed
        }
        else
        {
            JITDUMP("Marking " FMT_BB " as needs gc poll\n", compCurBB->bbNum);
            compCurBB->SetFlags(BBF_NEEDS_GCPOLL);
            optMethodFlags |= OMF_NEEDS_GCPOLLS;
        }

        fgMorphCall(call);

        // Fast tail call: in case of fast tail calls, we need a jmp epilog and
        // hence mark it as BBJ_RETURN with BBF_JMP flag set.
        noway_assert(compCurBB->KindIs(BBJ_RETURN));
        if (canFastTailCall)
        {
            compCurBB->SetFlags(BBF_HAS_JMP);
        }
        else
        {
            // We call CORINFO_HELP_TAILCALL which does not return, so we will
            // not need epilogue.
            compCurBB->SetKindAndTargetEdge(BBJ_THROW);
        }

        if (isRootReplaced)
        {
            call->SetMorphed(this);

            // We have replaced the root node of this stmt and deleted the rest,
            // but we still have the deleted, dead nodes on the `fgMorph*` stack
            // if the root node was a store, `RET` or `CAST`.
            // Return a zero con node to exit morphing of the old trees without asserts
            // and forbid POST_ORDER morphing doing something wrong with our call.
            var_types zeroType = (origCallType == TYP_STRUCT) ? TYP_INT : genActualType(origCallType);
            result             = fgMorphTree(gtNewZeroConNode(zeroType));
        }
        else
        {
            result = call;
        }
    }

    return result;
}

//------------------------------------------------------------------------
// fgValidateIRForTailCall:
//     Validate that the IR looks ok to perform a tailcall.
//
// Arguments:
//     call - The call that we are dispatching as a tailcall.
//
// Notes:
//   This function needs to handle somewhat complex IR that appears after
//   tailcall candidates due to inlining.
//   Does not support checking struct returns since physical promotion can
//   create very hard to validate IR patterns.
//
void Compiler::fgValidateIRForTailCall(GenTreeCall* call)
{
#ifdef DEBUG
    if (call->TypeIs(TYP_STRUCT))
    {
        // Due to struct fields it can be very hard to track valid return
        // patterns; just give up on validating those.
        return;
    }

    class TailCallIRValidatorVisitor final : public GenTreeVisitor<TailCallIRValidatorVisitor>
    {
        GenTreeCall* m_tailcall;
        unsigned     m_lclNum;
        bool         m_active;

    public:
        enum
        {
            DoPostOrder       = true,
            UseExecutionOrder = true,
        };

        TailCallIRValidatorVisitor(Compiler* comp, GenTreeCall* tailcall)
            : GenTreeVisitor(comp)
            , m_tailcall(tailcall)
            , m_lclNum(BAD_VAR_NUM)
            , m_active(false)
        {
        }

        fgWalkResult PostOrderVisit(GenTree** use, GenTree* user)
        {
            GenTree* tree = *use;

            // Wait until we get to the actual call...
            if (!m_active)
            {
                if (tree == m_tailcall)
                {
                    m_active = true;
                }

                return WALK_CONTINUE;
            }

            if (tree->OperIs(GT_RETURN))
            {
                assert((tree->TypeIs(TYP_VOID) || ValidateUse(tree->gtGetOp1())) &&
                       "Expected return to be result of tailcall");
                return WALK_ABORT;
            }

            if (tree->OperIs(GT_NOP))
            {
                // GT_NOP might appear due to stores that end up as
                // self-stores, which get morphed to GT_NOP.
            }
            // We might see arbitrary chains of stores that trivially
            // propagate the result. Example:
            //
            //    *  STORE_LCL_VAR   ref    V05 tmp5
            //    \--*  CALL      ref    CultureInfo.InitializeUserDefaultUICulture
            // (in a new statement/BB)
            //    *  STORE_LCL_VAR   ref    V02 tmp2
            //    \--*  LCL_VAR   ref    V05 tmp5
            // (in a new statement/BB)
            //    *  RETURN    ref
            //    \--*  LCL_VAR   ref    V02 tmp2
            //
            else if (tree->OperIs(GT_STORE_LCL_VAR))
            {
                assert(ValidateUse(tree->AsLclVar()->Data()) && "Expected value of store to be result of tailcall");
                m_lclNum = tree->AsLclVar()->GetLclNum();
            }
            else if (tree->OperIs(GT_LCL_VAR))
            {
                assert(ValidateUse(tree) && "Expected use of local to be tailcall value");
            }
            else if (IsCommaNop(tree))
            {
                // COMMA(NOP,NOP)
            }
            else
            {
                DISPTREE(tree);
                assert(!"Unexpected tree op after call marked as tailcall");
            }

            return WALK_CONTINUE;
        }

        bool IsCommaNop(GenTree* node)
        {
            if (!node->OperIs(GT_COMMA))
            {
                return false;
            }

            return node->AsOp()->gtGetOp1()->OperIs(GT_NOP) && node->AsOp()->gtGetOp2()->OperIs(GT_NOP);
        }

        bool ValidateUse(GenTree* node)
        {
            if (m_lclNum != BAD_VAR_NUM)
            {
                return node->OperIs(GT_LCL_VAR) && (node->AsLclVar()->GetLclNum() == m_lclNum);
            }

            if (node == m_tailcall)
            {
                return true;
            }

            // If we do not use the call value directly we might have passed
            // this function's ret buffer arg, so verify that is being used.
            CallArg* retBufferArg = m_tailcall->gtArgs.GetRetBufferArg();
            if (retBufferArg != nullptr)
            {
                GenTree* retBufferNode = retBufferArg->GetNode();
                return retBufferNode->OperIs(GT_LCL_VAR) &&
                       (retBufferNode->AsLclVar()->GetLclNum() == m_compiler->info.compRetBuffArg) &&
                       node->OperIs(GT_LCL_VAR) && (node->AsLclVar()->GetLclNum() == m_compiler->info.compRetBuffArg);
            }

            return false;
        }
    };

    TailCallIRValidatorVisitor visitor(this, call);
    for (Statement* stmt = compCurStmt; stmt != nullptr; stmt = stmt->GetNextStmt())
    {
        visitor.WalkTree(stmt->GetRootNodePointer(), nullptr);
    }

    BasicBlock* bb = compCurBB;
    while (!bb->KindIs(BBJ_RETURN))
    {
        bb = bb->GetUniqueSucc();
        assert((bb != nullptr) && "Expected straight flow after tailcall");

        for (Statement* stmt : bb->Statements())
        {
            visitor.WalkTree(stmt->GetRootNodePointer(), nullptr);
        }
    }
#endif
}

//------------------------------------------------------------------------
// fgMorphTailCallViaHelpers: Transform the given GT_CALL tree for tailcall code
// generation.
//
// Arguments:
//     call - The call to transform
//     helpers - The tailcall helpers provided by the runtime.
//
// Return Value:
//    Returns the transformed node.
//
// Notes:
//   This transforms
//     GT_CALL
//         {callTarget}
//         {this}
//         {args}
//   into
//     GT_COMMA
//       GT_CALL StoreArgsStub
//         {callTarget}         (depending on flags provided by the runtime)
//         {this}               (as a regular arg)
//         {args}
//       GT_COMMA
//         GT_CALL Dispatcher
//           GT_LCL_ADDR ReturnAddress
//           {CallTargetStub}
//           GT_LCL_ADDR ReturnValue
//         GT_LCL ReturnValue
// whenever the call node returns a value. If the call node does not return a
// value the last comma will not be there.
//
GenTree* Compiler::fgMorphTailCallViaHelpers(GenTreeCall* call, CORINFO_TAILCALL_HELPERS& help)
{
    // R2R requires different handling but we don't support tailcall via
    // helpers in R2R yet, so just leave it for now.
    // TODO: R2R: TailCallViaHelper
    assert(!IsAot());

    JITDUMP("fgMorphTailCallViaHelpers (before):\n");
    DISPTREE(call);

    // Don't support tail calling helper methods
    assert(!call->IsHelperCall());

    // We come this route only for tail prefixed calls that cannot be dispatched as
    // fast tail calls
    assert(!call->IsImplicitTailCall());

    // We want to use the following assert, but it can modify the IR in some cases, so we
    // can't do that in an assert.
    // assert(!fgCanFastTailCall(call, nullptr));

    // We might or might not have called AddFinalArgsAndDetermineABIInfo before
    // this point: in builds with FEATURE_FASTTAILCALL we will have called it
    // when checking if we could do a fast tailcall, so it is possible we have
    // added extra IR for non-standard args that we must get rid of. Get rid of
    // the extra arguments here.
    call->gtArgs.ResetFinalArgsAndABIInfo();

    GenTree* callDispatcherAndGetResult = fgCreateCallDispatcherAndGetResult(call, help.hCallTarget, help.hDispatcher);

    // Change the call to a call to the StoreArgs stub.
    if (call->gtArgs.HasRetBuffer())
    {
        JITDUMP("Removing retbuf");

        call->gtArgs.Remove(call->gtArgs.GetRetBufferArg());
        call->gtCallMoreFlags &= ~GTF_CALL_M_RETBUFFARG;
    }

    const bool stubNeedsTargetFnPtr = (help.flags & CORINFO_TAILCALL_STORE_TARGET) != 0;

    GenTree* doBeforeStoreArgsStub = nullptr;
    GenTree* thisPtrStubArg        = nullptr;

    // Put 'this' in normal param list
    if (call->gtArgs.HasThisPointer())
    {
        JITDUMP("Moving this pointer into arg list\n");
        CallArg* thisArg = call->gtArgs.GetThisArg();
        GenTree* objp    = thisArg->GetNode();
        GenTree* thisPtr = nullptr;

        // JIT will need one or two copies of "this" in the following cases:
        //   1) the call needs null check;
        //   2) StoreArgs stub needs the target function pointer address and if the call is virtual
        //      the stub also needs "this" in order to evaluate the target.

        const bool callNeedsNullCheck = call->NeedsNullCheck();
        const bool stubNeedsThisPtr   = stubNeedsTargetFnPtr && call->IsVirtual();

        if (callNeedsNullCheck || stubNeedsThisPtr)
        {
            // Clone "this" if "this" has no side effects.
            if ((objp->gtFlags & GTF_SIDE_EFFECT) == 0)
            {
                thisPtr = gtClone(objp, true);
            }

            // Create a temp and spill "this" to the temp if "this" has side effects or "this" was too complex to clone.
            if (thisPtr == nullptr)
            {
                const unsigned lclNum = lvaGrabTemp(true DEBUGARG("tail call thisptr"));

                // tmp = "this"
                doBeforeStoreArgsStub = gtNewTempStore(lclNum, objp);

                if (callNeedsNullCheck)
                {
                    // COMMA(tmp = "this", deref(tmp))
                    GenTree* tmp          = gtNewLclvNode(lclNum, objp->TypeGet());
                    GenTree* nullcheck    = gtNewNullCheck(tmp);
                    doBeforeStoreArgsStub = gtNewOperNode(GT_COMMA, TYP_VOID, doBeforeStoreArgsStub, nullcheck);
                }

                thisPtr = gtNewLclvNode(lclNum, objp->TypeGet());

                if (stubNeedsThisPtr)
                {
                    thisPtrStubArg = gtNewLclvNode(lclNum, objp->TypeGet());
                }
            }
            else
            {
                if (callNeedsNullCheck)
                {
                    // deref("this")
                    doBeforeStoreArgsStub = gtNewNullCheck(objp);

                    if (stubNeedsThisPtr)
                    {
                        thisPtrStubArg = gtClone(objp, true);
                    }
                }
                else
                {
                    assert(stubNeedsThisPtr);

                    thisPtrStubArg = objp;
                }
            }

            call->gtFlags &= ~GTF_CALL_NULLCHECK;

            assert((thisPtrStubArg != nullptr) == stubNeedsThisPtr);
        }
        else
        {
            thisPtr = objp;
        }

        // During rationalization tmp="this" and null check will be materialized
        // in the right execution order.
        call->gtArgs.PushFront(this, NewCallArg::Primitive(thisPtr, thisArg->GetSignatureType()));
        call->gtArgs.Remove(thisArg);
    }

    // We may need to pass the target, for instance for calli or generic methods
    // where we pass instantiating stub.
    if (stubNeedsTargetFnPtr)
    {
        JITDUMP("Adding target since VM requested it\n");
        GenTree* target;
        if (!call->IsVirtual())
        {
            if (call->gtCallType == CT_INDIRECT)
            {
                noway_assert(call->gtCallAddr != nullptr);
                target = call->gtCallAddr;
            }
            else
            {
                CORINFO_CONST_LOOKUP addrInfo;
                info.compCompHnd->getFunctionEntryPoint(call->gtCallMethHnd, &addrInfo);

                CORINFO_GENERIC_HANDLE handle       = nullptr;
                void*                  pIndirection = nullptr;
                assert(addrInfo.accessType != IAT_PPVALUE && addrInfo.accessType != IAT_RELPVALUE);

                if (addrInfo.accessType == IAT_VALUE)
                {
                    handle = addrInfo.handle;
                }
                else if (addrInfo.accessType == IAT_PVALUE)
                {
                    pIndirection = addrInfo.addr;
                }
                target = gtNewIconEmbHndNode(handle, pIndirection, GTF_ICON_FTN_ADDR, call->gtCallMethHnd);
            }
        }
        else
        {
            assert(!call->tailCallInfo->GetSig()->hasTypeArg());

            CORINFO_CALL_INFO callInfo;
            unsigned          flags = CORINFO_CALLINFO_LDFTN;
            if (call->tailCallInfo->IsCallvirt())
            {
                flags |= CORINFO_CALLINFO_CALLVIRT;
            }

            eeGetCallInfo(call->tailCallInfo->GetToken(), nullptr, (CORINFO_CALLINFO_FLAGS)flags, &callInfo);
            target = getVirtMethodPointerTree(thisPtrStubArg, call->tailCallInfo->GetToken(), &callInfo);
        }

        call->gtArgs.PushBack(this, NewCallArg::Primitive(target));
    }

    // This is now a direct call to the store args stub and not a tailcall.
    call->gtCallType    = CT_USER_FUNC;
    call->gtCallMethHnd = help.hStoreArgs;
    call->gtFlags &= ~GTF_CALL_VIRT_KIND_MASK;
    call->gtCallMoreFlags &= ~(GTF_CALL_M_TAILCALL | GTF_CALL_M_DELEGATE_INV | GTF_CALL_M_WRAPPER_DELEGATE_INV);

    // The store-args stub returns no value.
    call->gtRetClsHnd  = nullptr;
    call->gtType       = TYP_VOID;
    call->gtReturnType = TYP_VOID;

    GenTree* callStoreArgsStub = call;

    if (doBeforeStoreArgsStub != nullptr)
    {
        callStoreArgsStub = gtNewOperNode(GT_COMMA, TYP_VOID, doBeforeStoreArgsStub, callStoreArgsStub);
    }

    GenTree* finalTree =
        gtNewOperNode(GT_COMMA, callDispatcherAndGetResult->TypeGet(), callStoreArgsStub, callDispatcherAndGetResult);

    finalTree = fgMorphTree(finalTree);

    JITDUMP("fgMorphTailCallViaHelpers (after):\n");
    DISPTREE(finalTree);
    return finalTree;
}

//------------------------------------------------------------------------
// fgCreateCallDispatcherAndGetResult: Given a call
// CALL
//   {callTarget}
//   {retbuf}
//   {this}
//   {args}
// create a similarly typed node that calls the tailcall dispatcher and returns
// the result, as in the following:
// COMMA
//   CALL TailCallDispatcher
//     ADDR ReturnAddress
//     &CallTargetFunc
//     ADDR RetValue
//   RetValue
// If the call has type TYP_VOID, only create the CALL node.
//
// Arguments:
//    origCall - the call
//    callTargetStubHnd - the handle of the CallTarget function (this is a special
//    IL stub created by the runtime)
//    dispatcherHnd - the handle of the tailcall dispatcher function
//
// Return Value:
//    A node that can be used in place of the original call.
//
GenTree* Compiler::fgCreateCallDispatcherAndGetResult(GenTreeCall*          origCall,
                                                      CORINFO_METHOD_HANDLE callTargetStubHnd,
                                                      CORINFO_METHOD_HANDLE dispatcherHnd)
{
    GenTreeCall* callDispatcherNode = gtNewCallNode(CT_USER_FUNC, dispatcherHnd, TYP_VOID, fgMorphStmt->GetDebugInfo());
    // The dispatcher has signature
    // void DispatchTailCalls(void* callersRetAddrSlot, void* callTarget, ref byte retValue)

    // Add return value arg.
    GenTree*     retValArg;
    GenTree*     retVal    = nullptr;
    unsigned int newRetLcl = BAD_VAR_NUM;

    if (origCall->gtArgs.HasRetBuffer())
    {
        JITDUMP("Transferring retbuf\n");
        GenTree* retBufArg = origCall->gtArgs.GetRetBufferArg()->GetNode();

        assert(info.compRetBuffArg != BAD_VAR_NUM);
        assert(retBufArg->OperIsLocal());
        assert(retBufArg->AsLclVarCommon()->GetLclNum() == info.compRetBuffArg);

        retValArg = retBufArg;

        if (!origCall->TypeIs(TYP_VOID))
        {
            retVal = gtClone(retBufArg);
        }
    }
    else if (!origCall->TypeIs(TYP_VOID))
    {
        JITDUMP("Creating a new temp for the return value\n");
        newRetLcl = lvaGrabTemp(false DEBUGARG("Return value for tail call dispatcher"));
        if (varTypeIsStruct(origCall->gtType))
        {
            lvaSetStruct(newRetLcl, origCall->gtRetClsHnd, false);
        }
        else
        {
            // Since we pass a reference to the return value to the dispatcher
            // we need to use the real return type so we can normalize it on
            // load when we return it.
            lvaTable[newRetLcl].lvType = (var_types)origCall->gtReturnType;
        }

        lvaSetVarAddrExposed(newRetLcl DEBUGARG(AddressExposedReason::DISPATCH_RET_BUF));

        if (varTypeIsStruct(origCall) && compMethodReturnsMultiRegRetType())
        {
            lvaGetDesc(newRetLcl)->lvIsMultiRegRet = true;
        }

        retValArg = gtNewLclVarAddrNode(newRetLcl);
        retVal    = gtNewLclvNode(newRetLcl, genActualType(lvaTable[newRetLcl].lvType));
    }
    else
    {
        JITDUMP("No return value so using null pointer as arg\n");
        retValArg = gtNewZeroConNode(TYP_I_IMPL);
    }

    // Args are (void** callersReturnAddressSlot, void* callTarget, ref byte retVal)
    GenTree* callTarget = new (this, GT_FTN_ADDR) GenTreeFptrVal(TYP_I_IMPL, callTargetStubHnd);

    // Add the caller's return address slot.
    if (lvaRetAddrVar == BAD_VAR_NUM)
    {
        lvaRetAddrVar                  = lvaGrabTemp(false DEBUGARG("Return address"));
        lvaTable[lvaRetAddrVar].lvType = TYP_I_IMPL;
        lvaSetVarAddrExposed(lvaRetAddrVar DEBUGARG(AddressExposedReason::DISPATCH_RET_BUF));
    }

    GenTree* retAddrSlot = gtNewLclVarAddrNode(lvaRetAddrVar);

    NewCallArg retAddrSlotArg = NewCallArg::Primitive(retAddrSlot);
    NewCallArg callTargetArg  = NewCallArg::Primitive(callTarget);
    NewCallArg retValCallArg  = NewCallArg::Primitive(retValArg);
    callDispatcherNode->gtArgs.PushFront(this, retAddrSlotArg, callTargetArg, retValCallArg);

    if (origCall->TypeIs(TYP_VOID))
    {
        return callDispatcherNode;
    }

    assert(retVal != nullptr);
    GenTree* comma = gtNewOperNode(GT_COMMA, origCall->TypeGet(), callDispatcherNode, retVal);

    // The JIT seems to want to CSE this comma and messes up multi-reg ret
    // values in the process. Just avoid CSE'ing this tree entirely in that
    // case.
    if (origCall->HasMultiRegRetVal())
    {
        comma->gtFlags |= GTF_DONT_CSE;
    }

    return comma;
}

//------------------------------------------------------------------------
// getLookupTree: get a lookup tree
//
// Arguments:
//    pResolvedToken - resolved token of the call
//    pLookup - the lookup to get the tree for
//    handleFlags - flags to set on the result node
//    compileTimeHandle - compile-time handle corresponding to the lookup
//
// Return Value:
//    A node representing the lookup tree
//
GenTree* Compiler::getLookupTree(CORINFO_RESOLVED_TOKEN* pResolvedToken,
                                 CORINFO_LOOKUP*         pLookup,
                                 GenTreeFlags            handleFlags,
                                 void*                   compileTimeHandle)
{
    if (!pLookup->lookupKind.needsRuntimeLookup)
    {
        // No runtime lookup is required.
        // Access is direct or memory-indirect (of a fixed address) reference

        CORINFO_GENERIC_HANDLE handle       = nullptr;
        void*                  pIndirection = nullptr;
        assert(pLookup->constLookup.accessType != IAT_PPVALUE && pLookup->constLookup.accessType != IAT_RELPVALUE);

        if (pLookup->constLookup.accessType == IAT_VALUE)
        {
            handle = pLookup->constLookup.handle;
        }
        else if (pLookup->constLookup.accessType == IAT_PVALUE)
        {
            pIndirection = pLookup->constLookup.addr;
        }

        return gtNewIconEmbHndNode(handle, pIndirection, handleFlags, compileTimeHandle);
    }

    return getRuntimeLookupTree(pResolvedToken, pLookup, compileTimeHandle);
}

//------------------------------------------------------------------------
// getRuntimeLookupTree: get a tree for a runtime lookup
//
// Arguments:
//    pResolvedToken - resolved token of the call
//    pLookup - the lookup to get the tree for
//    compileTimeHandle - compile-time handle corresponding to the lookup
//
// Return Value:
//    A node representing the runtime lookup tree
//
GenTree* Compiler::getRuntimeLookupTree(CORINFO_RESOLVED_TOKEN* pResolvedToken,
                                        CORINFO_LOOKUP*         pLookup,
                                        void*                   compileTimeHandle)
{
    assert(!compIsForInlining());

    CORINFO_RUNTIME_LOOKUP* pRuntimeLookup = &pLookup->runtimeLookup;

    // If pRuntimeLookup->indirections is equal to CORINFO_USEHELPER, it specifies that a run-time helper should be
    // used; otherwise, it specifies the number of indirections via pRuntimeLookup->offsets array.
    if ((pRuntimeLookup->indirections == CORINFO_USEHELPER) || (pRuntimeLookup->indirections == CORINFO_USENULL) ||
        pRuntimeLookup->testForNull)
    {
        return gtNewRuntimeLookupHelperCallNode(pRuntimeLookup,
                                                getRuntimeContextTree(pLookup->lookupKind.runtimeLookupKind),
                                                compileTimeHandle);
    }

    GenTree* result = getRuntimeContextTree(pLookup->lookupKind.runtimeLookupKind);

    ArrayStack<GenTree*> stmts(getAllocator(CMK_ArrayStack));

    auto cloneTree = [&](GenTree** tree DEBUGARG(const char* reason)) -> GenTree* {
        if (!((*tree)->gtFlags & GTF_GLOB_EFFECT))
        {
            GenTree* clone = gtClone(*tree, true);

            if (clone)
            {
                return clone;
            }
        }

        unsigned temp = lvaGrabTemp(true DEBUGARG(reason));
        stmts.Push(gtNewTempStore(temp, *tree));
        *tree = gtNewLclvNode(temp, lvaGetActualType(temp));
        return gtNewLclvNode(temp, lvaGetActualType(temp));
    };

    // Apply repeated indirections
    for (WORD i = 0; i < pRuntimeLookup->indirections; i++)
    {
        GenTree* preInd = nullptr;
        if ((i == 1 && pRuntimeLookup->indirectFirstOffset) || (i == 2 && pRuntimeLookup->indirectSecondOffset))
        {
            preInd = cloneTree(&result DEBUGARG("getRuntimeLookupTree indirectOffset"));
        }

        if (i != 0)
        {
            result = gtNewIndir(TYP_I_IMPL, result, GTF_IND_NONFAULTING | GTF_IND_INVARIANT);
        }

        if ((i == 1 && pRuntimeLookup->indirectFirstOffset) || (i == 2 && pRuntimeLookup->indirectSecondOffset))
        {
            result = gtNewOperNode(GT_ADD, TYP_I_IMPL, preInd, result);
        }

        if (pRuntimeLookup->offsets[i] != 0)
        {
            result = gtNewOperNode(GT_ADD, TYP_I_IMPL, result, gtNewIconNode(pRuntimeLookup->offsets[i], TYP_I_IMPL));
        }
    }

    assert(!pRuntimeLookup->testForNull);
    if (pRuntimeLookup->indirections > 0)
    {
        result = gtNewIndir(TYP_I_IMPL, result, GTF_IND_NONFAULTING);
    }

    // Produces GT_COMMA(stmt1, GT_COMMA(stmt2, ... GT_COMMA(stmtN, result)))

    while (!stmts.Empty())
    {
        result = gtNewOperNode(GT_COMMA, TYP_I_IMPL, stmts.Pop(), result);
    }

    DISPTREE(result);
    return result;
}

//------------------------------------------------------------------------
// getVirtMethodPointerTree: get a tree for a virtual method pointer
//
// Arguments:
//    thisPtr - tree representing `this` pointer
//    pResolvedToken - pointer to the resolved token of the method
//    pCallInfo - pointer to call info
//
// Return Value:
//    A node representing the virtual method pointer

GenTree* Compiler::getVirtMethodPointerTree(GenTree*                thisPtr,
                                            CORINFO_RESOLVED_TOKEN* pResolvedToken,
                                            CORINFO_CALL_INFO*      pCallInfo)
{
    GenTree* exactTypeDesc   = getTokenHandleTree(pResolvedToken, true);
    GenTree* exactMethodDesc = getTokenHandleTree(pResolvedToken, false);

    return gtNewHelperCallNode(CORINFO_HELP_VIRTUAL_FUNC_PTR, TYP_I_IMPL, thisPtr, exactTypeDesc, exactMethodDesc);
}

//------------------------------------------------------------------------
// getTokenHandleTree: get a handle tree for a token. This method should never
//    be called for tokens imported from inlinees.
//
// Arguments:
//    pResolvedToken - token to get a handle for
//    parent - whether parent should be imported
//
// Return Value:
//    A node representing the virtual method pointer

GenTree* Compiler::getTokenHandleTree(CORINFO_RESOLVED_TOKEN* pResolvedToken, bool parent)
{
    CORINFO_GENERICHANDLE_RESULT embedInfo;

    // NOTE: inlining is done at this point, so we don't know which method contained this token.
    // It's fine because currently this is never used for something that belongs to an inlinee.
    // Namely, we currently use it for:
    //   1) Methods with EH are never inlined
    //   2) Methods with explicit tail calls are never inlined
    //
    info.compCompHnd->embedGenericHandle(pResolvedToken, parent, info.compMethodHnd, &embedInfo);

    GenTree* result = getLookupTree(pResolvedToken, &embedInfo.lookup, gtTokenToIconFlags(pResolvedToken->token),
                                    embedInfo.compileTimeHandle);

    // If we have a result and it requires runtime lookup, wrap it in a runtime lookup node.
    if ((result != nullptr) && embedInfo.lookup.lookupKind.needsRuntimeLookup)
    {
        result = gtNewRuntimeLookup(embedInfo.compileTimeHandle, embedInfo.handleType, result);
    }

    return result;
}

/*****************************************************************************
 *
 *  Transform the given GT_CALL tree for tail call via JIT helper.
 */
void Compiler::fgMorphTailCallViaJitHelper(GenTreeCall* call)
{
    JITDUMP("fgMorphTailCallViaJitHelper (before):\n");
    DISPTREE(call);

    // For the helper-assisted tail calls, we need to push all the arguments
    // into a single list, and then add a few extra at the beginning or end.
    //
    // For Windows x86, the tailcall helper is defined as:
    //
    //      JIT_TailCall(<function args>, int numberOfOldStackArgsWords, int numberOfNewStackArgsWords, int flags, void*
    //      callTarget)
    //
    // Note that the special arguments are on the stack, whereas the function arguments follow
    // the normal convention: there might be register arguments in ECX and EDX. The stack will
    // look like (highest address at the top):
    //      first normal stack argument
    //      ...
    //      last normal stack argument
    //      numberOfOldStackArgs
    //      numberOfNewStackArgs
    //      flags
    //      callTarget
    //
    // Each special arg is 4 bytes.
    //
    // 'flags' is a bitmask where:
    //      1 == restore callee-save registers (EDI,ESI,EBX). The JIT always saves all
    //          callee-saved registers for tailcall functions. Note that the helper assumes
    //          that the callee-saved registers live immediately below EBP, and must have been
    //          pushed in this order: EDI, ESI, EBX.
    //      2 == call target is a virtual stub dispatch.
    //
    // The x86 tail call helper lives in VM\i386\jithelp.asm. See that function for more details
    // on the custom calling convention.

    // Check for PInvoke call types that we don't handle in codegen yet.
    assert(!call->IsUnmanaged());
    assert(call->IsVirtual() || (call->gtCallType != CT_INDIRECT) || (call->gtCallCookie == nullptr));

    // Don't support tail calling helper methods
    assert(!call->IsHelperCall());

    // We come this route only for tail prefixed calls that cannot be dispatched as
    // fast tail calls
    assert(!call->IsImplicitTailCall());

    // We want to use the following assert, but it can modify the IR in some cases, so we
    // can't do that in an assert.
    // assert(!fgCanFastTailCall(call, nullptr));

    // First move the 'this' pointer (if any) onto the regular arg list. We do this because
    // we are going to prepend special arguments onto the argument list (for non-x86 platforms),
    // and thus shift where the 'this' pointer will be passed to a later argument slot. In
    // addition, for all platforms, we are going to change the call into a helper call. Our code
    // generation code for handling calls to helpers does not handle 'this' pointers. So, when we
    // do this transformation, we must explicitly create a null 'this' pointer check, if required,
    // since special 'this' pointer handling will no longer kick in.
    //
    // Some call types, such as virtual vtable calls, require creating a call address expression
    // that involves the "this" pointer. Lowering will sometimes create an embedded statement
    // to create a temporary that is assigned to the "this" pointer expression, and then use
    // that temp to create the call address expression. This temp creation embedded statement
    // will occur immediately before the "this" pointer argument, and then will be used for both
    // the "this" pointer argument as well as the call address expression. In the normal ordering,
    // the embedded statement establishing the "this" pointer temp will execute before both uses
    // of the temp. However, for tail calls via a helper, we move the "this" pointer onto the
    // normal call argument list, and insert a placeholder which will hold the call address
    // expression. For non-x86, things are ok, because the order of execution of these is not
    // altered. However, for x86, the call address expression is inserted as the *last* argument
    // in the argument list, *after* the "this" pointer. It will be put on the stack, and be
    // evaluated first. To ensure we don't end up with out-of-order temp definition and use,
    // for those cases where call lowering creates an embedded form temp of "this", we will
    // create a temp here, early, that will later get morphed correctly.

    CallArg* thisArg = call->gtArgs.GetThisArg();
    if (thisArg != nullptr)
    {
        GenTree* thisPtr = nullptr;
        GenTree* objp    = thisArg->GetNode();

        if ((call->IsDelegateInvoke() || call->IsVirtualVtable()) && !objp->OperIs(GT_LCL_VAR))
        {
            // tmp = "this"
            unsigned lclNum = lvaGrabTemp(true DEBUGARG("tail call thisptr"));
            GenTree* store  = gtNewTempStore(lclNum, objp);

            // COMMA(tmp = "this", tmp)
            var_types vt  = objp->TypeGet();
            GenTree*  tmp = gtNewLclvNode(lclNum, vt);
            thisPtr       = gtNewOperNode(GT_COMMA, vt, store, tmp);

            objp = thisPtr;
        }

        if (call->NeedsNullCheck())
        {
            // clone "this" if "this" has no side effects.
            if ((thisPtr == nullptr) && !(objp->gtFlags & GTF_SIDE_EFFECT))
            {
                thisPtr = gtClone(objp, true);
            }

            var_types vt = objp->TypeGet();
            if (thisPtr == nullptr)
            {
                // create a temp if either "this" has side effects or "this" is too complex to clone.

                // tmp = "this"
                unsigned lclNum = lvaGrabTemp(true DEBUGARG("tail call thisptr"));
                GenTree* store  = gtNewTempStore(lclNum, objp);

                // COMMA(tmp = "this", deref(tmp))
                GenTree* tmp       = gtNewLclvNode(lclNum, vt);
                GenTree* nullcheck = gtNewNullCheck(tmp);
                store              = gtNewOperNode(GT_COMMA, TYP_VOID, store, nullcheck);

                // COMMA(COMMA(tmp = "this", deref(tmp)), tmp)
                thisPtr = gtNewOperNode(GT_COMMA, vt, store, gtNewLclvNode(lclNum, vt));
            }
            else
            {
                // thisPtr = COMMA(deref("this"), "this")
                GenTree* nullcheck = gtNewNullCheck(thisPtr);
                thisPtr            = gtNewOperNode(GT_COMMA, vt, nullcheck, gtClone(objp, true));
            }

            call->gtFlags &= ~GTF_CALL_NULLCHECK;
        }
        else
        {
            thisPtr = objp;
        }

        // TODO-Cleanup: we leave it as a virtual stub call to
        // use logic in `LowerVirtualStubCall`, clear GTF_CALL_VIRT_KIND_MASK here
        // and change `LowerCall` to recognize it as a direct call.

        // During rationalization tmp="this" and null check will
        // materialize as embedded stmts in right execution order.
        assert(thisPtr != nullptr);
        call->gtArgs.PushFront(this, NewCallArg::Primitive(thisPtr, thisArg->GetSignatureType()));
        call->gtArgs.Remove(thisArg);
    }

    unsigned nOldStkArgsWords = lvaParameterStackSize / REGSIZE_BYTES;
    GenTree* arg3Node         = gtNewIconNode((ssize_t)nOldStkArgsWords, TYP_I_IMPL);
    CallArg* arg3 =
        call->gtArgs.PushBack(this, NewCallArg::Primitive(arg3Node).WellKnown(WellKnownArg::X86TailCallSpecialArg));
    // Inject a placeholder for the count of outgoing stack arguments that the Lowering phase will generate.
    // The constant will be replaced.
    GenTree* arg2Node = gtNewIconNode(9, TYP_I_IMPL);
    CallArg* arg2 =
        call->gtArgs.InsertAfter(this, arg3,
                                 NewCallArg::Primitive(arg2Node).WellKnown(WellKnownArg::X86TailCallSpecialArg));
    // Inject a placeholder for the flags.
    // The constant will be replaced.
    GenTree* arg1Node = gtNewIconNode(8, TYP_I_IMPL);
    CallArg* arg1 =
        call->gtArgs.InsertAfter(this, arg2,
                                 NewCallArg::Primitive(arg1Node).WellKnown(WellKnownArg::X86TailCallSpecialArg));
    // Inject a placeholder for the real call target that the Lowering phase will generate.
    // The constant will be replaced.
    GenTree* arg0Node = gtNewIconNode(7, TYP_I_IMPL);
    CallArg* arg0 =
        call->gtArgs.InsertAfter(this, arg1,
                                 NewCallArg::Primitive(arg0Node).WellKnown(WellKnownArg::X86TailCallSpecialArg));

    // It is now a varargs tail call.
    call->gtArgs.SetIsVarArgs();
    call->gtFlags &= ~GTF_CALL_POP_ARGS;

    // The function is responsible for doing explicit null check when it is necessary.
    assert(!call->NeedsNullCheck());

    JITDUMP("fgMorphTailCallViaJitHelper (after):\n");
    DISPTREE(call);
}

//------------------------------------------------------------------------
// fgGetStubAddrArg: Return the virtual stub address for the given call.
//
// Notes:
//    the JIT must place the address of the stub used to load the call target,
//    the "stub indirection cell", in special call argument with special register.
//
// Arguments:
//    call - a call that needs virtual stub dispatching.
//
// Return Value:
//    addr tree
//
GenTree* Compiler::fgGetStubAddrArg(GenTreeCall* call)
{
    assert(call->IsVirtualStub());
    GenTree* stubAddrArg;
    if (call->gtCallType == CT_INDIRECT)
    {
        stubAddrArg = gtClone(call->gtCallAddr, true);
    }
    else
    {
        assert(call->gtCallMoreFlags & GTF_CALL_M_VIRTSTUB_REL_INDIRECT);
        ssize_t addr = ssize_t(call->gtStubCallStubAddr);
        stubAddrArg  = gtNewIconHandleNode(addr, GTF_ICON_FTN_ADDR);
        INDEBUG(stubAddrArg->AsIntCon()->gtTargetHandle = (size_t)call->gtCallMethHnd);
    }
    assert(stubAddrArg != nullptr);
    return stubAddrArg;
}

//------------------------------------------------------------------------------
// fgGetArgTabEntryParameterLclNum : Get the lcl num for the parameter that
// corresponds to the argument to a recursive call.
//
// Notes:
//    Due to non-standard args this is not just the index of the argument in
//    the arg list. For example, in R2R compilations we will have added a
//    non-standard arg for the R2R indirection cell.
//
// Arguments:
//    arg  - the arg
//
unsigned Compiler::fgGetArgParameterLclNum(GenTreeCall* call, CallArg* arg)
{
    unsigned num = 0;

    for (CallArg& otherArg : call->gtArgs.Args())
    {
        if (&otherArg == arg)
        {
            break;
        }

        // Late added args add extra args that do not map to IL parameters and that we should not reassign.
        if (!otherArg.IsArgAddedLate())
        {
            num++;
        }
    }

    return num;
}

//------------------------------------------------------------------------------
// fgMorphRecursiveFastTailCallIntoLoop : Transform a recursive fast tail call into a loop.
//
//
// Arguments:
//    block  - basic block ending with a recursive fast tail call
//    recursiveTailCall - recursive tail call to transform
//
// Notes:
//    The legality of the transformation is ensured by the checks in endsWithTailCallConvertibleToLoop.

void Compiler::fgMorphRecursiveFastTailCallIntoLoop(BasicBlock* block, GenTreeCall* recursiveTailCall)
{
    assert(recursiveTailCall->IsTailCallConvertibleToLoop());
    Statement* lastStmt = block->lastStmt();
    assert(recursiveTailCall == lastStmt->GetRootNode());

    // Transform recursive tail call into a loop.

    Statement*       earlyArgInsertionPoint = lastStmt;
    const DebugInfo& callDI                 = lastStmt->GetDebugInfo();

    // All arguments whose trees may involve caller parameter local variables need to be assigned to temps first;
    // then the temps need to be assigned to the method parameters. This is done so that the caller
    // parameters are not re-assigned before call arguments depending on them  are evaluated.
    // tmpAssignmentInsertionPoint and paramAssignmentInsertionPoint keep track of
    // where the next temp or parameter assignment should be inserted.

    // In the example below the first call argument (arg1 - 1) needs to be assigned to a temp first
    // while the second call argument (const 1) doesn't.
    // Basic block before tail recursion elimination:
    //  ***** BB04, stmt 1 (top level)
    //  [000037] ------------             *  stmtExpr  void  (top level) (IL 0x00A...0x013)
    //  [000033] --C - G------ - \--*  call      void   RecursiveMethod
    //  [000030] ------------ | / --*  const     int - 1
    //  [000031] ------------arg0 in rcx + --*  +int
    //  [000029] ------------ | \--*  lclVar    int    V00 arg1
    //  [000032] ------------arg1 in rdx    \--*  const     int    1
    //
    //
    //  Basic block after tail recursion elimination :
    //  ***** BB04, stmt 1 (top level)
    //  [000051] ------------             *  stmtExpr  void  (top level) (IL 0x00A... ? ? ? )
    //  [000030] ------------ | / --*  const     int - 1
    //  [000031] ------------ | / --*  +int
    //  [000029] ------------ | | \--*  lclVar    int    V00 arg1
    //  [000050] - A----------             \--* = int
    //  [000049] D------N----                \--*  lclVar    int    V02 tmp0
    //
    //  ***** BB04, stmt 2 (top level)
    //  [000055] ------------             *  stmtExpr  void  (top level) (IL 0x00A... ? ? ? )
    //  [000052] ------------ | / --*  lclVar    int    V02 tmp0
    //  [000054] - A----------             \--* = int
    //  [000053] D------N----                \--*  lclVar    int    V00 arg0

    //  ***** BB04, stmt 3 (top level)
    //  [000058] ------------             *  stmtExpr  void  (top level) (IL 0x00A... ? ? ? )
    //  [000032] ------------ | / --*  const     int    1
    //  [000057] - A----------             \--* = int
    //  [000056] D------N----                \--*  lclVar    int    V01 arg1

    Statement* tmpAssignmentInsertionPoint   = lastStmt;
    Statement* paramAssignmentInsertionPoint = lastStmt;

    // Process early args. They may contain both setup statements for late args and actual args.
    for (CallArg& arg : recursiveTailCall->gtArgs.EarlyArgs())
    {
        GenTree* earlyArg = arg.GetEarlyNode();
        if (arg.GetLateNode() != nullptr)
        {
            // This is a setup node so we need to hoist it.
            Statement* earlyArgStmt = gtNewStmt(earlyArg, callDI);
            fgInsertStmtBefore(block, earlyArgInsertionPoint, earlyArgStmt);
        }
        else
        {
            // This is an actual argument that needs to be assigned to the corresponding caller parameter.
            // Late-added non-standard args are extra args that are not passed as locals, so skip those
            if (!arg.IsArgAddedLate())
            {
                Statement* paramAssignStmt =
                    fgAssignRecursiveCallArgToCallerParam(earlyArg, &arg,
                                                          fgGetArgParameterLclNum(recursiveTailCall, &arg), block,
                                                          callDI, tmpAssignmentInsertionPoint,
                                                          paramAssignmentInsertionPoint);
                if ((tmpAssignmentInsertionPoint == lastStmt) && (paramAssignStmt != nullptr))
                {
                    // All temp assignments will happen before the first param assignment.
                    tmpAssignmentInsertionPoint = paramAssignStmt;
                }
            }
        }
    }

    // Process late args.
    for (CallArg& arg : recursiveTailCall->gtArgs.LateArgs())
    {
        // A late argument is an actual argument that needs to be assigned to the corresponding caller's parameter.
        GenTree* lateArg = arg.GetLateNode();
        // Late-added non-standard args are extra args that are not passed as locals, so skip those
        if (!arg.IsArgAddedLate())
        {
            Statement* paramAssignStmt =
                fgAssignRecursiveCallArgToCallerParam(lateArg, &arg, fgGetArgParameterLclNum(recursiveTailCall, &arg),
                                                      block, callDI, tmpAssignmentInsertionPoint,
                                                      paramAssignmentInsertionPoint);

            if ((tmpAssignmentInsertionPoint == lastStmt) && (paramAssignStmt != nullptr))
            {
                // All temp assignments will happen before the first param assignment.
                tmpAssignmentInsertionPoint = paramAssignStmt;
            }
        }
    }

    // If the method has starg.s 0 or ldarga.s 0 a special local (lvaArg0Var) is created so that
    // compThisArg stays immutable. Normally it's assigned in fgFirstBBScratch block. Since that
    // block won't be in the loop (it's assumed to have no predecessors), we need to update the special local here.
    if (!info.compIsStatic && (lvaArg0Var != info.compThisArg))
    {
        GenTree* const thisArg = gtNewLclVarNode(info.compThisArg);
        thisArg->SetMorphed(this);
        GenTree* const arg0Store = gtNewStoreLclVarNode(lvaArg0Var, thisArg);
        arg0Store->SetMorphed(this);
        Statement* const arg0StoreStmt = gtNewStmt(arg0Store, callDI);
        fgInsertStmtBefore(block, paramAssignmentInsertionPoint, arg0StoreStmt);
    }

    // If compInitMem is set, we may need to zero-initialize some locals. Normally it's done in the prolog
    // but this loop can't include the prolog. Since we don't have liveness information, we insert zero-initialization
    // for all non-parameter IL locals as well as temp structs with GC fields.
    // Liveness phase will remove unnecessary initializations.
    if (info.compInitMem || compSuppressedZeroInit)
    {
        for (unsigned varNum = 0; varNum < lvaCount; varNum++)
        {
#if FEATURE_FIXED_OUT_ARGS
            if (varNum == lvaOutgoingArgSpaceVar)
            {
                continue;
            }
#endif // FEATURE_FIXED_OUT_ARGS

            LclVarDsc* varDsc = lvaGetDesc(varNum);

            if (varDsc->lvIsParam)
            {
                continue;
            }

#if FEATURE_IMPLICIT_BYREFS
            if (varDsc->lvPromoted)
            {
                LclVarDsc* firstField = lvaGetDesc(varDsc->lvFieldLclStart);
                if (firstField->lvParentLcl != varNum)
                {
                    // Local copy for implicit byref promotion that was undone. Do
                    // not introduce new references to it, all uses have been
                    // morphed to access the parameter.

#ifdef DEBUG
                    LclVarDsc* param = lvaGetDesc(firstField->lvParentLcl);
                    assert(param->lvIsImplicitByRef && !param->lvPromoted);
                    assert(param->lvFieldLclStart == varNum);
#endif
                    continue;
                }
            }
#endif

            var_types lclType            = varDsc->TypeGet();
            bool      isUserLocal        = (varNum < info.compLocalsCount);
            bool      structWithGCFields = ((lclType == TYP_STRUCT) && varDsc->GetLayout()->HasGCPtr());
            bool      hadSuppressedInit  = varDsc->lvSuppressedZeroInit;
            if ((info.compInitMem && (isUserLocal || structWithGCFields)) || hadSuppressedInit)
            {
                GenTree* zero = (lclType == TYP_STRUCT) ? gtNewIconNode(0) : gtNewZeroConNode(lclType);
                zero->SetMorphed(this);
                GenTree* init = gtNewStoreLclVarNode(varNum, zero);

                // No need for assertion prop here since the first block is now an (opaque) join
                // and has already been morphed.
                init->SetMorphed(this);
                init->gtType = lclType; // TODO-ASG: delete this zero-diff quirk.
                if (lclType == TYP_STRUCT)
                {
                    init = fgMorphInitBlock(init);
                }

                Statement* initStmt = gtNewStmt(init, callDI);
                fgInsertStmtBefore(block, lastStmt, initStmt);
            }
        }
    }

    // Remove the call
    fgRemoveStmt(block, lastStmt);

    assert(!opts.IsOSR());
    // Set the loop edge.
    // TODO-Cleanup: We should really be expanding tailcalls into loops much
    // earlier than this, at a place where we can just use the init BB here.
    BasicBlock* entryBB = fgGetFirstILBlock();
    assert(doesMethodHaveRecursiveTailcall());

    FlowEdge* const newEdge = fgAddRefPred(entryBB, block);
    block->SetKindAndTargetEdge(BBJ_ALWAYS, newEdge);

    // Update profile
    if (block->hasProfileWeight() && entryBB->hasProfileWeight())
    {
        entryBB->increaseBBProfileWeight(block->bbWeight);
        JITDUMP("Flow into entry BB " FMT_BB " increased. Data %s inconsistent.\n", entryBB->bbNum,
                fgPgoConsistent ? "is now" : "was already");
        fgPgoConsistent = false;
    }

    // Finish hooking things up.
    block->RemoveFlags(BBF_HAS_JMP);
}

//------------------------------------------------------------------------------
// fgAssignRecursiveCallArgToCallerParam : Assign argument to a recursive call to the corresponding caller parameter.
//
// Arguments:
//    arg                           - argument to assign
//    callArg                       - the corresponding call argument
//    lclParamNum                   - the lcl num of the parameter
//    block                         - basic block the call is in
//    callILOffset                  - IL offset of the call
//    tmpAssignmentInsertionPoint   - tree before which temp assignment should be inserted (if necessary)
//    paramAssignmentInsertionPoint - tree before which parameter assignment should be inserted
//
// Return Value:
//    parameter assignment statement if one was inserted; nullptr otherwise.
//
Statement* Compiler::fgAssignRecursiveCallArgToCallerParam(GenTree*         arg,
                                                           CallArg*         callArg,
                                                           unsigned         lclParamNum,
                                                           BasicBlock*      block,
                                                           const DebugInfo& callDI,
                                                           Statement*       tmpAssignmentInsertionPoint,
                                                           Statement*       paramAssignmentInsertionPoint)
{
    // Call arguments should be assigned to temps first and then the temps should be assigned to parameters because
    // some argument trees may reference parameters directly.

    GenTree* argInTemp             = nullptr;
    bool     needToAssignParameter = true;

    // TODO-CQ: enable calls with struct arguments passed in registers.
    noway_assert(!varTypeIsStruct(arg->TypeGet()));

    if (arg->IsCnsIntOrI() || arg->IsCnsFltOrDbl())
    {
        // The argument is already assigned to a temp or is a const.
        argInTemp = arg;
    }
    else if (arg->OperIs(GT_LCL_VAR))
    {
        unsigned   lclNum = arg->AsLclVar()->GetLclNum();
        LclVarDsc* varDsc = lvaGetDesc(lclNum);
        if (!varDsc->lvIsParam)
        {
            // The argument is a non-parameter local so it doesn't need to be assigned to a temp.
            argInTemp = arg;
        }
        else if (lclNum == lclParamNum)
        {
            // The argument is the same parameter local that we were about to assign so
            // we can skip the assignment.
            needToAssignParameter = false;
        }
    }

    // TODO: We don't need temp assignments if we can prove that the argument tree doesn't involve
    // any caller parameters. Some common cases are handled above but we may be able to eliminate
    // more temp assignments.

    Statement* paramAssignStmt = nullptr;
    if (needToAssignParameter)
    {
        if (argInTemp == nullptr)
        {
            // The argument is not assigned to a temp. We need to create a new temp and insert a store.
            unsigned tmpNum         = lvaGrabTemp(true DEBUGARG("arg temp"));
            lvaTable[tmpNum].lvType = arg->gtType;
            GenTree* tempSrc        = arg;
            GenTree* tmpStoreNode   = gtNewStoreLclVarNode(tmpNum, tempSrc);
            tmpStoreNode->SetMorphed(this);
            Statement* tmpStoreStmt = gtNewStmt(tmpStoreNode, callDI);
            fgInsertStmtBefore(block, tmpAssignmentInsertionPoint, tmpStoreStmt);
            argInTemp = gtNewLclvNode(tmpNum, tempSrc->gtType);

            // No need for assertion prop here since the first block is now an opqaque join
            // and has laready been morphed
            argInTemp->SetMorphed(this);
        }

        // Now assign the temp to the parameter.
        assert(lvaGetDesc(lclParamNum)->lvIsParam);
        GenTree* paramStoreNode = gtNewStoreLclVarNode(lclParamNum, argInTemp);

        // No need for assertion prop here since the first block is now an opqaque join
        // and has laready been morphed
        paramStoreNode->SetMorphed(this);

        paramAssignStmt = gtNewStmt(paramStoreNode, callDI);

        fgInsertStmtBefore(block, paramAssignmentInsertionPoint, paramAssignStmt);
    }
    return paramAssignStmt;
}

/*****************************************************************************
 *
 *  Transform the given GT_CALL tree for code generation.
 */

GenTree* Compiler::fgMorphCall(GenTreeCall* call)
{
    if (call->CanTailCall())
    {
        GenTree* newNode = fgMorphPotentialTailCall(call);
        if (newNode != nullptr)
        {
            return newNode;
        }

        assert(!call->CanTailCall());

#if FEATURE_MULTIREG_RET
        if (fgGlobalMorph && call->HasMultiRegRetVal() && varTypeIsStruct(call->TypeGet()))
        {
            // The tail call has been rejected so we must finish the work deferred
            // by impFixupCallStructReturn for multi-reg-returning calls and transform
            //     ret call
            // into
            //     temp = call
            //     ret temp

            // Force re-evaluating the argInfo as the return argument has changed.
            call->gtArgs.ResetFinalArgsAndABIInfo();

            // Create a new temp.
            unsigned tmpNum =
                lvaGrabTemp(false DEBUGARG("Return value temp for multi-reg return (rejected tail call)."));
            lvaTable[tmpNum].lvIsMultiRegRet = true;

            CORINFO_CLASS_HANDLE structHandle = call->gtRetClsHnd;
            assert(structHandle != NO_CLASS_HANDLE);
            const bool unsafeValueClsCheck = false;
            lvaSetStruct(tmpNum, structHandle, unsafeValueClsCheck);
            GenTree* store = gtNewStoreLclVarNode(tmpNum, call);
            store          = fgMorphTree(store);

            // Create the store statement and insert it before the current statement.
            Statement* storeStmt = gtNewStmt(store, compCurStmt->GetDebugInfo());
            fgInsertStmtBefore(compCurBB, compCurStmt, storeStmt);

            // Return the temp.
            GenTree* result = gtNewLclvNode(tmpNum, lvaTable[tmpNum].lvType);
            result->gtFlags |= GTF_DONT_CSE;

            compCurBB->SetFlags(BBF_HAS_CALL); // This block has a call

            JITDUMP("\nInserting store of a multi-reg call result to a temp:\n");
            DISPSTMT(storeStmt);
            result->SetMorphed(this);

            return result;
        }
#endif
    }

    if (call->IsSpecialIntrinsic())
    {
        if (lookupNamedIntrinsic(call->AsCall()->gtCallMethHnd) ==
            NI_System_Text_UTF8Encoding_UTF8EncodingSealed_ReadUtf8)
        {
            // Expanded in fgVNBasedIntrinsicExpansion
            setMethodHasSpecialIntrinsics();
        }
    }

    if (((call->gtCallMoreFlags & (GTF_CALL_M_SPECIAL_INTRINSIC | GTF_CALL_M_LDVIRTFTN_INTERFACE)) == 0) &&
        (call->gtCallMethHnd == eeFindHelper(CORINFO_HELP_VIRTUAL_FUNC_PTR)
#ifdef FEATURE_READYTORUN
         || call->gtCallMethHnd == eeFindHelper(CORINFO_HELP_READYTORUN_VIRTUAL_FUNC_PTR)
#endif
             ) &&
        (call == fgMorphStmt->GetRootNode()))
    {
        // This is call to CORINFO_HELP_VIRTUAL_FUNC_PTR with ignored result.
        // Transform it into a null check.

        assert(call->gtArgs.CountArgs() >= 1);
        GenTree* objPtr = call->gtArgs.GetArgByIndex(0)->GetNode();

        GenTree* nullCheck = gtNewNullCheck(objPtr);

        return fgMorphTree(nullCheck);
    }

    noway_assert(call->OperIs(GT_CALL));

    //
    // Only count calls once (only in the global morph phase)
    //
    if (fgGlobalMorph)
    {
        if (call->gtCallType == CT_INDIRECT)
        {
            optCallCount++;
            optIndirectCallCount++;
            if (call->IsFastTailCall())
            {
                optFastTailCallCount++;
                optIndirectFastTailCallCount++;
            }
        }
        else if (call->gtCallType == CT_USER_FUNC)
        {
            optCallCount++;
            if (call->IsVirtual())
            {
                optIndirectCallCount++;
            }
            if (call->IsFastTailCall())
            {
                optFastTailCallCount++;
                if (call->IsVirtual())
                {
                    optIndirectFastTailCallCount++;
                }
            }
        }
    }

    // Couldn't inline - remember that this BB contains method calls

    // Mark the block as a GC safe point for the call if possible.
    // In the event the call indicates the block isn't a GC safe point
    // and the call is unmanaged with a GC transition suppression request
    // then insert a GC poll.

    if (IsGcSafePoint(call))
    {
        compCurBB->SetFlags(BBF_GC_SAFE_POINT);
    }

    // Regardless of the state of the basic block with respect to GC safe point,
    // we will always insert a GC Poll for scenarios involving a suppressed GC
    // transition. Only mark the block for GC Poll insertion on the first morph.
    if (fgGlobalMorph && call->IsUnmanaged() && call->IsSuppressGCTransition())
    {
        compCurBB->SetFlags(BBF_HAS_SUPPRESSGC_CALL | BBF_GC_SAFE_POINT);
        optMethodFlags |= OMF_NEEDS_GCPOLLS;
    }

    if (fgGlobalMorph)
    {
        if (IsStaticHelperEligibleForExpansion(call))
        {
            // Current method has potential candidates for fgExpandStaticInit phase
            setMethodHasStaticInit();
        }
        else if ((call->gtCallMoreFlags & GTF_CALL_M_CAST_CAN_BE_EXPANDED) != 0)
        {
            // Current method has potential candidates for fgLateCastExpansion phase
            setMethodHasExpandableCasts();
        }
    }

    // Morph Type.op_Equality, Type.op_Inequality, and Enum.HasFlag
    //
    // We need to do these before the arguments are morphed
    if (!call->gtArgs.AreArgsComplete() && call->IsSpecialIntrinsic())
    {
        // See if this is foldable
        GenTree* optTree = gtFoldExprCall(call);

        // If we optimized, morph the result
        if (optTree != call)
        {
            return fgMorphTree(optTree);
        }
    }

    compCurBB->SetFlags(BBF_HAS_CALL); // This block has a call

    // From this point on disallow shared temps to be reused until we are done
    // processing the call.
    SharedTempsScope sharedTemps(this);

    // Process the "normal" argument list
    call = fgMorphArgs(call);
    noway_assert(call->OperIs(GT_CALL));

    // Try to replace CORINFO_HELP_TYPEHANDLE_TO_RUNTIMETYPE with a constant gc handle
    // pointing to a frozen segment
    if (gtIsTypeHandleToRuntimeTypeHelper(call))
    {
        GenTree*             argNode = call->AsCall()->gtArgs.GetArgByIndex(0)->GetNode();
        CORINFO_CLASS_HANDLE hClass  = gtGetHelperArgClassHandle(argNode);
        if (hClass != NO_CLASS_HANDLE)
        {
            CORINFO_OBJECT_HANDLE ptr = info.compCompHnd->getRuntimeTypePointer(hClass);
            if (ptr != NULL)
            {
                return fgMorphTree(gtNewIconEmbObjHndNode(ptr));
            }
        }
    }

    // Assign DEF flags if it produces a definition from "return buffer".
    fgAssignSetVarDef(call);
    if (call->OperRequiresAsgFlag())
    {
        call->gtFlags |= GTF_ASG;
    }

    // Should we expand this virtual method call target early here?
    //
    if (call->IsExpandedEarly() && call->IsVirtualVtable())
    {
        // We expand the Vtable Call target either in the global morph phase or
        // in guarded devirt if we need it for the guard.
        if (fgGlobalMorph && (call->gtControlExpr == nullptr))
        {
            call->gtControlExpr = fgExpandVirtualVtableCallTarget(call);
        }
        // We always have to morph or re-morph the control expr
        //
        call->gtControlExpr = fgMorphTree(call->gtControlExpr);

        // Propagate any side effect flags into the call
        call->gtFlags |= call->gtControlExpr->gtFlags & GTF_ALL_EFFECT;
    }

    // Morph stelem.ref helper call to store a null value, into a store into an array without the helper.
    // This needs to be done after the arguments are morphed to ensure constant propagation has already taken place.
    if (opts.OptimizationEnabled() && call->IsHelperCall(this, CORINFO_HELP_ARRADDR_ST))
    {
        assert(call->gtArgs.CountArgs() == 3);
        GenTree* arr   = call->gtArgs.GetArgByIndex(0)->GetNode();
        GenTree* index = call->gtArgs.GetArgByIndex(1)->GetNode();
        GenTree* value = call->gtArgs.GetArgByIndex(2)->GetNode();

        if (gtCanSkipCovariantStoreCheck(value, arr))
        {
            // Either or both of the array and index arguments may have been spilled to temps by `fgMorphArgs`. Copy
            // the spill trees as well if necessary.
            GenTree* argSetup = nullptr;
            for (CallArg& arg : call->gtArgs.EarlyArgs())
            {
                if (arg.GetLateNode() == nullptr)
                {
                    continue;
                }

                GenTree* const setupArgNode = arg.GetEarlyNode();
                assert((setupArgNode != arr) && (setupArgNode != index));

                if (argSetup == nullptr)
                {
                    argSetup = setupArgNode;
                }
                else
                {
                    argSetup = new (this, GT_COMMA) GenTreeOp(GT_COMMA, TYP_VOID, argSetup, setupArgNode);
                    argSetup->SetMorphed(this);
                }
            }

            GenTree* indexAddr = gtNewArrayIndexAddr(arr, index, TYP_REF, NO_CLASS_HANDLE);
            GenTree* store     = gtNewStoreIndNode(TYP_REF, indexAddr, value);
            GenTree* result    = fgMorphTree(store);

            if (argSetup != nullptr)
            {
                result = new (this, GT_COMMA) GenTreeOp(GT_COMMA, TYP_VOID, argSetup, result);
                result->SetMorphed(this);
            }

            return result;
        }
    }

    if (call->IsNoReturn())
    {
        //
        // If this call does not return then set fgHasNoReturnCall = true to
        // indicate to fgMorphStmts that we can remove trees/statements after
        // this call.
        //
        // This isn't need for tail calls as there shouldn't be any code after the call anyway.
        // Besides, the tail call code is part of the epilog and converting the block to
        // BBJ_THROW would result in the tail call being dropped as the epilog is generated
        // only for BBJ_RETURN blocks.
        //

        if (!call->IsTailCall())
        {
            fgHasNoReturnCall = true;
        }
    }

    return call;
}

/*****************************************************************************
 *
 *  Expand and return the call target address for a VirtualCall
 *  The code here should match that generated by LowerVirtualVtableCall
 */

GenTree* Compiler::fgExpandVirtualVtableCallTarget(GenTreeCall* call)
{
    GenTree* result;

    JITDUMP("Expanding virtual call target for %d.%s:\n", call->gtTreeID, GenTree::OpName(call->gtOper));

    noway_assert(call->gtCallType == CT_USER_FUNC);

    assert(call->gtArgs.HasThisPointer());
    // get a reference to the thisPtr being passed
    GenTree* thisPtr = call->gtArgs.GetThisArg()->GetNode();

    // fgMorphArgs must enforce this invariant by creating a temp
    //
    assert(thisPtr->OperIsLocal());

    // Make a copy of the thisPtr by cloning
    //
    thisPtr = gtClone(thisPtr, true);

    noway_assert(thisPtr != nullptr);

    // Get hold of the vtable offset
    unsigned vtabOffsOfIndirection;
    unsigned vtabOffsAfterIndirection;
    bool     isRelative;
    info.compCompHnd->getMethodVTableOffset(call->gtCallMethHnd, &vtabOffsOfIndirection, &vtabOffsAfterIndirection,
                                            &isRelative);

    // Dereference the this pointer to obtain the method table, it is called vtab below
    assert(VPTR_OFFS == 0); // We have to add this value to the thisPtr to get the methodTable
    GenTree* vtab = gtNewIndir(TYP_I_IMPL, thisPtr, GTF_IND_INVARIANT);

    if (fgGlobalMorph)
    {
        vtab->gtFlags &= ~GTF_EXCEPT; // TODO-Cleanup: delete this zero-diff quirk.
    }

    // Get the appropriate vtable chunk
    if (vtabOffsOfIndirection != CORINFO_VIRTUALCALL_NO_CHUNK)
    {
        // Note this isRelative code path is currently never executed
        // as the VM doesn't ever return:  isRelative == true
        //
        if (isRelative)
        {
            // MethodTable offset is a relative pointer.
            //
            // Additional temporary variable is used to store virtual table pointer.
            // Address of method is obtained by the next computations:
            //
            // Save relative offset to tmp (vtab is virtual table pointer, vtabOffsOfIndirection is offset of
            // vtable-1st-level-indirection):
            // tmp = vtab
            //
            // Save address of method to result (vtabOffsAfterIndirection is offset of vtable-2nd-level-indirection):
            // result = [tmp + vtabOffsOfIndirection + vtabOffsAfterIndirection + [tmp + vtabOffsOfIndirection]]
            //
            //
            // When isRelative is true we need to setup two temporary variables
            // var1 = vtab
            // var2 = var1 + vtabOffsOfIndirection + vtabOffsAfterIndirection + [var1 + vtabOffsOfIndirection]
            // result = [var2] + var2
            //
            unsigned varNum1   = lvaGrabTemp(true DEBUGARG("var1 - vtab"));
            unsigned varNum2   = lvaGrabTemp(true DEBUGARG("var2 - relative"));
            GenTree* storeVar1 = gtNewTempStore(varNum1, vtab); // var1 = vtab

            // [tmp + vtabOffsOfIndirection]
            GenTree* tmpTree1 = gtNewOperNode(GT_ADD, TYP_I_IMPL, gtNewLclvNode(varNum1, TYP_I_IMPL),
                                              gtNewIconNode(vtabOffsOfIndirection, TYP_I_IMPL));
            tmpTree1          = gtNewIndir(TYP_I_IMPL, tmpTree1, GTF_IND_NONFAULTING | GTF_IND_INVARIANT);

            // var1 + vtabOffsOfIndirection + vtabOffsAfterIndirection
            GenTree* tmpTree2 =
                gtNewOperNode(GT_ADD, TYP_I_IMPL, gtNewLclvNode(varNum1, TYP_I_IMPL),
                              gtNewIconNode(vtabOffsOfIndirection + vtabOffsAfterIndirection, TYP_I_IMPL));

            // var1 + vtabOffsOfIndirection + vtabOffsAfterIndirection + [var1 + vtabOffsOfIndirection]
            tmpTree2           = gtNewOperNode(GT_ADD, TYP_I_IMPL, tmpTree2, tmpTree1);
            GenTree* storeVar2 = gtNewTempStore(varNum2, tmpTree2); // var2 = <expression>

            // This last indirection is not invariant, but is non-faulting
            result = gtNewIndir(TYP_I_IMPL, gtNewLclvNode(varNum2, TYP_I_IMPL), GTF_IND_NONFAULTING); // [var2]

            result = gtNewOperNode(GT_ADD, TYP_I_IMPL, result, gtNewLclvNode(varNum2, TYP_I_IMPL)); // [var2] + var2

            // Now stitch together the two stores and the calculation of result into a single tree
            GenTree* commaTree = gtNewOperNode(GT_COMMA, TYP_I_IMPL, storeVar2, result);
            result             = gtNewOperNode(GT_COMMA, TYP_I_IMPL, storeVar1, commaTree);
        }
        else
        {
            // result = [vtab + vtabOffsOfIndirection]
            result = gtNewOperNode(GT_ADD, TYP_I_IMPL, vtab, gtNewIconNode(vtabOffsOfIndirection, TYP_I_IMPL));
            result = gtNewIndir(TYP_I_IMPL, result, GTF_IND_NONFAULTING | GTF_IND_INVARIANT);
        }
    }
    else
    {
        result = vtab;
        assert(!isRelative);
    }

    if (!isRelative)
    {
        // Load the function address
        // result = [result + vtabOffsAfterIndirection]
        result = gtNewOperNode(GT_ADD, TYP_I_IMPL, result, gtNewIconNode(vtabOffsAfterIndirection, TYP_I_IMPL));
        // This last indirection is not invariant, but is non-faulting
        result = gtNewIndir(TYP_I_IMPL, result, GTF_IND_NONFAULTING);
    }

    return result;
}

/*****************************************************************************
 *
 *  Transform the given constant tree for code generation.
 */

GenTree* Compiler::fgMorphConst(GenTree* tree)
{
    assert(tree->OperIsConst());

    /* Clear any exception flags or other unnecessary flags
     * that may have been set before folding this node to a constant */

    tree->gtFlags &= ~(GTF_ALL_EFFECT | GTF_REVERSE_OPS);

    if (!tree->OperIs(GT_CNS_STR))
    {
        return tree;
    }

    if (tree->AsStrCon()->IsStringEmptyField())
    {
        LPVOID         pValue;
        InfoAccessType iat = info.compCompHnd->emptyStringLiteral(&pValue);
        return fgMorphTree(gtNewStringLiteralNode(iat, pValue));
    }

    // TODO-CQ: Do this for compCurBB->isRunRarely(). Doing that currently will
    // guarantee slow performance for that block. Instead cache the return value
    // of CORINFO_HELP_STRCNS and go to cache first giving reasonable perf.

    bool useLazyStrCns = false;
    if (compCurBB->KindIs(BBJ_THROW))
    {
        useLazyStrCns = true;
    }
    else if (fgGlobalMorph && compCurStmt->GetRootNode()->IsCall())
    {
        // Quick check: if the root node of the current statement happens to be a noreturn call.
        GenTreeCall* call = compCurStmt->GetRootNode()->AsCall();
        useLazyStrCns     = call->IsNoReturn() || fgIsThrow(call);
    }

    if (useLazyStrCns)
    {
        CorInfoHelpFunc helper = info.compCompHnd->getLazyStringLiteralHelper(tree->AsStrCon()->gtScpHnd);
        if (helper != CORINFO_HELP_UNDEF)
        {
            // For un-important blocks, we want to construct the string lazily

            tree =
                gtNewHelperCallNode(helper, TYP_REF, gtNewIconNode(RidFromToken(tree->AsStrCon()->gtSconCPX), TYP_INT),
                                    gtNewIconEmbScpHndNode(tree->AsStrCon()->gtScpHnd));
            return fgMorphTree(tree);
        }
    }

    assert(tree->AsStrCon()->gtScpHnd == info.compScopeHnd || !IsUninitialized(tree->AsStrCon()->gtScpHnd));

    LPVOID         pValue;
    InfoAccessType iat =
        info.compCompHnd->constructStringLiteral(tree->AsStrCon()->gtScpHnd, tree->AsStrCon()->gtSconCPX, &pValue);

    tree = gtNewStringLiteralNode(iat, pValue);

    return fgMorphTree(tree);
}

//------------------------------------------------------------------------
// fgMorphLeaf: Fully morph a tree with no operands.
//
// Arguments:
//    tree - The tree to morph
//
// Return Value:
//    The fully morphed "tree".
//
GenTree* Compiler::fgMorphLeaf(GenTree* tree)
{
    assert(tree->OperIsLeaf());

    if (tree->OperIs(GT_LCL_VAR, GT_LCL_FLD, GT_LCL_ADDR))
    {
        tree = fgMorphLeafLocal(tree->AsLclVarCommon());
    }
    else if (tree->OperIs(GT_FTN_ADDR))
    {
        GenTreeFptrVal* fptrValTree = tree->AsFptrVal();

        // A function pointer address is being used. Let the VM know if this is the
        // target of a Delegate or a raw function pointer.
        bool isUnsafeFunctionPointer = !fptrValTree->gtFptrDelegateTarget;

        CORINFO_CONST_LOOKUP  addrInfo;
        CORINFO_METHOD_HANDLE funcHandle = fptrValTree->gtFptrMethod;

#ifdef FEATURE_READYTORUN
        if (fptrValTree->gtEntryPoint.addr != nullptr)
        {
            addrInfo = fptrValTree->gtEntryPoint;
        }
        else
#endif
        {
            info.compCompHnd->getFunctionFixedEntryPoint(funcHandle, isUnsafeFunctionPointer, &addrInfo);
        }

        GenTree* indNode = nullptr;
        switch (addrInfo.accessType)
        {
            case IAT_PPVALUE:
                indNode = gtNewIndOfIconHandleNode(TYP_I_IMPL, (size_t)addrInfo.handle, GTF_ICON_CONST_PTR);

                // Add the second indirection
                indNode = gtNewIndir(TYP_I_IMPL, indNode, GTF_IND_NONFAULTING | GTF_IND_INVARIANT);
                break;

            case IAT_PVALUE:
                indNode = gtNewIndOfIconHandleNode(TYP_I_IMPL, (size_t)addrInfo.handle, GTF_ICON_FTN_ADDR);
                INDEBUG(indNode->gtGetOp1()->AsIntCon()->gtTargetHandle = reinterpret_cast<size_t>(funcHandle));
                break;

            case IAT_VALUE:
                // Refer to gtNewIconHandleNode() as the template for constructing a constant handle
                //
                tree->SetOper(GT_CNS_INT);
                tree->AsIntConCommon()->SetIconValue(ssize_t(addrInfo.handle));
                tree->gtFlags |= GTF_ICON_FTN_ADDR;
                INDEBUG(tree->AsIntCon()->gtTargetHandle = reinterpret_cast<size_t>(funcHandle));
                break;

            default:
                noway_assert(!"Unknown addrInfo.accessType");
        }

        if (indNode != nullptr)
        {
            DEBUG_DESTROY_NODE(tree);
            tree = fgMorphTree(indNode);
        }
    }

    return tree;
}

void Compiler::fgAssignSetVarDef(GenTree* tree)
{
    auto visitDef = [=](const LocalDef& def) {
        if (def.IsEntire)
        {
            def.Def->gtFlags |= GTF_VAR_DEF;
        }
        else
        {
            // We consider partial definitions to be modeled as uses followed by definitions.
            // This captures the idea that precedings defs are not necessarily made redundant
            // by this definition.
            def.Def->gtFlags |= (GTF_VAR_DEF | GTF_VAR_USEASG);
        }
        return GenTree::VisitResult::Continue;
    };

    tree->VisitLocalDefs(this, visitDef);
}

#ifdef FEATURE_SIMD

//--------------------------------------------------------------------------------------------------------------
// getSIMDStructFromField:
//   Checking whether the field belongs to a simd struct or not. If it is, return the GenTree* for
//   the struct node, also base type, field index and simd size. If it is not, just return  nullptr.
//   Usually if the tree node is from a simd lclvar which is not used in any SIMD intrinsic, then we
//   should return nullptr, since in this case we should treat SIMD struct as a regular struct.
//   However if no matter what, you just want get simd struct node, you can set the ignoreUsedInSIMDIntrinsic
//   as true. Then there will be no IsUsedInSIMDIntrinsic checking, and it will return SIMD struct node
//   if the struct is a SIMD struct.
//
// Arguments:
//       tree - GentreePtr. This node will be checked to see this is a field which belongs to a simd
//               struct used for simd intrinsic or not.
//       indexOut - unsigned pointer, if the tree is used for simd intrinsic, we will set *indexOut
//                  equals to the index number of this field.
//       simdSizeOut - unsigned pointer, if the tree is used for simd intrinsic, set the *simdSizeOut
//                     equals to the simd struct size which this tree belongs to.
//      ignoreUsedInSIMDIntrinsic - bool. If this is set to true, then this function will ignore
//                                  the UsedInSIMDIntrinsic check.
//
// Return Value:
//       A GenTree* which points the simd lclvar tree belongs to. If the tree is not the simd
//       instrinic related field, return nullptr.
//
GenTree* Compiler::getSIMDStructFromField(GenTree*  tree,
                                          unsigned* indexOut,
                                          unsigned* simdSizeOut,
                                          bool      ignoreUsedInSIMDIntrinsic /*false*/)
{
    if (tree->isIndir())
    {
        GenTree* addr = tree->AsIndir()->Addr();
        if (!addr->OperIs(GT_FIELD_ADDR) || !addr->AsFieldAddr()->IsInstance())
        {
            return nullptr;
        }

        GenTree* objRef = addr->AsFieldAddr()->GetFldObj();
        if (objRef->IsLclVarAddr())
        {
            LclVarDsc* varDsc = lvaGetDesc(objRef->AsLclVarCommon());
            if (varTypeIsSIMD(varDsc) && (varDsc->lvIsUsedInSIMDIntrinsic() || ignoreUsedInSIMDIntrinsic))
            {
                var_types elementType = tree->TypeGet();
                unsigned  fieldOffset = addr->AsFieldAddr()->gtFldOffset;
                unsigned  elementSize = genTypeSize(elementType);

                if (varTypeIsArithmetic(elementType) && ((fieldOffset % elementSize) == 0))
                {
                    *simdSizeOut = varDsc->lvExactSize();
                    *indexOut    = fieldOffset / elementSize;

                    return objRef;
                }
            }
        }
    }

    return nullptr;
}
#endif // FEATURE_SIMD

//------------------------------------------------------------------------------
// fgMorphCommutative : Try to simplify "(X op C1) op C2" to "X op C3"
//                      for commutative operators.
//
// Arguments:
//       tree - node to fold
//
// return value:
//       A folded GenTree* instance or nullptr if something prevents folding.
//

GenTreeOp* Compiler::fgMorphCommutative(GenTreeOp* tree)
{
    assert(varTypeIsIntegralOrI(tree->TypeGet()));
    assert(tree->OperIs(GT_ADD, GT_MUL, GT_OR, GT_AND, GT_XOR));

    if (opts.OptimizationDisabled())
    {
        return nullptr;
    }

    // op1 can be GT_COMMA, in this case we're going to fold
    // "(op (COMMA(... (op X C1))) C2)" to "(COMMA(... (op X C3)))"
    GenTree*   op1  = tree->gtGetOp1()->gtEffectiveVal();
    genTreeOps oper = tree->OperGet();

    if (!op1->OperIs(oper) || !tree->gtGetOp2()->IsCnsIntOrI() || !op1->gtGetOp2()->IsCnsIntOrI() ||
        op1->gtGetOp1()->IsCnsIntOrI())
    {
        return nullptr;
    }

    if (!fgGlobalMorph && (op1 != tree->gtGetOp1()))
    {
        // Since 'tree->gtGetOp1()' can have complex structure (e.g. COMMA(..(COMMA(..,op1)))
        // don't run the optimization for such trees outside of global morph.
        // Otherwise, there is a chance of violating VNs invariants.
        return nullptr;
    }

    if (tree->OperMayOverflow() && (tree->gtOverflow() || op1->gtOverflow()))
    {
        return nullptr;
    }

    GenTreeIntCon* cns1 = op1->gtGetOp2()->AsIntCon();
    GenTreeIntCon* cns2 = tree->gtGetOp2()->AsIntCon();

    if (!varTypeIsIntegralOrI(tree->TypeGet()) || cns1->TypeIs(TYP_REF) || !cns1->TypeIs(cns2->TypeGet()))
    {
        return nullptr;
    }

    GenTree* folded = gtFoldExprConst(gtNewOperNode(oper, cns1->TypeGet(), cns1, cns2));

    if (!folded->IsCnsIntOrI())
    {
        // Give up if we can't fold "C1 op C2"
        return nullptr;
    }

    auto foldedCns = folded->AsIntCon();

    cns1->SetIconValue(foldedCns->IconValue());
    cns1->SetVNsFromNode(foldedCns);
    cns1->gtFieldSeq = foldedCns->gtFieldSeq;

    op1 = tree->gtGetOp1();
    op1->SetVNsFromNode(tree);

    DEBUG_DESTROY_NODE(tree);
    DEBUG_DESTROY_NODE(cns2);
    DEBUG_DESTROY_NODE(foldedCns);
    cns1->SetMorphed(this);

    return op1->AsOp();
}

//------------------------------------------------------------------------
// fgMorphSmpOp: morph a GTK_SMPOP tree
//
// Arguments:
//    tree - tree to morph
//    mac  - address context for morphing
//    optAssertionPropDone - [out, optional] set true if local assertions
//       were killed/genned while morphing this tree
//
// Returns:
//    Tree, possibly updated
//
GenTree* Compiler::fgMorphSmpOp(GenTree* tree, MorphAddrContext* mac, bool* optAssertionPropDone)
{
    assert(tree->OperKind() & GTK_SMPOP);

    /* The steps in this function are :
       o Perform required preorder processing
       o Process the first, then second operand, if any
       o Perform required postorder morphing
       o Perform optional postorder morphing if optimizing
     */

    bool isQmarkColon = false;

    ASSERT_TP origAssertions = BitVecOps::UninitVal();
    ASSERT_TP thenAssertions = BitVecOps::UninitVal();

    genTreeOps oper = tree->OperGet();
    var_types  typ  = tree->TypeGet();
    GenTree*   op1  = tree->AsOp()->gtOp1;
    GenTree*   op2  = tree->gtGetOp2IfPresent();

    /*-------------------------------------------------------------------------
     * First do any PRE-ORDER processing
     */

    switch (oper)
    {
        // Some arithmetic operators need to use a helper call to the EE
        int helper;

        case GT_STORE_LCL_VAR:
        case GT_STORE_LCL_FLD:
        {
            LclVarDsc* lclDsc = lvaGetDesc(tree->AsLclVarCommon());
            if (lclDsc->IsAddressExposed()
#if FEATURE_IMPLICIT_BYREFS
                || lclDsc->lvIsLastUseCopyOmissionCandidate
#endif
            )
            {
                tree->AddAllEffectsFlags(GTF_GLOB_REF);
            }

            GenTree* expandedTree = fgMorphExpandLocal(tree->AsLclVarCommon());
            if (expandedTree != nullptr)
            {
                expandedTree->SetMorphed(this);
                tree = expandedTree;
                oper = tree->OperGet();
                op1  = tree->gtGetOp1();
                op2  = tree->gtGetOp2IfPresent();
            }
        }
        break;

        case GT_QMARK:
        case GT_JTRUE:

            noway_assert(op1);

            if (op1->OperIsCompare())
            {
                /* Mark the comparison node with GTF_RELOP_JMP_USED so it knows that it does
                   not need to materialize the result as a 0 or 1. */

                /* We also mark it as DONT_CSE, as we don't handle QMARKs with nonRELOP op1s */
                op1->gtFlags |= (GTF_RELOP_JMP_USED | GTF_DONT_CSE);
            }
            else
            {
                GenTree* effOp1 = op1->gtEffectiveVal();
                noway_assert(effOp1->OperIs(GT_CNS_INT) && (effOp1->IsIntegralConst(0) || effOp1->IsIntegralConst(1)));
            }
            break;

        case GT_COLON:
            if (optLocalAssertionProp)
            {
                isQmarkColon = true;
                BitVecOps::ClearD(apTraits, apLocalPostorder);
            }
            break;

        case GT_FIELD_ADDR:
            return fgMorphFieldAddr(tree, mac);

        case GT_INDEX_ADDR:
            return fgMorphIndexAddr(tree->AsIndexAddr());

        case GT_CAST:
        {
            GenTree* morphedCast = fgMorphExpandCast(tree->AsCast());
            if (morphedCast != nullptr)
            {
                return morphedCast;
            }

            op1 = tree->AsCast()->CastOp();
        }
        break;

        case GT_MUL:
            noway_assert(op2 != nullptr);

#ifndef TARGET_64BIT
            if (typ == TYP_LONG)
            {
                // For (long)int1 * (long)int2, we dont actually do the
                // casts, and just multiply the 32 bit values, which will
                // give us the 64 bit result in edx:eax.

                if (tree->Is64RsltMul())
                {
                    // We are seeing this node again.
                    // Morph only the children of casts,
                    // so as to avoid losing them.
                    tree = fgMorphLongMul(tree->AsOp());

                    goto DONE_MORPHING_CHILDREN;
                }

                tree = fgRecognizeAndMorphLongMul(tree->AsOp());
                op1  = tree->AsOp()->gtGetOp1();
                op2  = tree->AsOp()->gtGetOp2();

                if (tree->Is64RsltMul())
                {
                    goto DONE_MORPHING_CHILDREN;
                }
                else
                {
                    if (tree->gtOverflow())
                        helper = tree->IsUnsigned() ? CORINFO_HELP_ULMUL_OVF : CORINFO_HELP_LMUL_OVF;
                    else
                        helper = CORINFO_HELP_LMUL;

                    goto USE_HELPER_FOR_ARITH;
                }
            }
#endif // !TARGET_64BIT
            break;

        case GT_ARR_LENGTH:
            if (op1->OperIs(GT_CNS_STR))
            {
                // Optimize `ldstr + String::get_Length()` to CNS_INT
                // e.g. "Hello".Length => 5
                GenTreeIntCon* iconNode = gtNewStringLiteralLength(op1->AsStrCon());
                if (iconNode != nullptr)
                {
                    iconNode->SetMorphed(this);
                    return iconNode;
                }
            }
            break;

        case GT_IND:
            if (opts.OptimizationEnabled())
            {
                GenTree* constNode = gtFoldIndirConst(tree->AsIndir());
                if (constNode != nullptr)
                {
                    assert(constNode->OperIsConst()); // No further morphing required.
                    constNode->SetMorphed(this);
                    return constNode;
                }
            }
            break;

        case GT_STOREIND:
            if (varTypeIsGC(tree))
            {
                GenTree* addr = op1;
                // If we're storing a reference to a field (GT_FIELD_ADDR), let's check if the field's owner is a
                // byref-like struct.
                while ((addr != nullptr) && addr->OperIs(GT_FIELD_ADDR))
                {
                    CORINFO_FIELD_HANDLE fieldHandle = addr->AsFieldAddr()->gtFldHnd;
                    if (eeIsByrefLike(info.compCompHnd->getFieldClass(fieldHandle)))
                    {
                        JITDUMP(
                            "Marking [%06u] STOREIND as GTF_IND_TGT_NOT_HEAP: field's owner is a byref-like struct\n",
                            dspTreeID(tree));
                        tree->gtFlags |= GTF_IND_TGT_NOT_HEAP;
                        break;
                    }
                    addr = addr->AsFieldAddr()->GetFldObj();
                }
            }
            break;

        case GT_DIV:
            // Replace "val / dcon" with "val * (1.0 / dcon)" if dcon is a power of two.
            // Powers of two within range are always exactly represented,
            // so multiplication by the reciprocal is safe in this scenario
            if (fgGlobalMorph && op2->IsCnsFltOrDbl())
            {
                double divisor = op2->AsDblCon()->DconValue();
                if (((typ == TYP_DOUBLE) && FloatingPointUtils::hasPreciseReciprocal(divisor)) ||
                    ((typ == TYP_FLOAT) && FloatingPointUtils::hasPreciseReciprocal(forceCastToFloat(divisor))))
                {
                    oper = GT_MUL;
                    tree->ChangeOper(oper);
                    op2->AsDblCon()->SetDconValue(1.0 / divisor);
                }
            }

            // Convert DIV to UDIV if both op1 and op2 are known to be never negative
            if (varTypeIsIntegral(tree) && op1->IsNeverNegative(this) && op2->IsNeverNegative(this))
            {
                assert(tree->OperIs(GT_DIV));
                tree->ChangeOper(GT_UDIV, GenTree::PRESERVE_VN);
                return fgMorphSmpOp(tree, mac);
            }

#ifndef TARGET_64BIT
            if (typ == TYP_LONG)
            {
                helper = CORINFO_HELP_LDIV;
                goto USE_HELPER_FOR_ARITH;
            }

#if USE_HELPERS_FOR_INT_DIV
            if (typ == TYP_INT)
            {
                helper = CORINFO_HELP_DIV;
                goto USE_HELPER_FOR_ARITH;
            }
#endif
#endif // !TARGET_64BIT
            break;

        case GT_UDIV:

#ifndef TARGET_64BIT
            if (typ == TYP_LONG)
            {
                helper = CORINFO_HELP_ULDIV;
                goto USE_HELPER_FOR_ARITH;
            }
#if USE_HELPERS_FOR_INT_DIV
            if (typ == TYP_INT)
            {
                helper = CORINFO_HELP_UDIV;
                goto USE_HELPER_FOR_ARITH;
            }
#endif
#endif // TARGET_64BIT
            break;

        case GT_MOD:

            if (varTypeIsFloating(typ))
            {
                helper = CORINFO_HELP_DBLREM;
                noway_assert(op2);
                if (op1->TypeIs(TYP_FLOAT))
                {
                    if (op2->TypeIs(TYP_FLOAT))
                    {
                        helper = CORINFO_HELP_FLTREM;
                    }
                    else
                    {
                        tree->AsOp()->gtOp1 = op1 = gtNewCastNode(TYP_DOUBLE, op1, false, TYP_DOUBLE);
                    }
                }
                else if (op2->TypeIs(TYP_FLOAT))
                {
                    tree->AsOp()->gtOp2 = op2 = gtNewCastNode(TYP_DOUBLE, op2, false, TYP_DOUBLE);
                }
                goto USE_HELPER_FOR_ARITH;
            }

            // Convert MOD to UMOD if both op1 and op2 are known to be never negative
            if (varTypeIsIntegral(tree) && op1->IsNeverNegative(this) && op2->IsNeverNegative(this))
            {
                assert(tree->OperIs(GT_MOD));
                tree->ChangeOper(GT_UMOD, GenTree::PRESERVE_VN);
                return fgMorphSmpOp(tree, mac);
            }

            // Do not use optimizations (unlike UMOD's idiv optimizing during codegen) for signed mod.
            // A similar optimization for signed mod will not work for a negative perfectly divisible
            // HI-word. To make it correct, we would need to divide without the sign and then flip the
            // result sign after mod. This requires 18 opcodes + flow making it not worthy to inline.
            goto ASSIGN_HELPER_FOR_MOD;

        case GT_UMOD:

#ifdef TARGET_ARMARCH
//
// Note for TARGET_ARMARCH we don't have  a remainder instruction, so we don't do this optimization
//
#else  // TARGET_XARCH
       // If this is an unsigned long mod with a constant divisor,
       // then don't morph to a helper call - it can be done faster inline using idiv.

            noway_assert(op2);
            if ((typ == TYP_LONG) && opts.OptimizationEnabled())
            {
                if (op2->OperIs(GT_CNS_NATIVELONG) && op2->AsIntConCommon()->LngValue() >= 2 &&
                    op2->AsIntConCommon()->LngValue() <= 0x3fffffff)
                {
                    op2->SetMorphed(this);
                    tree->AsOp()->gtOp1 = op1 = fgMorphTree(op1);
                    noway_assert(op1->TypeIs(TYP_LONG));

                    // Update flags for op1 morph.
                    tree->gtFlags &= ~GTF_ALL_EFFECT;

                    // Only update with op1 as op2 is a constant.
                    tree->gtFlags |= (op1->gtFlags & GTF_ALL_EFFECT);

                    // If op1 is a constant, then do constant folding of the division operator.
                    if (op1->OperIs(GT_CNS_NATIVELONG))
                    {
                        tree = gtFoldExpr(tree);
                    }

                    if (!tree->OperIsConst())
                    {
                        tree->AsOp()->CheckDivideByConstOptimized(this);
                    }

                    return tree;
                }
            }
#endif // TARGET_XARCH

        ASSIGN_HELPER_FOR_MOD:

            if (tree->OperIs(GT_MOD, GT_UMOD) && (op2->IsIntegralConst(1)))
            {
                // Transformation: a % 1 = 0
                GenTree* optimizedTree = fgMorphModToZero(tree->AsOp());
                if (optimizedTree != nullptr)
                {
                    tree = optimizedTree;

                    if (tree->OperIs(GT_COMMA))
                    {
                        op1 = tree->gtGetOp1();
                        op2 = tree->gtGetOp2();
                    }
                    else
                    {
                        assert(tree->IsIntegralConst());
                        op1 = nullptr;
                        op2 = nullptr;
                    }
                    break;
                }
            }

#ifndef TARGET_64BIT
            if (typ == TYP_LONG)
            {
                helper = (oper == GT_UMOD) ? CORINFO_HELP_ULMOD : CORINFO_HELP_LMOD;
                goto USE_HELPER_FOR_ARITH;
            }

#if USE_HELPERS_FOR_INT_DIV
            if (typ == TYP_INT)
            {
                if (oper == GT_UMOD)
                {
                    helper = CORINFO_HELP_UMOD;
                    goto USE_HELPER_FOR_ARITH;
                }
                else if (oper == GT_MOD)
                {
                    helper = CORINFO_HELP_MOD;
                    goto USE_HELPER_FOR_ARITH;
                }
            }
#endif
#endif // !TARGET_64BIT

            if (tree->OperIs(GT_UMOD) && op2->IsIntegralConstUnsignedPow2())
            {
                // Transformation: a % b = a & (b - 1);
                tree = fgMorphUModToAndSub(tree->AsOp());
                op1  = tree->AsOp()->gtOp1;
                op2  = tree->AsOp()->gtOp2;
            }
#ifdef TARGET_ARM64
            // ARM64 architecture manual suggests this transformation
            // for the mod operator.
            else
#else
            // XARCH only applies this transformation if we know
            // that magic division will be used - which is determined
            // when 'b' is not a power of 2 constant and mod operator is signed.
            // Lowering for XARCH does this optimization already,
            // but is also done here to take advantage of CSE.
            else if (tree->OperIs(GT_MOD) && op2->IsIntegralConst() && !op2->IsIntegralConstAbsPow2())
#endif
            {
                // Transformation: a % b = a - (a / b) * b;
                tree = fgMorphModToSubMulDiv(tree->AsOp());
                op1  = tree->AsOp()->gtOp1;
                op2  = tree->AsOp()->gtOp2;
            }
            break;

        USE_HELPER_FOR_ARITH:
        {
            // TODO: this comment is wrong now, do an appropriate fix.
            /* We have to morph these arithmetic operations into helper calls
               before morphing the arguments (preorder), else the arguments
               won't get correct values of fgPtrArgCntCur.
               However, try to fold the tree first in case we end up with a
               simple node which won't need a helper call at all */

            noway_assert(tree->OperIsBinary());

            GenTree* oldTree = tree;

            tree = gtFoldExpr(tree);

            // Were we able to fold it ?
            // Note that gtFoldExpr may return a non-leaf even if successful
            // e.g. for something like "expr / 1" - see also bug #290853
            if (tree->OperIsLeaf() || (oldTree != tree))
            {
                return (oldTree != tree) ? fgMorphTree(tree) : fgMorphLeaf(tree);
            }

            // Did we fold it into a comma node with throw?
            if (tree->OperIs(GT_COMMA))
            {
                noway_assert(fgIsCommaThrow(tree));
                return fgMorphTree(tree);
            }
        }

            return fgMorphIntoHelperCall(tree, helper, true /* morphArgs */, op1, op2);

        case GT_RETURN:
        case GT_SWIFT_ERROR_RET:
        {
            GenTree*& retVal = tree->AsOp()->ReturnValueRef();

            // Apply some optimizations that change the type of the return.
            // These are not applicable when this is a merged return that will
            // be changed into a store and jump to the return BB.
            if (!tree->TypeIs(TYP_VOID) && ((genReturnBB == nullptr) || (compCurBB == genReturnBB)))
            {
                if (retVal->OperIs(GT_LCL_FLD))
                {
                    retVal = fgMorphRetInd(tree->AsOp());
                }

                fgTryReplaceStructLocalWithFields(&retVal);
            }

            // normalize small integer return values
            if (fgGlobalMorph && varTypeIsSmall(info.compRetType) && (retVal != nullptr) && !retVal->TypeIs(TYP_VOID) &&
                fgCastNeeded(retVal, info.compRetType))
            {
#ifdef SWIFT_SUPPORT
                // Morph error operand if tree is a GT_SWIFT_ERROR_RET node
                if (tree->OperIs(GT_SWIFT_ERROR_RET))
                {
                    GenTree* const errorVal = fgMorphTree(tree->gtGetOp1());
                    tree->AsOp()->gtOp1     = errorVal;

                    // Propagate side effect flags
                    tree->SetAllEffectsFlags(errorVal);
                }
#endif // SWIFT_SUPPORT

                // Small-typed return values are normalized by the callee
                retVal = gtNewCastNode(TYP_INT, retVal, false, info.compRetType);

                // Propagate GTF_COLON_COND
                retVal->gtFlags |= (tree->gtFlags & GTF_COLON_COND);

                retVal = fgMorphTree(retVal);

                // Propagate side effect flags
                tree->SetAllEffectsFlags(retVal);

                return tree;
            }

            if (tree->OperIs(GT_RETURN))
            {
                op1 = retVal;
            }
            else
            {
                op2 = retVal;
            }
            break;
        }

        case GT_EQ:
        case GT_NE:
        {
            if (opts.OptimizationEnabled())
            {
                GenTree* optimizedTree = gtFoldTypeCompare(tree);
                if (optimizedTree != tree)
                {
                    return fgMorphTree(optimizedTree);
                }
            }

            // Pattern-matching optimization:
            //    (a % c) ==/!= 0
            // for power-of-2 constant `c`
            // =>
            //    a & (c - 1) ==/!= 0
            // For integer `a`, even if negative.
            if (opts.OptimizationEnabled())
            {
                assert(tree->OperIs(GT_EQ, GT_NE));
                if (op1->OperIs(GT_MOD) && varTypeIsIntegral(op1) && op2->IsIntegralConst(0))
                {
                    GenTree* op1op2 = op1->AsOp()->gtOp2;
                    if (op1op2->IsCnsIntOrI())
                    {
                        const ssize_t modValue = op1op2->AsIntCon()->IconValue();
                        if (isPow2(modValue))
                        {
                            JITDUMP("\nTransforming:\n");
                            DISPTREE(tree);

                            op1->SetOper(GT_AND);                                 // Change % => &
                            op1op2->AsIntConCommon()->SetIconValue(modValue - 1); // Change c => c - 1
                            fgUpdateConstTreeValueNumber(op1op2);

                            JITDUMP("\ninto:\n");
                            DISPTREE(tree);
                        }
                    }
                }
            }
        }

            FALLTHROUGH;

        case GT_GT:
        {
            // Try and optimize nullable boxes feeding compares
            GenTree* optimizedTree = gtFoldBoxNullable(tree);

            if (optimizedTree->OperGet() != tree->OperGet())
            {
                return optimizedTree;
            }
            else
            {
                tree = optimizedTree;
            }

            op1 = tree->AsOp()->gtOp1;
            op2 = tree->gtGetOp2IfPresent();

            break;
        }

        case GT_RUNTIMELOOKUP:
            return fgMorphTree(op1);

        case GT_COMMA:
            if (op2->OperIsStore() || (op2->OperIs(GT_COMMA) && op2->TypeIs(TYP_VOID)) || fgIsThrow(op2))
            {
                typ = tree->gtType = TYP_VOID;
            }

            break;

        default:
            break;
    }

    if (opts.OptimizationEnabled() && fgGlobalMorph)
    {
        GenTree* morphed = fgMorphReduceAddOps(tree);
        if (morphed != tree)
            return fgMorphTree(morphed);
    }

    /*-------------------------------------------------------------------------
     * Process the first operand, if any
     */

    if (op1 != nullptr)
    {
        // If we are entering the "then" part of a Qmark-Colon we must
        // save the state of the current assertions table so that we can
        // restore this state when entering the "else" part
        if (isQmarkColon)
        {
            noway_assert(optLocalAssertionProp);
            BitVecOps::Assign(apTraits, origAssertions, apLocal);
        }

        // TODO-Bug: Moving the null check to this indirection should nominally check for interference with
        // the other operands in case this is a store. However, doing so unconditionally preserves previous
        // behavior and "fixes up" field store importation that places the null check in the wrong location
        // (before the 'value' operand is evaluated).
        MorphAddrContext indMac;
        if (tree->OperIsIndir() && !tree->OperIsAtomicOp())
        {
            // Communicate to FIELD_ADDR morphing that the parent is an indirection.
            indMac.m_user = tree->AsIndir();
            mac           = &indMac;
        }
        // For additions, if we already have a context, keep track of whether all offsets added
        // to the address are constant, and their sum does not overflow.
        else if ((mac != nullptr) && tree->OperIs(GT_ADD) && op2->IsCnsIntOrI())
        {
            ClrSafeInt<size_t> offset(mac->m_totalOffset);
            offset += op2->AsIntCon()->IconValue();
            if (!offset.IsOverflow())
            {
                mac->m_totalOffset = offset.Value();
            }
            else
            {
                mac = nullptr;
            }
        }
        else // Reset the context.
        {
            mac = nullptr;
        }

        tree->AsOp()->gtOp1 = op1 = fgMorphTree(op1, mac);

        // If we are exiting the "then" part of a Qmark-Colon we must
        // save the state of the current assertions table so that we
        // can merge this state with the "else" part exit
        if (isQmarkColon)
        {
            noway_assert(optLocalAssertionProp);
            BitVecOps::Assign(apTraits, thenAssertions, apLocal);
        }
    }

    /*-------------------------------------------------------------------------
     * Process the second operand, if any
     */

    if (op2 != nullptr)
    {
        // If we are entering the "else" part of a Qmark-Colon we must
        // reset the state of the current assertions table
        if (isQmarkColon)
        {
            noway_assert(optLocalAssertionProp);
            BitVecOps::Assign(apTraits, apLocal, origAssertions);
        }

        tree->AsOp()->gtOp2 = op2 = fgMorphTree(op2);

        // If we are exiting the "else" part of a Qmark-Colon we must
        // merge the state of the current assertions table with that
        // of the exit of the "then" part.
        //
        if (isQmarkColon)
        {
            noway_assert(optLocalAssertionProp);

            // Merge then and else (current) assertion sets.
            //
            BitVecOps::IntersectionD(apTraits, apLocal, thenAssertions);
        }
    }

#ifndef TARGET_64BIT
DONE_MORPHING_CHILDREN:
#endif // !TARGET_64BIT

    gtUpdateNodeOperSideEffects(tree);

    if (op1 != nullptr)
    {
        tree->AddAllEffectsFlags(op1);
    }
    if (op2 != nullptr)
    {
        tree->AddAllEffectsFlags(op2);
    }

    /*-------------------------------------------------------------------------
     * Now do POST-ORDER processing
     */

    if (varTypeIsGC(tree->TypeGet()) && (op1 && !varTypeIsGC(op1->TypeGet())) && (op2 && !varTypeIsGC(op2->TypeGet())))
    {
        // The tree is really not GC but was marked as such. Now that the
        // children have been unmarked, unmark the tree too.

        // Remember that GT_COMMA inherits it's type only from op2
        if (tree->OperIs(GT_COMMA))
        {
            tree->gtType = genActualType(op2->TypeGet());
        }
        else
        {
            tree->gtType = genActualType(op1->TypeGet());
        }
    }

    GenTree* oldTree = tree;

    GenTree* qmarkOp1 = nullptr;
    GenTree* qmarkOp2 = nullptr;

    if (tree->OperIs(GT_QMARK) && (tree->AsOp()->gtOp2->OperIs(GT_COLON)))
    {
        qmarkOp1 = oldTree->AsOp()->gtOp2->AsOp()->gtOp1;
        qmarkOp2 = oldTree->AsOp()->gtOp2->AsOp()->gtOp2;
    }

    // During global morph, give assertion prop another shot at this tree.
    //
    // We need to use the "postorder" assertion set here, because apLocal
    // may reflect results from subtrees that have since been reordered.
    //
    // apLocalPostorder only includes live assertions from prior statements.
    //
    if (fgGlobalMorph && optLocalAssertionProp && (optAssertionCount > 0))
    {
        GenTree* optimizedTree = tree;
        bool     again         = JitConfig.JitEnablePostorderLocalAssertionProp() > 0;
        bool     didOptimize   = false;

        if (!again)
        {
            JITDUMP("*** Postorder assertion prop disabled by config\n");
        }

        while (again)
        {
            tree          = optimizedTree;
            optimizedTree = optAssertionProp(apLocalPostorder, tree, nullptr, nullptr);
            again         = (optimizedTree != nullptr);
            didOptimize |= again;
        }

        assert(tree != nullptr);

        if (didOptimize)
        {
            gtUpdateNodeSideEffects(tree);
        }
    }

    // Try to fold it, maybe we get lucky,
    tree = gtFoldExpr(tree);

    if (oldTree != tree)
    {
        /* if gtFoldExpr returned op1 or op2 then we are done */
        if ((tree == op1) || (tree == op2) || (tree == qmarkOp1) || (tree == qmarkOp2))
        {
            return tree;
        }

        /* If we created a comma-throw tree then we need to morph op1 */
        if (fgIsCommaThrow(tree))
        {
            tree->AsOp()->gtOp1 = fgMorphTree(tree->AsOp()->gtOp1);
            fgMorphTreeDone(tree);
            return tree;
        }

        return tree;
    }
    else if (tree->OperIsConst())
    {
        return tree;
    }
    else if (tree->IsNothingNode())
    {
        return tree;
    }

    /* gtFoldExpr could have used setOper to change the oper */
    oper = tree->OperGet();
    typ  = tree->TypeGet();

    /* gtFoldExpr could have changed op1 and op2 */
    op1 = tree->AsOp()->gtOp1;
    op2 = tree->gtGetOp2IfPresent();

    /*-------------------------------------------------------------------------
     * Perform the required oper-specific postorder morphing
     */

    switch (oper)
    {
        case GT_STORE_LCL_VAR:
        case GT_STORE_LCL_FLD:
        case GT_STOREIND:
            tree = fgOptimizeCastOnStore(tree);
            op1  = tree->gtGetOp1();
            op2  = tree->gtGetOp2IfPresent();

            if (tree->OperIs(GT_STOREIND))
            {
                GenTree* optimizedTree = fgMorphFinalizeIndir(tree->AsIndir());
                if (optimizedTree != nullptr)
                {
                    return optimizedTree;
                }
            }
            break;

        case GT_CAST:
            tree = fgOptimizeCast(tree->AsCast());
            if (!tree->OperIsSimple())
            {
                return tree;
            }
            if (tree->OperIs(GT_CAST) && tree->gtOverflow())
            {
                fgAddCodeRef(compCurBB, SCK_OVERFLOW);
            }

            typ  = tree->TypeGet();
            oper = tree->OperGet();
            op1  = tree->AsOp()->gtGetOp1();
            op2  = tree->gtGetOp2IfPresent();
            break;

        case GT_BITCAST:
        {
            GenTree* optimizedTree = fgOptimizeBitCast(tree->AsUnOp());
            if (optimizedTree != nullptr)
            {
                return optimizedTree;
            }
        }
        break;

        case GT_EQ:
        case GT_NE:
            if (op2->IsIntegralConst())
            {
                tree = fgOptimizeEqualityComparisonWithConst(tree->AsOp());
                assert(tree->OperIsCompare());

                oper = tree->OperGet();
                op1  = tree->gtGetOp1();
                op2  = tree->gtGetOp2();
            }
            goto COMPARE;

        case GT_LT:
        case GT_LE:
        case GT_GE:
        case GT_GT:
            // Change "CNS relop op2" to "op2 relop* CNS"
            if (op1->IsIntegralConst() && tree->OperIsCompare() && gtCanSwapOrder(op1, op2))
            {
                std::swap(tree->AsOp()->gtOp1, tree->AsOp()->gtOp2);
                tree->gtOper = GenTree::SwapRelop(tree->OperGet());

                oper = tree->OperGet();
                op1  = tree->gtGetOp1();
                op2  = tree->gtGetOp2();
            }

            if (op1->OperIs(GT_CAST) || op2->OperIs(GT_CAST))
            {
                tree = fgOptimizeRelationalComparisonWithCasts(tree->AsOp());
                oper = tree->OperGet();
                op1  = tree->gtGetOp1();
                op2  = tree->gtGetOp2();
            }

            if (op2->IsIntegralConst())
            {
                tree = fgOptimizeRelationalComparisonWithConst(tree->AsOp());
                oper = tree->OperGet();
                op1  = tree->gtGetOp1();
                op2  = tree->gtGetOp2();
            }

            if (opts.OptimizationEnabled() && fgGlobalMorph && tree->OperIs(GT_GT, GT_LT, GT_LE, GT_GE))
            {
                // Normalize unsigned comparisons to signed if both operands a known to be never negative.
                if (tree->IsUnsigned() && varTypeIsIntegral(op1) && op1->IsNeverNegative(this) &&
                    op2->IsNeverNegative(this))
                {
                    tree->ClearUnsigned();
                }

                if (op2->IsIntegralConst() || op1->IsIntegralConst())
                {
                    tree = fgOptimizeRelationalComparisonWithFullRangeConst(tree->AsOp());
                    if (tree->OperIs(GT_CNS_INT))
                    {
                        return tree;
                    }
                }
            }

        COMPARE:

            noway_assert(tree->OperIsCompare());
            break;

        case GT_MUL:

#ifndef TARGET_64BIT
            if (typ == TYP_LONG)
            {
                // This must be GTF_MUL_64RSLT
                INDEBUG(tree->AsOp()->DebugCheckLongMul());
                return tree;
            }
#endif // TARGET_64BIT
            goto CM_OVF_OP;

        case GT_SUB:

            if (tree->gtOverflow())
            {
                goto CM_OVF_OP;
            }

            if (!fgGlobalMorph)
            {
                break;
            }

            if (!op2->TypeIs(TYP_BYREF))
            {
                /* Check for "op1 - cns2" , we change it to "op1 + (-cns2)" */

                if (op2->IsCnsIntOrI() && !op2->IsIconHandle())
                {
                    // Negate the constant and change the node to be "+",
                    // except when `op2` is a const byref.

                    op2->AsIntConCommon()->SetIconValue(-op2->AsIntConCommon()->IconValue());
                    op2->AsIntConRef().gtFieldSeq = nullptr;
                    fgUpdateConstTreeValueNumber(op2);

                    oper = GT_ADD;
                    tree->ChangeOper(oper);
                    goto CM_ADD_OP;
                }

                /* Check for "cns1 - op2" , we change it to "(cns1 + (-op2))" */

                noway_assert(op1);
                if (op1->IsCnsIntOrI())
                {
                    noway_assert(varTypeIsIntegralOrI(tree));

                    // The type of the new GT_NEG node cannot just be op2->TypeGet().
                    // Otherwise we may sign-extend incorrectly in cases where the GT_NEG
                    // node ends up feeding directly into a cast, for example in
                    // GT_CAST<ubyte>(GT_SUB(0, s_1.ubyte))

                    if (op1->IsIntegralConst(0))
                    {
                        tree->ChangeOper(GT_NEG);
                        tree->gtType = genActualType(op2->TypeGet());

                        tree->AsOp()->gtOp1 = op2;
                        tree->AsOp()->gtOp2 = nullptr;

                        DEBUG_DESTROY_NODE(op1);
                        return tree;
                    }

                    tree->AsOp()->gtOp2 = op2 = gtNewOperNode(GT_NEG, genActualType(op2->TypeGet()), op2);
                    fgMorphTreeDone(op2);

                    oper = GT_ADD;
                    tree->ChangeOper(oper);
                    goto CM_ADD_OP;
                }
            }

            // Skip optimization if non-NEG operand is constant.
            // Both op1 and op2 are not constant because it was already checked above.
            if (opts.OptimizationEnabled())
            {
                if (!op2->OperIs(GT_NEG))
                {
                    break;
                }

                if (op1->OperIs(GT_NEG) && gtCanSwapOrder(op1, op2))
                {
                    // -a - -b = > b - a
                    // SUB(NEG(a), NEG(b)) => SUB(b, a)

                    // tree: SUB
                    // op1: NEG
                    // op1Child: a
                    // op2: NEG
                    // op2Child: b

                    GenTree* op1Child   = op1->AsOp()->gtOp1; // a
                    GenTree* op2Child   = op2->AsOp()->gtOp1; // b
                    tree->AsOp()->gtOp1 = op2Child;
                    tree->AsOp()->gtOp2 = op1Child;

                    DEBUG_DESTROY_NODE(op1);
                    DEBUG_DESTROY_NODE(op2);

                    op1 = op2Child;
                    op2 = op1Child;
                }
                else
                {
                    // a - -b = > a + b
                    // SUB(a, NEG(b)) => ADD(a, b)

                    // tree: SUB
                    // op1: a
                    // op2: NEG
                    // op2Child: b

                    GenTree* op2Child = op2->AsOp()->gtOp1; // b
                    oper              = GT_ADD;
                    tree->SetOper(oper, GenTree::PRESERVE_VN);
                    tree->AsOp()->gtOp2 = op2Child;

                    DEBUG_DESTROY_NODE(op2);

                    op2 = op2Child;
                }
            }

            break;

        case GT_DIV:
        case GT_UDIV:
#if defined(TARGET_LOONGARCH64) || defined(TARGET_RISCV64)
        case GT_MOD:
        case GT_UMOD:
#endif // TARGET_LOONGARCH64 || TARGET_RISCV64
        {
#if defined(TARGET_ARM64) || defined(TARGET_LOONGARCH64) || defined(TARGET_RISCV64)
            if (!varTypeIsFloating(tree->gtType))
            {
                ExceptionSetFlags exSetFlags = tree->OperExceptions(this);

                if ((oper == GT_DIV) || (oper == GT_MOD))
                {
                    if ((exSetFlags & ExceptionSetFlags::ArithmeticException) != ExceptionSetFlags::None)
                    {
                        fgAddCodeRef(compCurBB, SCK_OVERFLOW);
                    }
                    else
                    {
                        tree->gtFlags |= GTF_DIV_MOD_NO_OVERFLOW;
                    }
                }

                if ((exSetFlags & ExceptionSetFlags::DivideByZeroException) != ExceptionSetFlags::None)
                {
                    fgAddCodeRef(compCurBB, SCK_DIV_BY_ZERO);
                }
                else
                {
                    tree->gtFlags |= GTF_DIV_MOD_NO_BY_ZERO;
                }
            }
#endif // TARGET_ARM64 || TARGET_LOONGARCH64 || TARGET_RISCV64

            if (opts.OptimizationDisabled())
            {
                break;
            }

            if (!op1->OperIs(GT_NEG))
            {
                break;
            }

            if (op2->IsCnsFltOrDbl())
            {
                // DIV(NEG(a), C) => DIV(a, NEG(C)); for floating-point
                tree->AsOp()->gtOp1 = op1->gtGetOp1();

                op2->AsDblCon()->SetDconValue(-op2->AsDblCon()->DconValue());
                fgUpdateConstTreeValueNumber(op2);

                DEBUG_DESTROY_NODE(op1);
                op1 = tree->AsOp()->gtOp1;
            }
            break;
        }

        case GT_ADD:

        CM_OVF_OP:
            if (tree->gtOverflow())
            {
                // Add the excptn-throwing basic block to jump to on overflow

                fgAddCodeRef(compCurBB, SCK_OVERFLOW);

                // We can't do any commutative morphing for overflow instructions

                break;
            }

        CM_ADD_OP:

            FALLTHROUGH;

        case GT_OR:
        case GT_XOR:
        case GT_AND:
            tree = fgOptimizeCommutativeArithmetic(tree->AsOp());
            if (!tree->OperIsSimple())
            {
                return tree;
            }
            typ  = tree->TypeGet();
            oper = tree->OperGet();
            op1  = tree->gtGetOp1();
            op2  = tree->gtGetOp2IfPresent();
            break;

        case GT_NOT:
        case GT_NEG:
        {
            if (opts.OptimizationDisabled())
            {
                break;
            }

            // Remove double negation/not.
            if (op1->OperIs(oper))
            {
                JITDUMP("Remove double negation/not\n")
                GenTree* op1op1 = op1->gtGetOp1();
                DEBUG_DESTROY_NODE(tree);
                DEBUG_DESTROY_NODE(op1);
                return op1op1;
            }

            // Distribute negation over simple multiplication/division expressions
            if (tree->OperIs(GT_NEG) && op1->OperIs(GT_MUL, GT_DIV))
            {
                GenTreeOp* mulOrDiv = op1->AsOp();
                GenTree*   op1op1   = mulOrDiv->gtGetOp1();
                GenTree*   op1op2   = mulOrDiv->gtGetOp2();

                if ((op1op2->IsCnsIntOrI() && !op1op2->IsIconHandle()) || op1op2->IsCnsFltOrDbl())
                {
                    // NEG(MUL(a, C)) => MUL(a, NEG(C))
                    // NEG(DIV(a, C)) => DIV(a, NEG(C)), except when C = {-1, 1} for integral

                    bool canTransform = true;

                    if (op1op2->IsCnsIntOrI())
                    {
                        if (mulOrDiv->OperIs(GT_DIV))
                        {
                            ssize_t constVal = op1op2->AsIntCon()->IconValue();
                            canTransform     = (constVal != -1) && (constVal != 1);
                        }
                        else
                        {
                            canTransform = !mulOrDiv->gtOverflow();
                        }

                        if (canTransform)
                        {
                            op1op2->AsIntConCommon()->SetIconValue(-op1op2->AsIntConCommon()->IconValue());
                            op1op2->AsIntConRef().gtFieldSeq = nullptr;
                        }
                    }
                    else
                    {
                        assert(op1op2->IsCnsFltOrDbl());
                        op1op2->AsDblCon()->SetDconValue(-op1op2->AsDblCon()->DconValue());
                    }
                    fgUpdateConstTreeValueNumber(op1op2);

                    if (canTransform)
                    {
                        mulOrDiv->SetVNsFromNode(tree);
                        DEBUG_DESTROY_NODE(tree);
                        return mulOrDiv;
                    }
                }
            }

            // Any constant cases should have been folded earlier
            noway_assert(!op1->OperIsConst() || op1->IsIconHandle());
            break;
        }

        case GT_CKFINITE:

            noway_assert(varTypeIsFloating(op1->TypeGet()));

            fgAddCodeRef(compCurBB, SCK_ARITH_EXCPN);
            break;

        case GT_BOUNDS_CHECK:
            setMethodHasBoundsChecks();
            fgAddCodeRef(compCurBB, tree->AsBoundsChk()->gtThrowKind);
            break;

        case GT_IND:
        {
            if (op1->IsIconHandle())
            {
                // All indirections with (handle) constant addresses are
                // nonfaulting.
                tree->gtFlags |= GTF_IND_NONFAULTING;

                // We know some handle types always point to invariant data.
                if (GenTree::HandleKindDataIsInvariant(op1->GetIconHandleFlag()))
                {
                    tree->gtFlags |= GTF_IND_INVARIANT;
                }
            }

            GenTree* optimizedTree = fgMorphFinalizeIndir(tree->AsIndir());
            if (optimizedTree != nullptr)
            {
                return optimizedTree;
            }

            // Only do this optimization when we are in the global optimizer. Doing this after value numbering
            // could result in an invalid value number for the newly generated GT_IND node.
            if (!varTypeIsStruct(tree) && op1->OperIs(GT_COMMA) && fgGlobalMorph)
            {
                // Perform the transform IND(COMMA(x, ..., z)) -> COMMA(x, ..., IND(z)).
                GenTree*     commaNode = op1;
                GenTreeFlags treeFlags = tree->gtFlags;
                commaNode->gtType      = typ;
                commaNode->gtFlags     = (treeFlags & ~GTF_REVERSE_OPS); // Bashing the GT_COMMA flags here is
                                                                         // dangerous, clear the GTF_REVERSE_OPS at
                                                                         // least.
                commaNode->SetMorphed(this);

                while (commaNode->AsOp()->gtOp2->OperIs(GT_COMMA))
                {
                    commaNode         = commaNode->AsOp()->gtOp2;
                    commaNode->gtType = typ;
                    commaNode->gtFlags =
                        (treeFlags & ~GTF_REVERSE_OPS & ~GTF_ASG & ~GTF_CALL); // Bashing the GT_COMMA flags here is
                    // dangerous, clear the GTF_REVERSE_OPS, GTF_ASG, and GTF_CALL at
                    // least.
                    commaNode->gtFlags |= ((commaNode->AsOp()->gtOp1->gtFlags | commaNode->AsOp()->gtOp2->gtFlags) &
                                           (GTF_ASG | GTF_CALL));
                    commaNode->SetMorphed(this);
                }

                tree          = op1;
                GenTree* addr = commaNode->AsOp()->gtOp2;
                // TODO-1stClassStructs: we often create a struct IND without a handle, fix it.
                op1 = gtNewIndir(typ, addr);

                // GTF_GLOB_EFFECT flags can be recomputed from the child
                // nodes. GTF_ORDER_SIDEEFF may be set already and indicate no
                // reordering is allowed with sibling nodes, so we cannot
                // recompute that.
                //
                op1->gtFlags |= treeFlags & ~GTF_GLOB_EFFECT;
                op1->gtFlags |= (addr->gtFlags & GTF_ALL_EFFECT);

                // if this was a non-faulting indir, clear GTF_EXCEPT,
                // unless we inherit it from the addr.
                //
                if (((treeFlags & GTF_IND_NONFAULTING) != 0) && ((addr->gtFlags & GTF_EXCEPT) == 0))
                {
                    op1->gtFlags &= ~GTF_EXCEPT;
                }

                op1->gtFlags |= treeFlags & GTF_GLOB_REF;
                op1->SetMorphed(this);
                commaNode->AsOp()->gtOp2 = op1;
                commaNode->gtFlags |= (op1->gtFlags & GTF_ALL_EFFECT);
                return tree;
            }
        }
        break;

        case GT_NULLCHECK:
            if (opts.OptimizationEnabled() && !tree->OperMayThrow(this))
            {
                JITDUMP("\nNULLCHECK on [%06u] will always succeed\n", dspTreeID(op1));
                if ((op1->gtFlags & GTF_SIDE_EFFECT) != 0)
                {
                    tree = gtUnusedValNode(op1);
                    tree->SetMorphed(this, /* doChildren */ true);
                }
                else
                {
                    tree->gtBashToNOP();
                }
                return tree;
            }
            break;

        case GT_COLON:
            if (fgGlobalMorph)
            {
                /* Mark the nodes that are conditionally executed */
                fgWalkTreePre(&tree, gtMarkColonCond);
            }
            /* Since we're doing this postorder we clear this if it got set by a child */
            fgRemoveRestOfBlock = false;
            break;

        case GT_COMMA:
        {
            /* Special case: trees that don't produce a value */
            if (op2->OperIsStore() || (op2->OperIs(GT_COMMA) && op2->TypeIs(TYP_VOID)) || fgIsThrow(op2))
            {
                typ = tree->gtType = TYP_VOID;
            }

            // Extract the side effects from the left side of the comma.  Since they don't "go" anywhere, this
            // is all we need.

            GenTree* op1SideEffects = nullptr;
            // The addition of "GTF_MAKE_CSE" below prevents us from throwing away (for example)
            // hoisted expressions in loops.
            gtExtractSideEffList(op1, &op1SideEffects, (GTF_SIDE_EFFECT | GTF_MAKE_CSE));
            if (op1SideEffects)
            {
                // Replace the left hand side with the side effect list.
                op1                 = op1SideEffects;
                tree->AsOp()->gtOp1 = op1SideEffects;
                gtUpdateNodeSideEffects(tree);
            }
            else
            {
                op2->gtFlags |= (tree->gtFlags & GTF_DONT_CSE);
                DEBUG_DESTROY_NODE(tree);
                DEBUG_DESTROY_NODE(op1);
                return op2;
            }

            // If the right operand is just a void nop node, throw it away. Unless this is a
            // comma throw, in which case we want the top-level morphing loop to recognize it.
            if (op2->IsNothingNode() && op1->TypeIs(TYP_VOID) && !fgIsCommaThrow(tree))
            {
                op1->gtFlags |= (tree->gtFlags & GTF_DONT_CSE);
                DEBUG_DESTROY_NODE(tree);
                DEBUG_DESTROY_NODE(op2);
                return op1;
            }
            break;
        }

        case GT_JTRUE:

            /* Special case if fgRemoveRestOfBlock is set to true */
            if (fgRemoveRestOfBlock)
            {
                if (fgIsCommaThrow(op1, true))
                {
                    GenTree* throwNode = op1->AsOp()->gtOp1;

                    JITDUMP("Removing [%06d] GT_JTRUE as the block now unconditionally throws an exception.\n",
                            dspTreeID(tree));
                    DEBUG_DESTROY_NODE(tree);

                    return throwNode;
                }

                noway_assert(op1->OperIsCompare());
                noway_assert(op1->gtFlags & GTF_EXCEPT);

                // We need to keep op1 for the side-effects. Hang it off
                // a GT_COMMA node

                JITDUMP("Keeping side-effects by bashing [%06d] GT_JTRUE into a GT_COMMA.\n", dspTreeID(tree));

                tree->ChangeOper(GT_COMMA);
                tree->AsOp()->gtOp2 = op2 = gtNewNothingNode();

                // Additionally since we're eliminating the JTRUE
                // codegen won't like it if op1 is a RELOP of longs, floats or doubles.
                // So we change it into a GT_COMMA as well.
                JITDUMP("Also bashing [%06d] (a relop) into a GT_COMMA.\n", dspTreeID(op1));
                op1->ChangeOper(GT_COMMA);
                op1->gtFlags &= ~GTF_UNSIGNED; // Clear the unsigned flag if it was set on the relop
                op1->gtType = op1->AsOp()->gtOp1->gtType;

                return tree;
            }
            break;

        case GT_INTRINSIC:
            if (tree->AsIntrinsic()->gtIntrinsicName ==
                NI_System_Runtime_CompilerServices_RuntimeHelpers_IsKnownConstant)
            {
                JITDUMP("\nExpanding RuntimeHelpers.IsKnownConstant to ");
                if (op1->OperIsConst() || gtIsTypeof(op1))
                {
                    // We're lucky to catch a constant here while importer was not
                    JITDUMP("true\n");
                    DEBUG_DESTROY_NODE(tree, op1);
                    tree = gtNewIconNode(1);
                }
                else
                {
                    JITDUMP("false\n");
                    tree = gtWrapWithSideEffects(gtNewIconNode(0), op1, GTF_ALL_EFFECT);
                }
                tree->SetMorphed(this);
                return tree;
            }
            break;

        case GT_RETURN:
        case GT_SWIFT_ERROR_RET:
        {
            // Retry updating return operand to a field -- assertion
            // prop done when morphing this operand changed the local.
            // Skip this for merged returns that will be changed to a store and
            // jump to the return BB.
            GenTree*& retVal = tree->AsOp()->ReturnValueRef();
            if ((retVal != nullptr) && ((genReturnBB == nullptr) || (compCurBB == genReturnBB)))
            {
                if (fgTryReplaceStructLocalWithFields(&retVal))
                {
                    retVal = fgMorphTree(retVal);
                }
            }
            break;
        }

        default:
            break;
    }

    assert(oper == tree->gtOper);

    // Propagate comma throws. Only done in global morph since this does not preserve VNs.
    if (fgGlobalMorph && (oper != GT_COLON) &&
        /* TODO-ASG-Cleanup: delete this zero-diff quirk */ !GenTree::OperIsStore(oper))
    {
        if ((op1 != nullptr) && fgIsCommaThrow(op1, true))
        {
            GenTree* propagatedThrow = fgPropagateCommaThrow(tree, op1->AsOp(), GTF_EMPTY);
            if (propagatedThrow != nullptr)
            {
                return propagatedThrow;
            }
        }

        if ((op2 != nullptr) && fgIsCommaThrow(op2, true))
        {
            GenTree* propagatedThrow = fgPropagateCommaThrow(tree, op2->AsOp(), op1->gtFlags & GTF_ALL_EFFECT);
            if (propagatedThrow != nullptr)
            {
                return propagatedThrow;
            }
        }
    }

    /*-------------------------------------------------------------------------
     * Optional morphing is done if tree transformations is permitted
     */

    if ((opts.compFlags & CLFLG_TREETRANS) == 0)
    {
        return tree;
    }

    tree = fgMorphSmpOpOptional(tree->AsOp(), optAssertionPropDone);

    return tree;
}

//------------------------------------------------------------------------
// fgTryReplaceStructLocalWithField: see if a struct use can be replaced
//   with an equivalent field use
//
// Arguments:
//    tree - tree to examine and possibly modify
//
// Notes:
//    Currently only called when the tree parent is a GT_RETURN/GT_SWIFT_ERROR_RET.
//
bool Compiler::fgTryReplaceStructLocalWithFields(GenTree** use)
{
    if (!(*use)->OperIs(GT_LCL_VAR))
    {
        return false;
    }

    LclVarDsc* varDsc = lvaGetDesc((*use)->AsLclVar());

    if (varDsc->lvDoNotEnregister || !varDsc->lvPromoted)
    {
        return false;
    }

    *use = fgMorphLclToFieldList((*use)->AsLclVar());
    return true;
}

//------------------------------------------------------------------------
// fgMorphFinalizeIndir: Finalize morphing an indirection.
//
// Turns indirections off of local addresses into local field nodes.
// Adds UNALIGNED for some accesses on ARM for backwards compatibility.
//
// Arguments:
//    indir - The indirection to morph (can be a store)
//
// Return Value:
//    The optimized tree or "nullptr" if no transformations that would
//    replace it were performed.
//
GenTree* Compiler::fgMorphFinalizeIndir(GenTreeIndir* indir)
{
    assert(indir->isIndir());
    GenTree* addr = indir->Addr();

#ifdef TARGET_ARM
    if (varTypeIsFloating(indir))
    {
        // Check for a misaligned floating point indirection.
        GenTree*       effAddr = addr->gtEffectiveVal();
        target_ssize_t offset;
        gtPeelOffsets(&effAddr, &offset);

        if (((offset % genTypeSize(TYP_FLOAT)) != 0) ||
            (effAddr->IsCnsIntOrI() && ((effAddr->AsIntConCommon()->IconValue() % genTypeSize(TYP_FLOAT)) != 0)))
        {
            indir->gtFlags |= GTF_IND_UNALIGNED;
        }
    }
#endif // TARGET_ARM

    if (!indir->IsVolatile() && !indir->TypeIs(TYP_STRUCT) && addr->OperIs(GT_LCL_ADDR))
    {
        unsigned size    = indir->Size();
        unsigned offset  = addr->AsLclVarCommon()->GetLclOffs();
        unsigned extent  = offset + size;
        unsigned lclSize = lvaLclExactSize(addr->AsLclVarCommon()->GetLclNum());

        if ((extent <= lclSize) && (extent < UINT16_MAX))
        {
            addr->ChangeType(indir->TypeGet());
            if (indir->OperIs(GT_STOREIND))
            {
                GenTree* value = indir->Data();
                addr->SetOper(GT_STORE_LCL_FLD);
                addr->AsLclFld()->Data() = value;
                addr->gtFlags |= (GTF_ASG | GTF_VAR_DEF);
                addr->AddAllEffectsFlags(value);
            }
            else
            {
                assert(indir->OperIs(GT_IND));
                addr->SetOper(GT_LCL_FLD);
            }
            addr->AsLclFld()->SetLclOffs(offset);
            addr->SetVNsFromNode(indir);
            addr->AddAllEffectsFlags(indir->gtFlags & GTF_GLOB_REF);

            if (addr->OperIs(GT_STORE_LCL_FLD) && addr->IsPartialLclFld(this))
            {
                addr->gtFlags |= GTF_VAR_USEASG;
            }

            return addr;
        }
    }

    return nullptr;
}

//------------------------------------------------------------------------
// fgOptimizeCast: Optimizes the supplied GT_CAST tree.
//
// Tries to get rid of the cast, its operand, the GTF_OVERFLOW flag, calls
// calls "optNarrowTree". Called in post-order by "fgMorphSmpOp".
//
// Arguments:
//    tree - the cast tree to optimize
//
// Return Value:
//    The optimized tree (that can have any shape).
//
GenTree* Compiler::fgOptimizeCast(GenTreeCast* cast)
{
    GenTree* src = cast->CastOp();

    // See if we can discard the cast.
    if (varTypeIsIntegral(cast) && varTypeIsIntegral(src))
    {
        IntegralRange srcRange   = IntegralRange::ForNode(src, this);
        IntegralRange noOvfRange = IntegralRange::ForCastInput(cast);

        if (noOvfRange.Contains(srcRange))
        {
            // Casting between same-sized types is a no-op,
            // given we have proven this cast cannot overflow.
            if (genActualType(cast) == genActualType(src))
            {
                return src;
            }

            cast->ClearOverflow();
            cast->SetAllEffectsFlags(src);

            // Try and see if we can make this cast into a cheaper zero-extending version.
            if (genActualTypeIsInt(src) && cast->TypeIs(TYP_LONG) && srcRange.IsNonNegative())
            {
                cast->SetUnsigned();
            }
        }

        // For checked casts, we're done.
        if (cast->gtOverflow())
        {
            return cast;
        }

        var_types castToType = cast->CastToType();

        // For indir-like nodes, we may be able to change their type to satisfy (and discard) the cast.
        if (varTypeIsSmall(castToType) && (genTypeSize(castToType) == genTypeSize(src)) &&
            src->OperIs(GT_IND, GT_LCL_FLD))
        {
            // We're changing the type here so we need to update the VN;
            // in other cases we discard the cast without modifying src
            // so the VN doesn't change.

            src->ChangeType(castToType);
            src->SetVNsFromNode(cast);

            return src;
        }

        // Try to narrow the operand of the cast and discard the cast.
        if (opts.OptEnabled(CLFLG_TREETRANS) && (genTypeSize(src) > genTypeSize(castToType)) &&
            optNarrowTree(src, src->TypeGet(), castToType, cast->gtVNPair, false))
        {
            optNarrowTree(src, src->TypeGet(), castToType, cast->gtVNPair, true);

            // "optNarrowTree" may leave a redundant cast behind.
            if (src->OperIs(GT_CAST) && (src->AsCast()->CastToType() == genActualType(src->AsCast()->CastOp())))
            {
                src = src->AsCast()->CastOp();
            }

            return src;
        }

        // Check for two consecutive casts, we may be able to discard the intermediate one.
        if (opts.OptimizationEnabled() && src->OperIs(GT_CAST) && !src->gtOverflow())
        {
            var_types dstCastToType = castToType;
            var_types srcCastToType = src->AsCast()->CastToType();

            // CAST(ubyte <- CAST(short <- X)): CAST(ubyte <- X).
            // CAST(ushort <- CAST(short <- X)): CAST(ushort <- X).
            if (varTypeIsSmall(srcCastToType) && (genTypeSize(dstCastToType) <= genTypeSize(srcCastToType)))
            {
                cast->CastOp() = src->AsCast()->CastOp();
                DEBUG_DESTROY_NODE(src);
            }
        }
    }

    return cast;
}

//------------------------------------------------------------------------
// fgOptimizeCastOnStore: Optimizes the supplied store tree with a GT_CAST node.
//
// Arguments:
//    tree - the store to optimize
//
// Return Value:
//    The optimized store tree.
//
GenTree* Compiler::fgOptimizeCastOnStore(GenTree* store)
{
    assert(store->OperIsStore());

    GenTree* const src = store->Data();

    if (!src->OperIs(GT_CAST))
        return store;

    if (store->OperIs(GT_STORE_LCL_VAR))
    {
        LclVarDsc* varDsc = lvaGetDesc(store->AsLclVarCommon()->GetLclNum());

        // We can make this transformation only under the assumption that NOL locals are always normalized before they
        // are used,
        // however this is not always the case: the JIT will utilize subrange assertions for NOL locals to make
        // normalization
        // assumptions -- see fgMorphLeafLocal. Thus we can only do this for cases where we know for sure that
        // subsequent uses
        // will normalize, which we can only guarantee when the local is address exposed.
        if (!varDsc->lvNormalizeOnLoad() || !varDsc->IsAddressExposed())
            return store;
    }

    if (src->gtOverflow())
        return store;

    GenTreeCast* cast         = src->AsCast();
    var_types    castToType   = cast->CastToType();
    var_types    castFromType = cast->CastFromType();

    if (!varTypeIsSmall(store))
        return store;

    if (!varTypeIsSmall(castToType))
        return store;

    if (!varTypeIsIntegral(castFromType))
        return store;

    // If we are performing a narrowing cast and
    // castToType is larger or the same as op1's type
    // then we can discard the cast.
    if (genTypeSize(castToType) < genTypeSize(store))
        return store;

    if (genActualType(castFromType) == genActualType(castToType))
    {
        // Removes the cast.
        store->Data() = cast->CastOp();
    }
    else
    {
        // This is a type-changing cast so we cannot remove it entirely.
        cast->gtCastType = genActualType(castToType);

        // See if we can optimize the new cast.
        store->Data() = fgOptimizeCast(cast);
    }

    return store;
}

//------------------------------------------------------------------------
// fgOptimizeBitCast: Optimizes the supplied BITCAST node.
//
// Retypes the source node and removes the cast if possible.
//
// Arguments:
//    bitCast - the BITCAST node
//
// Return Value:
//    The optimized tree or "nullptr" if no transformations were performed.
//
GenTree* Compiler::fgOptimizeBitCast(GenTreeUnOp* bitCast)
{
    if (opts.OptimizationDisabled())
    {
        return nullptr;
    }

    GenTree* op1 = bitCast->gtGetOp1();
    if (op1->OperIs(GT_IND, GT_LCL_FLD) && (genTypeSize(op1) == genTypeSize(bitCast)))
    {
        op1->ChangeType(bitCast->TypeGet());
        op1->SetVNsFromNode(bitCast);
        return op1;
    }

    return nullptr;
}

//------------------------------------------------------------------------
// fgOptimizeEqualityComparisonWithConst: optimizes various EQ/NE(OP, CONST) patterns.
//
// Arguments:
//    cmp - The GT_NE/GT_EQ tree the second operand of which is an integral constant
//
// Return Value:
//    The optimized tree, "cmp" in case no optimizations were done.
//    Currently only returns relop trees.
//
GenTree* Compiler::fgOptimizeEqualityComparisonWithConst(GenTreeOp* cmp)
{
    assert(cmp->OperIs(GT_EQ, GT_NE));
    assert(cmp->gtGetOp2()->IsIntegralConst());

    GenTree*             op1 = cmp->gtGetOp1();
    GenTreeIntConCommon* op2 = cmp->gtGetOp2()->AsIntConCommon();

    // Check for "(expr +/- icon1) ==/!= (non-zero-icon2)".
    if (op2->IsCnsIntOrI() && (op2->IconValue() != 0))
    {
        // Since this can occur repeatedly we use a while loop.
        while (op1->OperIs(GT_ADD, GT_SUB) && op1->AsOp()->gtGetOp2()->IsCnsIntOrI() && op1->TypeIs(TYP_INT) &&
               !op1->gtOverflow())
        {
            // Got it; change "x + icon1 == icon2" to "x == icon2 - icon1".
            ssize_t op1Value = op1->AsOp()->gtGetOp2()->AsIntCon()->IconValue();
            ssize_t op2Value = op2->IconValue();

            if (op1->OperIs(GT_ADD))
            {
                op2Value -= op1Value;
            }
            else
            {
                op2Value += op1Value;
            }

            op1 = op1->AsOp()->gtGetOp1();
            op2->SetIconValue(static_cast<int32_t>(op2Value));
        }

        cmp->gtOp1 = op1;
        fgUpdateConstTreeValueNumber(op2);
    }

    // Here we look for the following tree
    //
    //                        EQ/NE
    //                        /  \.
    //                      op1   CNS 0/1
    //
    if (op2->IsIntegralConst(0) || op2->IsIntegralConst(1))
    {
        ssize_t op2Value = static_cast<ssize_t>(op2->IntegralValue());

        if (op1->OperIsCompare())
        {
            // Here we look for the following tree
            //
            //                        EQ/NE           ->      RELOP/!RELOP
            //                        /  \                       /    \.
            //                     RELOP  CNS 0/1
            //                     /   \.
            //
            // Note that we will remove/destroy the EQ/NE node and move
            // the RELOP up into it's location.

            // Here we reverse the RELOP if necessary.

            bool reverse = ((op2Value == 0) == cmp->OperIs(GT_EQ));

            if (reverse)
            {
                gtReverseCond(op1);
            }

            noway_assert((op1->gtFlags & GTF_RELOP_JMP_USED) == 0);
            op1->gtFlags |= cmp->gtFlags & (GTF_RELOP_JMP_USED | GTF_DONT_CSE);
            op1->SetVNsFromNode(cmp);

            DEBUG_DESTROY_NODE(cmp);
            return op1;
        }

        //
        // Now we check for a compare with the result of an '&' operator
        //
        // Here we look for the following transformation (canonicalization):
        //
        //                        EQ/NE                  EQ/NE
        //                        /  \                   /  \.
        //                      AND   CNS 0/1  ->      AND   CNS 0
        //                     /   \                  /   \.
        //                RSZ/RSH   CNS 1            x     LSH  (folded if 'y' is constant)
        //                  /  \                          /   \.
        //                 x    y                        1     y

        if (fgGlobalMorph && op1->OperIs(GT_AND) && op1->AsOp()->gtGetOp1()->OperIs(GT_RSZ, GT_RSH))
        {
            GenTreeOp* andOp    = op1->AsOp();
            GenTreeOp* rshiftOp = andOp->gtGetOp1()->AsOp();

            if (!andOp->gtGetOp2()->IsIntegralConst(1))
            {
                goto SKIP;
            }

            // If the shift is constant, we can fold the mask and delete the shift node:
            //   -> AND(x, CNS(1 << y)) EQ/NE 0
            if (rshiftOp->gtGetOp2()->IsCnsIntOrI())
            {
                ssize_t shiftAmount = rshiftOp->gtGetOp2()->AsIntCon()->IconValue();

                if (shiftAmount < 0)
                {
                    goto SKIP;
                }

                GenTreeIntConCommon* andMask = andOp->gtGetOp2()->AsIntConCommon();

                if (andOp->TypeIs(TYP_INT) && shiftAmount < 32)
                {
                    andMask->SetIconValue(static_cast<int32_t>(1 << shiftAmount));
                }
                else if (andOp->TypeIs(TYP_LONG) && shiftAmount < 64)
                {
                    andMask->SetLngValue(1LL << shiftAmount);
                }
                else
                {
                    goto SKIP; // Unsupported type or invalid shift amount.
                }
                andOp->gtOp1 = rshiftOp->gtGetOp1();

                DEBUG_DESTROY_NODE(rshiftOp->gtGetOp2());
                DEBUG_DESTROY_NODE(rshiftOp);
            }
            // Otherwise, if the shift is not constant, just rewire the nodes and reverse the shift op:
            //   AND(RSH(x, y), 1)  ->  AND(x, LSH(1, y))
            //
            // On ARM/BMI2 the original pattern should result in smaller code when comparing to non-zero,
            // the other case where this transform is worth is if the compare is being used by a jump.
            //
            else
            {
                if (!(cmp->gtFlags & GTF_RELOP_JMP_USED) &&
                    ((op2Value == 0 && cmp->OperIs(GT_NE)) || (op2Value == 1 && cmp->OperIs(GT_EQ))))
                {
                    goto SKIP;
                }

                andOp->gtOp1    = rshiftOp->gtGetOp1();
                rshiftOp->gtOp1 = andOp->gtGetOp2();
                andOp->gtOp2    = rshiftOp;

                rshiftOp->SetOper(GT_LSH);
                gtUpdateNodeSideEffects(rshiftOp);
            }

            // Reverse the condition if necessary.
            if (op2Value == 1)
            {
                gtReverseCond(cmp);
                op2->SetIntegralValue(0);
            }
        }
    }

SKIP:

    // Now check for compares with small constant longs that can be cast to int.
    // Note that we filter out negative values here so that the transformations
    // below are correct. E. g. "EQ(-1L, CAST_UN(int))" is always "false", but were
    // we to make it into "EQ(-1, int)", "true" becomes possible for negative inputs.
    if (!op2->TypeIs(TYP_LONG) || ((op2->LngValue() >> 31) != 0))
    {
        return cmp;
    }

    if (!op1->OperIs(GT_AND))
    {
        // Another interesting case: cast from int.
        if (op1->OperIs(GT_CAST) && op1->AsCast()->CastOp()->TypeIs(TYP_INT) && !op1->gtOverflow())
        {
            // Simply make this into an integer comparison.
            cmp->gtOp1 = op1->AsCast()->CastOp();

            op2->BashToConst(static_cast<int32_t>(op2->LngValue()));
            fgUpdateConstTreeValueNumber(op2);
        }

        return cmp;
    }

    // Now we perform the following optimization:
    // EQ/NE(AND(OP long, CNS_LNG), CNS_LNG) =>
    // EQ/NE(AND(CAST(int <- OP), CNS_INT), CNS_INT)
    // when the constants are sufficiently small.
    // This transform cannot preserve VNs.
    if (fgGlobalMorph)
    {
        assert(op1->TypeIs(TYP_LONG) && op1->OperIs(GT_AND));

        // Is the result of the mask effectively an INT?
        GenTreeOp* andOp = op1->AsOp();
        if (!andOp->gtGetOp2()->OperIs(GT_CNS_NATIVELONG))
        {
            return cmp;
        }

        GenTreeIntConCommon* andMask = andOp->gtGetOp2()->AsIntConCommon();
        if ((andMask->LngValue() >> 32) != 0)
        {
            return cmp;
        }

        GenTree* andOpOp1 = andOp->gtGetOp1();
        // Now we narrow the first operand of AND to int.
        if (optNarrowTree(andOpOp1, TYP_LONG, TYP_INT, ValueNumPair(), false))
        {
            optNarrowTree(andOpOp1, TYP_LONG, TYP_INT, ValueNumPair(), true);

            // "optNarrowTree" may leave a redundant cast behind.
            if (andOpOp1->OperIs(GT_CAST) &&
                (andOpOp1->AsCast()->CastToType() == genActualType(andOpOp1->AsCast()->CastOp())))
            {
                andOp->gtOp1 = andOpOp1->AsCast()->CastOp();
            }
        }
        else
        {
            GenTree* const newOp1 = gtNewCastNode(TYP_INT, andOp->gtGetOp1(), false, TYP_INT);
            newOp1->SetMorphed(this);
            andOp->gtOp1 = newOp1;
        }

        assert(andMask == andOp->gtGetOp2());

        // Now replace the mask node.
        andMask->BashToConst(static_cast<int32_t>(andMask->LngValue()));

        // Now change the type of the AND node.
        andOp->ChangeType(TYP_INT);

        // Finally we replace the comparand.
        op2->BashToConst(static_cast<int32_t>(op2->LngValue()));
    }

    return cmp;
}

//------------------------------------------------------------------------
// fgOptimizeRelationalComparisonWithFullRangeConst: optimizes a comparison operation.
//
// Recognizes "Always false"/"Always true" comparisons against various full range constant operands and morphs
// them into zero/one.
//
// Arguments:
//   cmp - the GT_LT/GT_GT tree to morph.
//
// Return Value:
//   1. The unmodified "cmp" tree.
//   2. A CNS_INT node containing zero.
//   3. A CNS_INT node containing one.
// Assumptions:
//   The second operand is an integral constant or the first operand is an integral constant.
//
GenTree* Compiler::fgOptimizeRelationalComparisonWithFullRangeConst(GenTreeOp* cmp)
{
    if (gtTreeHasSideEffects(cmp, GTF_SIDE_EFFECT))
    {
        return cmp;
    }

    GenTree* const op1 = cmp->gtGetOp1();
    GenTree* const op2 = cmp->gtGetOp2();

    if (!varTypeIsIntegral(op1->TypeGet()) || !varTypeIsIntegral(op2->TypeGet()))
    {
        return cmp;
    }

    int64_t lhsMin;
    int64_t lhsMax;
    if (cmp->gtGetOp1()->IsIntegralConst())
    {
        lhsMin = cmp->gtGetOp1()->AsIntConCommon()->IntegralValue();
        lhsMax = lhsMin;
    }
    else
    {
        IntegralRange lhsRange = IntegralRange::ForNode(cmp->gtGetOp1(), this);
        lhsMin                 = IntegralRange::SymbolicToRealValue(lhsRange.GetLowerBound());
        lhsMax                 = IntegralRange::SymbolicToRealValue(lhsRange.GetUpperBound());
    }

    int64_t rhsMin;
    int64_t rhsMax;
    if (cmp->gtGetOp2()->IsIntegralConst())
    {
        rhsMin = cmp->gtGetOp2()->AsIntConCommon()->IntegralValue();
        rhsMax = rhsMin;
    }
    else
    {
        IntegralRange rhsRange = IntegralRange::ForNode(cmp->gtGetOp2(), this);
        rhsMin                 = IntegralRange::SymbolicToRealValue(rhsRange.GetLowerBound());
        rhsMax                 = IntegralRange::SymbolicToRealValue(rhsRange.GetUpperBound());
    }

    genTreeOps op = cmp->gtOper;
    if ((op != GT_LT) && (op != GT_LE))
    {
        op = GenTree::SwapRelop(op);
        std::swap(lhsMin, rhsMin);
        std::swap(lhsMax, rhsMax);
    }

    GenTree* ret = nullptr;

    if (cmp->IsUnsigned())
    {
        if ((lhsMin < 0) && (lhsMax >= 0))
        {
            // [0, (uint64_t)lhsMax] U [(uint64_t)lhsMin, MaxValue]
            lhsMin = 0;
            lhsMax = -1;
        }

        if ((rhsMin < 0) && (rhsMax >= 0))
        {
            // [0, (uint64_t)rhsMax] U [(uint64_t)rhsMin, MaxValue]
            rhsMin = 0;
            rhsMax = -1;
        }

        if (((op == GT_LT) && ((uint64_t)lhsMax < (uint64_t)rhsMin)) ||
            ((op == GT_LE) && ((uint64_t)lhsMax <= (uint64_t)rhsMin)))
        {
            ret = gtNewOneConNode(TYP_INT);
        }
        else if (((op == GT_LT) && ((uint64_t)lhsMin >= (uint64_t)rhsMax)) ||
                 ((op == GT_LE) && ((uint64_t)lhsMin > (uint64_t)rhsMax)))
        {
            ret = gtNewZeroConNode(TYP_INT);
        }
    }
    else
    {
        //  [x0, x1] <  [y0, y1] is false if x0 >= y1
        //  [x0, x1] <= [y0, y1] is false if x0 > y1
        if (((op == GT_LT) && (lhsMin >= rhsMax)) || (((op == GT_LE) && (lhsMin > rhsMax))))
        {
            ret = gtNewZeroConNode(TYP_INT);
        }
        // [x0, x1] < [y0, y1] is true if x1 < y0
        // [x0, x1] <= [y0, y1] is true if x1 <= y0
        else if (((op == GT_LT) && (lhsMax < rhsMin)) || ((op == GT_LE) && (lhsMax <= rhsMin)))
        {
            ret = gtNewOneConNode(TYP_INT);
        }
    }

    if (ret != nullptr)
    {
        fgUpdateConstTreeValueNumber(ret);

        DEBUG_DESTROY_NODE(cmp);
        ret->SetMorphed(this);

        return ret;
    }

    return cmp;
}

//------------------------------------------------------------------------
// fgOptimizeRelationalComparisonWithConst: optimizes a comparison operation.
//
// Recognizes comparisons against various constant operands and morphs
// them, if possible, into comparisons against zero.
//
// Arguments:
//   cmp - the GT_LE/GT_LT/GT_GE/GT_GT tree to morph.
//
// Return Value:
//   The "cmp" tree, possibly with a modified oper.
//   The second operand's constant value may be modified as well.
//
// Assumptions:
//   The operands have been swapped so that any constants are on the right.
//   The second operand is an integral constant.
//
GenTree* Compiler::fgOptimizeRelationalComparisonWithConst(GenTreeOp* cmp)
{
    assert(cmp->OperIs(GT_LE, GT_LT, GT_GE, GT_GT));
    assert(cmp->gtGetOp2()->IsIntegralConst());

    GenTree*             op1 = cmp->gtGetOp1();
    GenTreeIntConCommon* op2 = cmp->gtGetOp2()->AsIntConCommon();

    assert(genActualType(op1) == genActualType(op2));

    genTreeOps oper     = cmp->OperGet();
    int64_t    op2Value = op2->IntegralValue();

    if (op2Value == 1)
    {
        // Check for "expr >= 1".
        if (oper == GT_GE)
        {
            // Change to "expr != 0" for unsigned and "expr > 0" for signed.
            oper = cmp->IsUnsigned() ? GT_NE : GT_GT;
        }
        // Check for "expr < 1".
        else if (oper == GT_LT)
        {
            // Change to "expr == 0" for unsigned and "expr <= 0".
            oper = cmp->IsUnsigned() ? GT_EQ : GT_LE;
        }
    }
    // Check for "expr relop -1".
    else if (!cmp->IsUnsigned() && (op2Value == -1))
    {
        // Check for "expr <= -1".
        if (oper == GT_LE)
        {
            // Change to "expr < 0".
            oper = GT_LT;
        }
        // Check for "expr > -1".
        else if (oper == GT_GT)
        {
            // Change to "expr >= 0".
            oper = GT_GE;
        }
    }
    else if (cmp->IsUnsigned())
    {
        if ((oper == GT_LE) || (oper == GT_GT))
        {
            if (op2Value == 0)
            {
                // IL doesn't have a cne instruction so compilers use cgt.un instead. The JIT
                // recognizes certain patterns that involve GT_NE (e.g (x & 4) != 0) and fails
                // if GT_GT is used instead. Transform (x GT_GT.unsigned 0) into (x GT_NE 0)
                // and (x GT_LE.unsigned 0) into (x GT_EQ 0). The later case is rare, it sometimes
                // occurs as a result of branch inversion.
                oper = (oper == GT_LE) ? GT_EQ : GT_NE;
                cmp->gtFlags &= ~GTF_UNSIGNED;
            }
            // LE_UN/GT_UN(expr, int/long.MaxValue) => GE/LT(expr, 0).
            else if (((op1->TypeIs(TYP_LONG) && (op2Value == INT64_MAX))) ||
                     ((genActualType(op1) == TYP_INT) && (op2Value == INT32_MAX)))
            {
                oper = (oper == GT_LE) ? GT_GE : GT_LT;
                cmp->gtFlags &= ~GTF_UNSIGNED;
            }
            // LE_UN/GT_UN(expr, int.MaxValue) => EQ/NE(RSZ(expr, 32), 0).
            else if (opts.OptimizationEnabled() && (op1->TypeIs(TYP_LONG) && (op2Value == UINT_MAX)))
            {
                oper            = (oper == GT_GT) ? GT_NE : GT_EQ;
                GenTree* icon32 = gtNewIconNode(32, TYP_INT);
                icon32->SetMorphed(this);

                GenTreeOp* shiftNode = gtNewOperNode(GT_RSZ, TYP_LONG, op1, icon32);
                shiftNode->SetMorphed(this);

                cmp->gtOp1 = shiftNode;
            }
        }
    }

    if (!cmp->OperIs(oper))
    {
        // Keep the old ValueNumber for 'tree' as the new expr
        // will still compute the same value as before.
        cmp->SetOper(oper, GenTree::PRESERVE_VN);
        op2->SetIntegralValue(0);
        fgUpdateConstTreeValueNumber(op2);
    }

    return cmp;
}

#ifdef FEATURE_HW_INTRINSICS
//------------------------------------------------------------------------
// fgOptimizeHWIntrinsic: optimize a HW intrinsic node
//
// Arguments:
//    node - HWIntrinsic node to examine
//
// Returns:
//    The original node if no optimization happened or if tree bashing occurred.
//    An alternative tree if an optimization happened.
//
// Notes:
//    Checks for HWIntrinsic nodes: Vector64.Create/Vector128.Create/Vector256.Create,
//    and if the call is one of these, attempt to optimize.
//    This is post-order, meaning that it will not morph the children.
//
GenTree* Compiler::fgOptimizeHWIntrinsic(GenTreeHWIntrinsic* node)
{
    assert(opts.OptimizationEnabled());

    GenTree* optimizedTree = fgOptimizeHWIntrinsicAssociative(node);

    if (optimizedTree != nullptr)
    {
        if (optimizedTree != node)
        {
            assert(!fgIsCommaThrow(optimizedTree));
            optimizedTree->SetMorphed(this);
            return optimizedTree;
        }
        else if (!optimizedTree->OperIsHWIntrinsic())
        {
            optimizedTree->SetMorphed(this);
            return optimizedTree;
        }
    }

    NamedIntrinsic intrinsicId     = node->GetHWIntrinsicId();
    var_types      retType         = node->TypeGet();
    CorInfoType    simdBaseJitType = node->GetSimdBaseJitType();
    var_types      simdBaseType    = node->GetSimdBaseType();
    unsigned       simdSize        = node->GetSimdSize();

    switch (intrinsicId)
    {
#if defined(TARGET_ARM64)
        case NI_Vector64_Create:
#endif // TARGET_ARM64
        case NI_Vector128_Create:
        {
            // The managed `Dot` API returns a scalar. However, many common usages require
            // it to be then immediately broadcast back to a vector so that it can be used
            // in a subsequent operation. One of the most common is normalizing a vector
            // which is effectively `value / value.Length` where `Length` is
            // `Sqrt(Dot(value, value))`. Because of this, and because of how a lot of
            // hardware works, we treat `NI_Vector_Dot` as returning a SIMD type and then
            // also wrap it in `ToScalar` where required.
            //
            // In order to ensure that developers can still utilize this efficiently, we
            // then look for four common patterns:
            //  * Create(Dot(..., ...))
            //  * Create(Sqrt(Dot(..., ...)))
            //  * Create(ToScalar(Dot(..., ...)))
            //  * Create(ToScalar(Sqrt(Dot(..., ...))))
            //
            // When these exist, we'll avoid converting to a scalar and hence, avoid broadcasting
            // the value back into a vector. Instead we'll just keep everything as a vector.
            //
            // We only do this for Vector64/Vector128 today. We could expand this more in
            // the future but it would require additional hand handling for Vector256
            // (since a 256-bit result requires more work). We do some integer handling
            // when the value is trivially replicated to all elements without extra work.

            if (node->GetOperandCount() != 1)
            {
                break;
            }

            GenTree* op1      = node->Op(1);
            GenTree* sqrt     = nullptr;
            GenTree* toScalar = nullptr;

            if (op1->OperIs(GT_INTRINSIC))
            {
                if (!varTypeIsFloating(simdBaseType))
                {
                    break;
                }

                if (op1->AsIntrinsic()->gtIntrinsicName != NI_System_Math_Sqrt)
                {
                    break;
                }

                sqrt = op1;
                op1  = op1->gtGetOp1();
            }

            if (!op1->OperIs(GT_HWINTRINSIC))
            {
                break;
            }

            GenTreeHWIntrinsic* hwop1 = op1->AsHWIntrinsic();

#if defined(TARGET_ARM64)
            if ((hwop1->GetHWIntrinsicId() == NI_Vector64_ToScalar) ||
                (hwop1->GetHWIntrinsicId() == NI_Vector128_ToScalar))
#else
            if (hwop1->GetHWIntrinsicId() == NI_Vector128_ToScalar)
#endif
            {
                op1 = hwop1->Op(1);

                if (!op1->OperIs(GT_HWINTRINSIC))
                {
                    break;
                }

                toScalar = hwop1;
                hwop1    = op1->AsHWIntrinsic();
            }

#if defined(TARGET_ARM64)
            if ((hwop1->GetHWIntrinsicId() != NI_Vector64_Dot) && (hwop1->GetHWIntrinsicId() != NI_Vector128_Dot))
#else
            if (hwop1->GetHWIntrinsicId() != NI_Vector128_Dot)
#endif
            {
                break;
            }

            // Must be working with the same types of vectors.
            if (hwop1->TypeGet() != retType)
            {
                break;
            }

            if (toScalar != nullptr)
            {
                DEBUG_DESTROY_NODE(toScalar);
            }

            if (sqrt != nullptr)
            {
                var_types simdType = getSIMDTypeForSize(simdSize);

                node = gtNewSimdSqrtNode(simdType, hwop1, simdBaseJitType, simdSize)->AsHWIntrinsic();
                DEBUG_DESTROY_NODE(sqrt);
            }
            else
            {
                node = hwop1;
            }
            node->SetMorphed(this);
            return node;
        }

        default:
        {
            break;
        }
    }

    bool       isScalar = false;
    genTreeOps oper     = node->GetOperForHWIntrinsicId(&isScalar, /* getEffectiveOp */ true);

    auto ExtractEffectiveOp = [&](genTreeOps oper, GenTreeHWIntrinsic* node, bool destroyNodes) -> GenTree* {
        GenTree* result = nullptr;

        switch (oper)
        {
            case GT_NEG:
            {
                if (node->GetOperandCount() == 2)
                {
                    if (varTypeIsFloating(node->GetSimdBaseType()))
                    {
                        // v1 ^ -0.0
                        result = node->Op(1);

                        if (destroyNodes)
                        {
                            DEBUG_DESTROY_NODE(node->Op(2));
                        }
                    }
                    else
                    {
                        // 0 - v2
                        result = node->Op(2);

                        if (destroyNodes)
                        {
                            DEBUG_DESTROY_NODE(node->Op(1));
                        }
                    }
                }
                else
                {
                    // -v1
                    result = node->Op(1);
                }
                break;
            }

            case GT_NOT:
            {
                if (node->GetOperandCount() == 2)
                {
                    // v1 ^ AllBitsSet
                    result = node->Op(1);

                    if (destroyNodes)
                    {
                        DEBUG_DESTROY_NODE(node->Op(2));
                    }
                }
                else
                {
                    // ~v1
                    result = node->Op(1);
                }
                break;
            }

            default:
            {
                unreached();
            }
        }

        if (destroyNodes)
        {
            DEBUG_DESTROY_NODE(node);
        }
        return result;
    };

    if (isScalar)
    {
        // scalar operations zero or copy the upper bits
        return node;
    }

    switch (oper)
    {
        // Transforms:
        // 1. (-v1) + v2 to v2 - v1; except for integrals when v2 is a constant
        // 2. (~v1) + 1 to -v1; for integral
        // 3. v1 + (-v2) to v1 - v2
        case GT_ADD:
        {
            GenTree* op1 = node->Op(1);
            GenTree* op2 = node->Op(2);

            if (op1->OperIsHWIntrinsic())
            {
                GenTreeHWIntrinsic* op1Intrin = op1->AsHWIntrinsic();

                bool       op1IsScalar = false;
                genTreeOps op1Oper     = op1Intrin->GetOperForHWIntrinsicId(&op1IsScalar, /* getEffectiveOp */ true);
                var_types  op1SimdBaseType = op1Intrin->GetSimdBaseType();

                if (op1IsScalar)
                {
                    // scalar operations zero or copy the upper bits
                    break;
                }

                if (op1Oper == GT_NEG)
                {
                    if (varTypeIsIntegral(simdBaseType) && op2->IsCnsVec())
                    {
                        // We don't handle this case because we are
                        // treating (-v1) + cns1 as the canonical form
                        break;
                    }

                    if (varTypeToSigned(simdBaseType) != varTypeToSigned(op1SimdBaseType))
                    {
                        // We need the base types to be of the same kind and size
                        // that is, we can't mix floating-point and integers or int and long
                        // but we can mix int and uint or long and ulong.
                        break;
                    }

                    op1 = ExtractEffectiveOp(GT_NEG, op1Intrin, /* destroyNodes */ true);

                    NamedIntrinsic subIntrinsic =
                        GenTreeHWIntrinsic::GetHWIntrinsicIdForBinOp(this, GT_SUB, op2, op1, simdBaseType, simdSize,
                                                                     isScalar);

                    node->ChangeHWIntrinsicId(subIntrinsic, op2, op1);
                    return fgMorphHWIntrinsicRequired(node);
                }
                else if (op1Oper == GT_NOT)
                {
                    if (varTypeIsIntegral(simdBaseType))
                    {
                        break;
                    }

                    if (!op2->IsCnsVec())
                    {
                        break;
                    }

                    if (!op2->AsVecCon()->IsBroadcast(simdBaseType))
                    {
                        break;
                    }

                    if (!op2->AsVecCon()->IsScalarOne(simdBaseType))
                    {
                        break;
                    }

                    op1 = ExtractEffectiveOp(GT_NOT, op1Intrin, /* destroyNodes */ true);

                    DEBUG_DESTROY_NODE(op2);
                    DEBUG_DESTROY_NODE(node);

                    node = gtNewSimdUnOpNode(GT_NEG, retType, op1, simdBaseJitType, simdSize)->AsHWIntrinsic();

#if defined(TARGET_XARCH)
                    if (varTypeIsFloating(simdBaseType))
                    {
                        node->AsHWIntrinsic()->Op(2)->SetMorphed(this);
                    }
                    else
                    {
                        node->AsHWIntrinsic()->Op(1)->SetMorphed(this);
                    }
#endif // TARGET_XARCH

                    return fgMorphHWIntrinsicRequired(node);
                }
            }
            else if (op2->OperIsHWIntrinsic())
            {
                GenTreeHWIntrinsic* op2Intrin = op2->AsHWIntrinsic();

                bool       op2IsScalar = false;
                genTreeOps op2Oper     = op2Intrin->GetOperForHWIntrinsicId(&op2IsScalar, /* getEffectiveOp */ true);
                var_types  op2SimdBaseType = op2Intrin->GetSimdBaseType();

                if ((op2Oper != GT_NEG) || op2IsScalar)
                {
                    // scalar operations zero or copy the upper bits
                    break;
                }

                if (varTypeToSigned(simdBaseType) != varTypeToSigned(op2SimdBaseType))
                {
                    // We need the base types to be of the same kind and size
                    // that is, we can't mix floating-point and integers or int and long
                    // but we can mix int and uint or long and ulong.
                    break;
                }

                op2 = ExtractEffectiveOp(GT_NEG, op2Intrin, /* destroyNodes */ true);

                NamedIntrinsic subIntrinsic =
                    GenTreeHWIntrinsic::GetHWIntrinsicIdForBinOp(this, GT_SUB, op1, op2, simdBaseType, simdSize,
                                                                 isScalar);

                node->ChangeHWIntrinsicId(subIntrinsic, op1, op2);
                return fgMorphHWIntrinsicRequired(node);
            }
            break;
        }

        // Transforms:
        // 1. (-v1) / cns2 to v1 / (-cns2); for floating-point
        case GT_DIV:
        {
            if (varTypeIsIntegral(simdBaseType))
            {
                break;
            }

            GenTree* op1 = node->Op(1);
            GenTree* op2 = node->Op(2);

            if (!op1->OperIsHWIntrinsic())
            {
                break;
            }

            GenTreeHWIntrinsic* op1Intrin = op1->AsHWIntrinsic();

            bool       op1IsScalar     = false;
            genTreeOps op1Oper         = op1Intrin->GetOperForHWIntrinsicId(&op1IsScalar, /* getEffectiveOp */ true);
            var_types  op1SimdBaseType = op1Intrin->GetSimdBaseType();

            if ((op1Oper != GT_NEG) || op1IsScalar)
            {
                // scalar operations zero or copy the upper bits
                break;
            }

            if (varTypeToSigned(simdBaseType) != varTypeToSigned(op1SimdBaseType))
            {
                // We need the base types to be of the same kind and size
                // that is, we can't mix floating-point and integers or int and long
                // but we can mix int and uint or long and ulong.
                break;
            }

            if (!op2->IsCnsVec())
            {
                break;
            }

            op2->AsVecCon()->EvaluateUnaryInPlace(GT_NEG, isScalar, simdBaseType);
            fgUpdateConstTreeValueNumber(op2);

            op1         = ExtractEffectiveOp(GT_NEG, op1Intrin, /* destroyNodes */ true);
            node->Op(1) = op1;

            return node;
        }

        // Transforms:
        // 1. (-v1) * cns2 to v1 * (-cns2)
        case GT_MUL:
        {
            GenTree* op1 = node->Op(1);
            GenTree* op2 = node->Op(2);

            if (!op1->OperIsHWIntrinsic())
            {
                break;
            }

            GenTreeHWIntrinsic* op1Intrin = op1->AsHWIntrinsic();

            bool       op1IsScalar     = false;
            genTreeOps op1Oper         = op1Intrin->GetOperForHWIntrinsicId(&op1IsScalar, /* getEffectiveOp */ true);
            var_types  op1SimdBaseType = op1Intrin->GetSimdBaseType();

            if ((op1Oper != GT_NEG) || op1IsScalar)
            {
                // scalar operations zero or copy the upper bits
                break;
            }

            if (varTypeToSigned(simdBaseType) != varTypeToSigned(op1SimdBaseType))
            {
                // We need the base types to be of the same kind and size
                // that is, we can't mix floating-point and integers or int and long
                // but we can mix int and uint or long and ulong.
                break;
            }

            if (!op2->IsCnsVec())
            {
                break;
            }

            op2->AsVecCon()->EvaluateUnaryInPlace(GT_NEG, isScalar, simdBaseType);
            fgUpdateConstTreeValueNumber(op2);

            op1         = ExtractEffectiveOp(GT_NEG, op1Intrin, /* destroyNodes */ true);
            node->Op(1) = op1;

            return node;
        }

        // Transforms:
        // 1. -(-v1) to v1
        // 2. -(v1 * cns2) to v1 * (-cns2)
        // 3. -(v1 / cns2) to v1 / (-cns2); except when any element of cns2 is 1 or -1 for integral
        case GT_NEG:
        {
            GenTree* op1 = ExtractEffectiveOp(GT_NEG, node, /* destroyNodes */ false);

            if (!op1->OperIsHWIntrinsic())
            {
                break;
            }

            GenTreeHWIntrinsic* op1Intrin = op1->AsHWIntrinsic();

            bool       op1IsScalar     = false;
            genTreeOps op1Oper         = op1Intrin->GetOperForHWIntrinsicId(&op1IsScalar, /* getEffectiveOp */ true);
            var_types  op1SimdBaseType = op1Intrin->GetSimdBaseType();

            if (op1IsScalar)
            {
                // scalar operations zero or copy the upper bits
                break;
            }

            if (varTypeToSigned(simdBaseType) != varTypeToSigned(op1SimdBaseType))
            {
                // We need the base types to be of the same kind and size
                // that is, we can't mix floating-point and integers or int and long
                // but we can mix int and uint or long and ulong.
                break;
            }

            if (op1Oper == GT_NEG)
            {
                GenTree* result = ExtractEffectiveOp(GT_NEG, op1Intrin, /* destroyNodes */ true);
                ExtractEffectiveOp(GT_NEG, node, /* destroyNodes */ true);
                return result;
            }
            else if ((op1Oper == GT_MUL) || (op1Oper == GT_DIV))
            {
                GenTree* op2 = op1Intrin->Op(2);

                if (!op2->IsCnsVec())
                {
                    break;
                }

                if ((op1Oper == GT_DIV) && varTypeIsIntegral(simdBaseType))
                {
                    bool     canTransform = true;
                    unsigned elementCount = GenTreeVecCon::ElementCount(simdSize, simdBaseType);

                    for (unsigned i = 0; i < elementCount; i++)
                    {
                        int64_t element = op2->AsVecCon()->GetElementIntegral(simdBaseType, i);

                        if ((element == 1) || (element == -1))
                        {
                            canTransform = false;
                            break;
                        }
                    }

                    if (!canTransform)
                    {
                        break;
                    }
                }

                op2->AsVecCon()->EvaluateUnaryInPlace(GT_NEG, isScalar, simdBaseType);
                fgUpdateConstTreeValueNumber(op2);

                ExtractEffectiveOp(GT_NEG, node, /* destroyNodes */ true);
                return op1Intrin;
            }
            break;
        }

        // Transforms:
        // 1. ~(~v1) to v1
        // 2. ~(v1 cmp v2) to v1 cmp* v2
        case GT_NOT:
        {
            GenTree* op1 = ExtractEffectiveOp(GT_NOT, node, /* destroyNodes */ false);

            if (!op1->OperIsHWIntrinsic())
            {
                break;
            }

            GenTreeHWIntrinsic* op1Intrin = op1->AsHWIntrinsic();
            GenTreeHWIntrinsic* cvtIntrin = nullptr;

            if (op1Intrin->OperIsConvertMaskToVector())
            {
                // If we have a ConvertMaskToVector, then we
                // need to get its op1 to check for a compare

                cvtIntrin = op1Intrin;
                op1       = op1Intrin->Op(1);

                if (!op1->OperIsHWIntrinsic())
                {
                    break;
                }

                op1Intrin = op1->AsHWIntrinsic();
            }

            bool       op1IsScalar = false;
            genTreeOps op1Oper     = op1Intrin->GetOperForHWIntrinsicId(&op1IsScalar, /* getEffectiveOp */ true);
            var_types  op1RetType  = op1Intrin->TypeGet();

            NamedIntrinsic op1Intrinsic       = op1Intrin->GetHWIntrinsicId();
            CorInfoType    op1SimdBaseJitType = op1Intrin->GetSimdBaseJitType();
            var_types      op1SimdBaseType    = op1Intrin->GetSimdBaseType();
            unsigned       op1SimdSize        = op1Intrin->GetSimdSize();

            if (op1Oper == GT_NOT)
            {
                // The simdBaseTypes can differ for GT_NOT since its a bitwise operation
                GenTree* result = ExtractEffectiveOp(GT_NOT, op1Intrin, /* destroyNodes */ true);
                ExtractEffectiveOp(GT_NOT, node, /* destroyNodes */ true);
                return result;
            }

            if (GenTree::OperIsCompare(op1Oper))
            {
                assert(op1Intrin->GetOperandCount() == 2);

                GenTree* cmpOp1 = op1Intrin->Op(1);
                GenTree* cmpOp2 = op1Intrin->Op(2);

                const bool reverseCond = true;

                var_types lookupType =
                    op1IsScalar ? op1RetType
                                : GenTreeHWIntrinsic::GetLookupTypeForCmpOp(this, op1Oper, op1RetType, op1SimdBaseType,
                                                                            op1SimdSize, reverseCond);
                NamedIntrinsic newId =
                    GenTreeHWIntrinsic::GetHWIntrinsicIdForCmpOp(this, op1Oper, lookupType, cmpOp1, cmpOp2,
                                                                 op1SimdBaseType, op1SimdSize, op1IsScalar,
                                                                 reverseCond);

                if (newId != NI_Illegal)
                {
                    op1Intrin->ResetHWIntrinsicId(newId, cmpOp1, cmpOp2);
                    ExtractEffectiveOp(GT_NOT, node, /* destroyNodes */ true);

                    if (lookupType != op1RetType)
                    {
                        assert(cvtIntrin == nullptr);
                        assert(varTypeIsSIMD(op1RetType));
                        assert(varTypeIsMask(lookupType));

                        op1Intrin->gtType = lookupType;
                        op1Intrin = gtNewSimdCvtMaskToVectorNode(retType, op1Intrin, op1SimdBaseJitType, op1SimdSize)
                                        ->AsHWIntrinsic();
                    }
                    else if (cvtIntrin != nullptr)
                    {
                        cvtIntrin->Op(1) = op1Intrin;
                        op1Intrin        = cvtIntrin;
                    }
                    return fgMorphHWIntrinsicRequired(op1Intrin);
                }
            }
#if defined(TARGET_XARCH)
            else
            {
                // We have some other comparison intrinsics that don't map to the simple forms

                switch (op1Intrinsic)
                {
                    case NI_AVX_Compare:
                    case NI_AVX_CompareScalar:
                    case NI_AVX512_CompareMask:
                    {
                        assert(op1Intrin->GetOperandCount() == 3);

                        GenTree* cmpOp3 = op1Intrin->Op(3);

                        if (!cmpOp3->IsCnsIntOrI())
                        {
                            break;
                        }

                        FloatComparisonMode mode =
                            static_cast<FloatComparisonMode>(cmpOp3->AsIntConCommon()->IntegralValue());

                        FloatComparisonMode newMode = mode;

                        switch (mode)
                        {
                            case FloatComparisonMode::UnorderedEqualNonSignaling:
                            case FloatComparisonMode::UnorderedEqualSignaling:
                            {
                                newMode = FloatComparisonMode::OrderedNotEqualNonSignaling;
                                break;
                            }

                            case FloatComparisonMode::OrderedFalseNonSignaling:
                            case FloatComparisonMode::OrderedFalseSignaling:
                            {
                                newMode = FloatComparisonMode::UnorderedTrueNonSignaling;
                                break;
                            }

                            case FloatComparisonMode::OrderedNotEqualNonSignaling:
                            case FloatComparisonMode::OrderedNotEqualSignaling:
                            {
                                newMode = FloatComparisonMode::UnorderedEqualNonSignaling;
                                break;
                            }

                            case FloatComparisonMode::UnorderedTrueNonSignaling:
                            case FloatComparisonMode::UnorderedTrueSignaling:
                            {
                                newMode = FloatComparisonMode::OrderedFalseNonSignaling;
                                break;
                            }

                            default:
                            {
                                // Other modes should either have been normalized or
                                // will be out of range values and can't be handled
                                break;
                            }
                        }

                        if (newMode != mode)
                        {
                            ExtractEffectiveOp(GT_NOT, node, /* destroyNodes */ true);
                            cmpOp3->AsIntConCommon()->SetIntegralValue(static_cast<uint8_t>(mode));
                            return fgMorphHWIntrinsicRequired(op1Intrin);
                        }
                        break;
                    }

                    default:
                    {
                        break;
                    }
                }
            }
#endif // TARGET_XARCH
            break;
        }

        // Transforms:
        // 1. (-v1) - (-v2) to v2 - v1
        // 2. v1 - (-v2) to v1 + v2
        case GT_SUB:
        {
            if (!fgGlobalMorph)
            {
                break;
            }

            GenTree* op1 = node->Op(1);
            GenTree* op2 = node->Op(2);

            if (!op2->OperIsHWIntrinsic())
            {
                break;
            }

            GenTreeHWIntrinsic* op2Intrin = op2->AsHWIntrinsic();

            bool       op2IsScalar     = false;
            genTreeOps op2Oper         = op2Intrin->GetOperForHWIntrinsicId(&op2IsScalar, /* getEffectiveOp */ true);
            var_types  op2SimdBaseType = op2Intrin->GetSimdBaseType();

            if ((op2Oper != GT_NEG) || op2IsScalar)
            {
                break;
            }

            if (varTypeToSigned(simdBaseType) != varTypeToSigned(op2SimdBaseType))
            {
                // We need the base types to be of the same kind and size
                // that is, we can't mix floating-point and integers or int and long
                // but we can mix int and uint or long and ulong.
                break;
            }

            op2 = ExtractEffectiveOp(GT_NEG, op2Intrin, /* destroyNodes */ true);

            if (op1->OperIsHWIntrinsic())
            {
                GenTreeHWIntrinsic* op1Intrin = op1->AsHWIntrinsic();

                bool       op1IsScalar = false;
                genTreeOps op1Oper     = op1Intrin->GetOperForHWIntrinsicId(&op1IsScalar, /* getEffectiveOp */ true);
                var_types  op1SimdBaseType = op1Intrin->GetSimdBaseType();

                if ((op1Oper == GT_NEG) && !op1IsScalar &&
                    (varTypeToSigned(simdBaseType) == varTypeToSigned(op1SimdBaseType)))
                {
                    op1 = ExtractEffectiveOp(GT_NEG, op1Intrin, /* destroyNodes */ true);

                    node->Op(1) = op2;
                    node->Op(2) = op1;

                    break;
                }
            }

            NamedIntrinsic addIntrinsic =
                GenTreeHWIntrinsic::GetHWIntrinsicIdForBinOp(this, GT_ADD, op1, op2, simdBaseType, simdSize, isScalar);

            node->ChangeHWIntrinsicId(addIntrinsic, op1, op2);
            return fgMorphHWIntrinsicRequired(node);
        }

        default:
        {
            break;
        }
    }

    return node;
}

//------------------------------------------------------------------------
// fgOptimizeHWIntrinsicAssociative: Morph an associative GenTreeHWIntrinsic tree.
//
// Arguments:
//    tree - The tree to morph
//
// Return Value:
//    The fully morphed tree.
//
GenTree* Compiler::fgOptimizeHWIntrinsicAssociative(GenTreeHWIntrinsic* tree)
{
    // In general this tries to simplify `(v1 op c1) op c2` into `v1 op (c1 op c2)`
    // so that we can fold it down to `v1 op c3`
    assert(opts.OptimizationEnabled());

    NamedIntrinsic intrinsicId     = tree->GetHWIntrinsicId();
    var_types      retType         = tree->TypeGet();
    CorInfoType    simdBaseJitType = tree->GetSimdBaseJitType();
    var_types      simdBaseType    = tree->GetSimdBaseType();
    unsigned       simdSize        = tree->GetSimdSize();

    if (!varTypeIsSIMD(retType) && !varTypeIsMask(retType))
    {
        return nullptr;
    }

    bool       isScalar              = false;
    genTreeOps oper                  = tree->GetOperForHWIntrinsicId(&isScalar);
    bool       needsMatchingBaseType = false;

    switch (oper)
    {
        case GT_ADD:
        case GT_MUL:
        {
            if (varTypeIsIntegral(simdBaseType))
            {
                needsMatchingBaseType = true;
                break;
            }
            return nullptr;
        }

        case GT_AND:
        case GT_OR:
        case GT_XOR:
        {
            break;
        }

        default:
        {
            return nullptr;
        }
    }

    // op1 can be GT_COMMA, in which case we're going to fold
    // `(..., (v1 op c1)) op c2` to `(..., (v1 op c3))`

    GenTree* op1          = tree->Op(1);
    GenTree* effectiveOp1 = op1->gtEffectiveVal();

    if (!effectiveOp1->OperIsHWIntrinsic())
    {
        return nullptr;
    }

    GenTreeHWIntrinsic* intrinOp1 = effectiveOp1->AsHWIntrinsic();

    bool       op1IsScalar = false;
    genTreeOps op1Oper     = intrinOp1->GetOperForHWIntrinsicId(&op1IsScalar);

    if ((op1Oper != oper) || (op1IsScalar != isScalar))
    {
        return nullptr;
    }

    if (needsMatchingBaseType && (intrinOp1->GetSimdBaseType() != simdBaseType))
    {
        return nullptr;
    }

    if (!intrinOp1->Op(2)->OperIsConst() || !tree->Op(2)->OperIsConst())
    {
        return nullptr;
    }

    if (!fgGlobalMorph && (effectiveOp1 != op1))
    {
        // Since 'tree->Op(1)' can have complex structure; e.g. `(.., (.., op1))`
        // don't run the optimization for such trees outside of global morph.
        // Otherwise, there is a chance of violating VNs invariants.
        return nullptr;
    }

    GenTree* cns1 = intrinOp1->Op(2);
    GenTree* cns2 = tree->Op(2);

    assert(cns1->TypeIs(retType));
    assert(cns2->TypeIs(retType));

    GenTree* res = gtNewSimdHWIntrinsicNode(retType, cns1, cns2, intrinsicId, simdBaseJitType, simdSize);
    res          = gtFoldExprHWIntrinsic(res->AsHWIntrinsic());

    assert(res == cns1);
    assert(res->OperIsConst());
    assert(res->TypeIs(retType));

    if (effectiveOp1 != op1)
    {
        // We had a comma, so pull the VNs from node
        op1->SetVNsFromNode(tree);

        DEBUG_DESTROY_NODE(cns2);
        DEBUG_DESTROY_NODE(tree);

        return op1;
    }
    else
    {
        // We had a simple tree, so pull the value and new constant up

        tree->Op(1) = intrinOp1->Op(1);
        tree->Op(2) = intrinOp1->Op(2);

        DEBUG_DESTROY_NODE(cns2);
        DEBUG_DESTROY_NODE(intrinOp1);

        assert(tree->Op(2) == cns1);
        return tree;
    }
}
#endif // FEATURE_HW_INTRINSICS

//------------------------------------------------------------------------
// fgOptimizeCommutativeArithmetic: Optimizes commutative operations.
//
// Arguments:
//   tree - the unchecked GT_ADD/GT_MUL/GT_OR/GT_XOR/GT_AND tree to optimize.
//
// Return Value:
//   The optimized tree that can have any shape.
//
GenTree* Compiler::fgOptimizeCommutativeArithmetic(GenTreeOp* tree)
{
    assert(tree->OperIs(GT_ADD, GT_MUL, GT_OR, GT_XOR, GT_AND));
    assert(!tree->gtOverflowEx());

    // Commute constants to the right.
    if (tree->gtGetOp1()->OperIsConst() && !tree->gtGetOp1()->TypeIs(TYP_REF))
    {
        // TODO-Review: We used to assert here that "(!op2->OperIsConst() || !opts.OptEnabled(CLFLG_CONSTANTFOLD))".
        // This may indicate a missed "remorph". Task is to re-enable this assertion and investigate.
        std::swap(tree->gtOp1, tree->gtOp2);
    }

    if (fgOperIsBitwiseRotationRoot(tree->OperGet()))
    {
        GenTree* rotationTree = fgRecognizeAndMorphBitwiseRotation(tree);
        if (rotationTree != nullptr)
        {
            return rotationTree;
        }
    }

    if (varTypeIsIntegralOrI(tree))
    {
        genTreeOps oldTreeOper   = tree->OperGet();
        GenTreeOp* optimizedTree = fgMorphCommutative(tree->AsOp());
        if (optimizedTree != nullptr)
        {
            if (!optimizedTree->OperIs(oldTreeOper))
            {
                // "optimizedTree" could end up being a COMMA.
                return optimizedTree;
            }

            tree = optimizedTree;
        }
    }

    GenTree* optimizedTree = nullptr;
    if (tree->OperIs(GT_ADD))
    {
        optimizedTree = fgOptimizeAddition(tree);
    }
    else if (tree->OperIs(GT_MUL))
    {
        optimizedTree = fgOptimizeMultiply(tree);
    }
    else if (tree->OperIs(GT_AND))
    {
        optimizedTree = fgOptimizeBitwiseAnd(tree);
    }
    else if (tree->OperIs(GT_XOR))
    {
        optimizedTree = fgOptimizeBitwiseXor(tree);
    }

    if (optimizedTree != nullptr)
    {
        return optimizedTree;
    }

    return tree;
}

//------------------------------------------------------------------------
// fgOptimizeAddition: optimizes addition.
//
// Arguments:
//   add - the unchecked GT_ADD tree to optimize.
//
// Return Value:
//   The optimized tree, that can have any shape, in case any transformations
//   were performed. Otherwise, "nullptr", guaranteeing no state change.
//
GenTree* Compiler::fgOptimizeAddition(GenTreeOp* add)
{
    assert(add->OperIs(GT_ADD) && !add->gtOverflow());

    GenTree* op1 = add->gtGetOp1();
    GenTree* op2 = add->gtGetOp2();

    // Fold "((x + icon1) + (y + icon2))" to ((x + y) + (icon1 + icon2))".
    // Be careful not to create a byref pointer that may point outside of the ref object.
    // Only do this in global morph as we don't recompute the VN for "(x + y)", the new "op2".
    if (op1->OperIs(GT_ADD) && op2->OperIs(GT_ADD) && !op1->gtOverflow() && !op2->gtOverflow() &&
        op1->AsOp()->gtGetOp2()->IsCnsIntOrI() && op2->AsOp()->gtGetOp2()->IsCnsIntOrI() &&
        !varTypeIsGC(op1->AsOp()->gtGetOp1()) && !varTypeIsGC(op2->AsOp()->gtGetOp1()) && fgGlobalMorph)
    {
        GenTreeOp*     addOne   = op1->AsOp();
        GenTreeOp*     addTwo   = op2->AsOp();
        GenTreeIntCon* constOne = addOne->gtGetOp2()->AsIntCon();

        // addOne is now "x + y"
        addOne->gtOp2 = addTwo->gtGetOp1();
        addOne->SetAllEffectsFlags(addOne->gtGetOp1(), addOne->gtGetOp2());

        // addTwo is now "icon1 + icon2" so we can fold it using gtFoldExprConst
        addTwo->gtOp1 = constOne;
        add->gtOp2    = gtFoldExprConst(add->gtOp2);
        op2           = add->gtGetOp2();
    }

    // Fold (x + 0) - given it won't change the tree type.
    if (op2->IsIntegralConst(0) && (genActualType(add) == genActualType(op1)))
    {
        // Keep the offset nodes with annotations for value numbering purposes.
        if (!op2->IsCnsIntOrI() || (op2->AsIntCon()->gtFieldSeq == nullptr))
        {
            DEBUG_DESTROY_NODE(op2);
            DEBUG_DESTROY_NODE(add);

            return op1;
        }

        // Communicate to CSE that this addition is a no-op.
        add->SetDoNotCSE();
    }

    if (opts.OptimizationEnabled())
    {
        // Reduce local addresses: "ADD(LCL_ADDR, OFFSET)" => "LCL_FLD_ADDR".
        //
        if (op1->OperIs(GT_LCL_ADDR) && op2->IsCnsIntOrI())
        {
            GenTreeLclVarCommon* lclAddrNode = op1->AsLclVarCommon();
            GenTreeIntCon*       offsetNode  = op2->AsIntCon();
            if (FitsIn<uint16_t>(offsetNode->IconValue()))
            {
                unsigned offset = lclAddrNode->GetLclOffs() + static_cast<uint16_t>(offsetNode->IconValue());

                // Note: the emitter does not expect out-of-bounds access for LCL_FLD_ADDR.
                if (FitsIn<uint16_t>(offset) && (offset < lvaLclExactSize(lclAddrNode->GetLclNum())))
                {
                    lclAddrNode->SetOper(GT_LCL_ADDR);
                    lclAddrNode->AsLclFld()->SetLclOffs(offset);
                    assert(lvaGetDesc(lclAddrNode)->lvDoNotEnregister);

                    lclAddrNode->SetVNsFromNode(add);

                    DEBUG_DESTROY_NODE(offsetNode);
                    DEBUG_DESTROY_NODE(add);

                    return lclAddrNode;
                }
            }
        }

        // - a + b = > b - a
        // ADD(NEG(a), b) => SUB(b, a)

        // Do not do this if "op2" is constant for canonicalization purposes.
        if (op1->OperIs(GT_NEG) && !op2->OperIs(GT_NEG) && !op2->IsIntegralConst() && gtCanSwapOrder(op1, op2))
        {
            add->SetOper(GT_SUB);
            add->gtOp1 = op2;
            add->gtOp2 = op1->AsOp()->gtGetOp1();

            DEBUG_DESTROY_NODE(op1);

            return add;
        }

        // a + -b = > a - b
        // ADD(a, NEG(b)) => SUB(a, b)
        if (!op1->OperIs(GT_NEG) && op2->OperIs(GT_NEG))
        {
            add->SetOper(GT_SUB);
            add->gtOp2 = op2->AsOp()->gtGetOp1();

            DEBUG_DESTROY_NODE(op2);

            return add;
        }

        // Fold (~x + 1) to -x.
        if (op1->OperIs(GT_NOT) && op2->IsIntegralConst(1))
        {
            op1->SetOper(GT_NEG);
            op1->SetVNsFromNode(add);
            DEBUG_DESTROY_NODE(op2);
            DEBUG_DESTROY_NODE(add);
            return op1;
        }
    }

    return nullptr;
}

//------------------------------------------------------------------------
// fgOptimizeMultiply: optimizes multiplication.
//
// Arguments:
//   mul - the unchecked TYP_I_IMPL/TYP_INT GT_MUL tree to optimize.
//
// Return Value:
//   The optimized tree, that can have any shape, in case any transformations
//   were performed. Otherwise, "nullptr", guaranteeing no state change.
//
GenTree* Compiler::fgOptimizeMultiply(GenTreeOp* mul)
{
    assert(mul->OperIs(GT_MUL));
    assert(varTypeIsIntOrI(mul) || varTypeIsFloating(mul));
    assert(!mul->gtOverflow());

    GenTree* op1 = mul->gtGetOp1();
    GenTree* op2 = mul->gtGetOp2();

    assert(mul->TypeGet() == genActualType(op1));
    assert(mul->TypeGet() == genActualType(op2));

    if (opts.OptimizationEnabled() && op2->IsCnsFltOrDbl())
    {
        double multiplierValue = op2->AsDblCon()->DconValue();

        if (multiplierValue == 1.0)
        {
            // Fold "x * 1.0" to "x".
            DEBUG_DESTROY_NODE(op2);
            DEBUG_DESTROY_NODE(mul);

            return op1;
        }

        // Fold "x * 2.0" to "x + x".
        // If op1 is not a local we will have to introduce a temporary via GT_COMMA.
        // Unfortunately, it's not optHoistLoopCode-friendly (yet), so we'll only do
        // this for locals / after hoisting has run (when rationalization remorphs
        // math INTRINSICSs into calls...).
        if ((multiplierValue == 2.0) && (op1->IsLocal() || (fgOrder == FGOrderLinear)))
        {
            op2          = fgMakeMultiUse(&op1);
            GenTree* add = gtNewOperNode(GT_ADD, mul->TypeGet(), op1, op2);
            add->SetMorphed(this, /* doChildren */ true);
            return add;
        }
    }

    if (op1->OperIs(GT_NEG) && opts.OptimizationEnabled())
    {
        if ((op2->IsCnsIntOrI() && !op2->IsIconHandle()) || op2->IsCnsFltOrDbl())
        {
            // MUL(NEG(a), C) => MUL(a, NEG(C))
            mul->gtOp1 = op1->AsUnOp()->gtGetOp1();

            if (op2->IsCnsIntOrI())
            {
                op2->AsIntConCommon()->SetIconValue(-op2->AsIntConCommon()->IconValue());
                op2->AsIntConRef().gtFieldSeq = nullptr;
            }
            else
            {
                assert(op2->IsCnsFltOrDbl());
                op2->AsDblCon()->SetDconValue(-op2->AsDblCon()->DconValue());
            }
            fgUpdateConstTreeValueNumber(op2);

            DEBUG_DESTROY_NODE(op1);
            op1 = mul->gtOp1;
        }
    }

    if (op2->IsIntegralConst())
    {
        // We should not get here for 64-bit multiplications on 32-bit.
        assert(op2->IsCnsIntOrI());

        ssize_t mult = op2->AsIntConCommon()->IconValue();

        if (mult == 0)
        {
            // We may be able to throw away op1 (unless it has side-effects)

            if ((op1->gtFlags & GTF_SIDE_EFFECT) == 0)
            {
                DEBUG_DESTROY_NODE(op1);
                DEBUG_DESTROY_NODE(mul);

                return op2; // Just return the "0" node
            }

            // We need to keep op1 for the side-effects. Hang it off a GT_COMMA node.
            mul->ChangeOper(GT_COMMA, GenTree::PRESERVE_VN);
            return mul;
        }

#ifdef TARGET_XARCH
        // Should we try to replace integer multiplication with lea/add/shift sequences?
        bool mulShiftOpt = compCodeOpt() != SMALL_CODE;
#else  // !TARGET_XARCH
        bool mulShiftOpt = false;
#endif // !TARGET_XARCH

        size_t abs_mult      = (mult >= 0) ? mult : -mult;
        size_t lowestBit     = genFindLowestBit(abs_mult);
        bool   changeToShift = false;

        // is it a power of two? (positive or negative)
        if (abs_mult == lowestBit)
        {
            // if negative negate (min-int does not need negation)
            if (mult < 0 && mult != SSIZE_T_MIN)
            {
                op1        = gtNewOperNode(GT_NEG, genActualType(op1), op1);
                mul->gtOp1 = op1;
                fgMorphTreeDone(op1);
            }

            if (abs_mult == 1)
            {
                DEBUG_DESTROY_NODE(op2);
                DEBUG_DESTROY_NODE(mul);
                return op1;
            }

            // Change the multiplication into a shift by log2(val) bits.
            op2->AsIntConCommon()->SetIconValue(genLog2(abs_mult));
            changeToShift = true;
        }
        else if (mulShiftOpt && (lowestBit > 1) && jitIsScaleIndexMul(lowestBit))
        {
            int     shift  = genLog2(lowestBit);
            ssize_t factor = abs_mult >> shift;

            if (factor == 3 || factor == 5 || factor == 9)
            {
                // if negative negate (min-int does not need negation)
                if (mult < 0 && mult != SSIZE_T_MIN)
                {
                    op1        = gtNewOperNode(GT_NEG, genActualType(op1), op1);
                    mul->gtOp1 = op1;
                    fgMorphTreeDone(op1);
                }

                // change the multiplication into a smaller multiplication (by 3, 5 or 9) and a shift
                GenTree* const factorNode = gtNewIconNodeWithVN(this, factor, mul->TypeGet());
                factorNode->SetMorphed(this);
                op1        = gtNewOperNode(GT_MUL, mul->TypeGet(), op1, factorNode);
                mul->gtOp1 = op1;
                fgMorphTreeDone(op1);

                op2->AsIntConCommon()->SetIconValue(shift);
                changeToShift = true;
            }
        }

        if (changeToShift)
        {
            fgUpdateConstTreeValueNumber(op2);
            mul->ChangeOper(GT_LSH, GenTree::PRESERVE_VN);

            return mul;
        }
    }

    return nullptr;
}

//------------------------------------------------------------------------
// fgOptimizeBitwiseAnd: optimizes the "and" operation.
//
// Arguments:
//   andOp - the GT_AND tree to optimize.
//
// Return Value:
//   The optimized tree, currently always a relop, in case any transformations
//   were performed. Otherwise, "nullptr", guaranteeing no state change.
//
GenTree* Compiler::fgOptimizeBitwiseAnd(GenTreeOp* andOp)
{
    assert(andOp->OperIs(GT_AND));

    GenTree* op1 = andOp->gtGetOp1();
    GenTree* op2 = andOp->gtGetOp2();

    // Fold "cmp & 1" to just "cmp".
    if (andOp->TypeIs(TYP_INT) && op1->OperIsCompare() && op2->IsIntegralConst(1))
    {
        DEBUG_DESTROY_NODE(op2);
        DEBUG_DESTROY_NODE(andOp);

        return op1;
    }

    return nullptr;
}

//------------------------------------------------------------------------
// fgOptimizeRelationalComparisonWithCasts: Recognizes comparisons against
//   various cast operands and tries to remove them. E.g.:
//
//   *  GE        int
//   +--*  CAST      long <- ulong <- uint
//   |  \--*  X         int
//   \--*  CNS_INT   long
//
//   to:
//
//   *  GE_un     int
//   +--*  X         int
//   \--*  CNS_INT   int
//
//   same for:
//
//   *  GE        int
//   +--*  CAST      long <- ulong <- uint
//   |  \--*  X         int
//   \--*  CAST      long <- [u]long <- int
//      \--*  ARR_LEN   int
//
//   These patterns quite often show up along with index checks
//
// Arguments:
//   cmp - the GT_LE/GT_LT/GT_GE/GT_GT tree to morph.
//
// Return Value:
//   Returns the same tree where operands might have narrower types
//
// Notes:
//   TODO-Casts: consider unifying this function with "optNarrowTree"
//
GenTree* Compiler::fgOptimizeRelationalComparisonWithCasts(GenTreeOp* cmp)
{
    assert(cmp->OperIs(GT_LE, GT_LT, GT_GE, GT_GT));

    GenTree* op1 = cmp->gtGetOp1();
    GenTree* op2 = cmp->gtGetOp2();

    // Caller is expected to call this function only if we have at least one CAST node
    assert(op1->OperIs(GT_CAST) || op2->OperIs(GT_CAST));

    assert(genActualType(op1) == genActualType(op2));

    if (!op1->TypeIs(TYP_LONG))
    {
        return cmp;
    }

    auto supportedOp = [](GenTree* op) {
        if (op->IsIntegralConst())
        {
            return true;
        }

        if (op->OperIs(GT_CAST))
        {
            if (op->gtOverflow())
            {
                return false;
            }

            if (genActualType(op->CastFromType()) != TYP_INT)
            {
                return false;
            }

            assert(varTypeIsLong(op->CastToType()));
            return true;
        }

        return false;
    };

    if (!supportedOp(op1) || !supportedOp(op2))
    {
        return cmp;
    }

    auto isUpperZero = [this](GenTree* op) {
        if (op->IsIntegralConst())
        {
            int64_t lng = op->AsIntConCommon()->LngValue();
            return (lng >= 0) && (lng <= UINT_MAX);
        }

        assert(op->OperIs(GT_CAST));
        if (op->AsCast()->IsUnsigned())
        {
            return true;
        }

        return IntegralRange::ForNode(op->AsCast()->CastOp(), this).IsNonNegative();
    };

    // If both operands have zero as the upper half then any signed/unsigned
    // 64-bit comparison is equivalent to the same unsigned 32-bit comparison.
    if (isUpperZero(op1) && isUpperZero(op2))
    {
        JITDUMP("Removing redundant cast(s) for:\n")
        DISPTREE(cmp)
        JITDUMP("\n\nto:\n\n")

        cmp->SetUnsigned();

        auto transform = [this](GenTree** use) {
            if ((*use)->IsIntegralConst())
            {
                (*use)->BashToConst(static_cast<int>((*use)->AsIntConCommon()->LngValue()));
                fgUpdateConstTreeValueNumber(*use);
            }
            else
            {
                assert((*use)->OperIs(GT_CAST));
                GenTreeCast* cast = (*use)->AsCast();
                *use              = cast->CastOp();
                DEBUG_DESTROY_NODE(cast);
            }
        };

        transform(&cmp->gtOp1);
        transform(&cmp->gtOp2);

        assert((genActualType(cmp->gtOp1) == TYP_INT) && (genActualType(cmp->gtOp2) == TYP_INT));

        DISPTREE(cmp)
        JITDUMP("\n")
    }
    return cmp;
}

// fgOptimizeBitwiseXor: optimizes the "xor" operation.
//
// Arguments:
//   xorOp - the GT_XOR tree to optimize.
//
// Return Value:
//   The optimized tree, currently always a local variable, in case any transformations
//   were performed. Otherwise, "nullptr", guaranteeing no state change.
//
GenTree* Compiler::fgOptimizeBitwiseXor(GenTreeOp* xorOp)
{
    assert(xorOp->OperIs(GT_XOR));

    GenTree* op1 = xorOp->gtGetOp1();
    GenTree* op2 = xorOp->gtGetOp2();

    if (op2->IsIntegralConst(0))
    {
        /* "x ^ 0" is "x" */
        DEBUG_DESTROY_NODE(xorOp, op2);
        return op1;
    }
    else if (op2->IsIntegralConst(-1))
    {
        /* "x ^ -1" is "~x" */
        xorOp->ChangeOper(GT_NOT);
        xorOp->gtOp2 = nullptr;
        DEBUG_DESTROY_NODE(op2);

        return xorOp;
    }
    else if (op2->IsIntegralConst(1) && op1->OperIsCompare())
    {
        /* "binaryVal ^ 1" is "!binaryVal" */
        gtReverseCond(op1);
        DEBUG_DESTROY_NODE(op2);
        DEBUG_DESTROY_NODE(xorOp);

        return op1;
    }
    else if (varTypeIsFloating(xorOp) && op2->IsFloatNegativeZero())
    {
        // "x ^ -0.0" is "-x"

        xorOp->ChangeOper(GT_NEG);
        xorOp->gtOp2 = nullptr;

        DEBUG_DESTROY_NODE(op2);
        return xorOp;
    }

    return nullptr;
}

//------------------------------------------------------------------------
// fgPropagateCommaThrow: propagate a "comma throw" up the tree.
//
// "Comma throws" in the compiler represent the canonical form of an always
// throwing expression. They have the shape of COMMA(THROW, ZERO), to satisfy
// the semantic that the original expression produced some value and are
// generated by "gtFoldExprConst" when it encounters checked arithmetic that
// will determinably overflow.
//
// In the global morphing phase, "comma throws" are "propagated" up the tree,
// in post-order, to eliminate nodes that will never execute. This method,
// called by "fgMorphSmpOp", encapsulates this optimization.
//
// Arguments:
//   parent               - the node currently being processed.
//   commaThrow           - the comma throw in question, "parent"'s operand.
//   precedingSideEffects - side effects of nodes preceding "comma" in execution order.
//
// Return Value:
//   If "parent" is to be replaced with a comma throw, i. e. the propagation was successful,
//   the new "parent", otherwise "nullptr", guaranteeing no state change, with one exception:
//   the "fgRemoveRestOfBlock" "global" may be set. Note that the new returned tree does not
//   have to be a "comma throw", it can be "bare" throw call if the "parent" node did not
//   produce any value.
//
// Notes:
//   "Comma throws" are very rare.
//
GenTree* Compiler::fgPropagateCommaThrow(GenTree* parent, GenTreeOp* commaThrow, GenTreeFlags precedingSideEffects)
{
    // Comma throw propagation does not preserve VNs, and deletes nodes.
    assert(fgGlobalMorph);
    assert(fgIsCommaThrow(commaThrow));

    if ((commaThrow->gtFlags & GTF_COLON_COND) == 0)
    {
        fgRemoveRestOfBlock = true;
    }

    if ((precedingSideEffects & GTF_ALL_EFFECT) == 0)
    {
        if (parent->TypeIs(TYP_VOID))
        {
            // Return the throw node as the new tree.
            return commaThrow->gtGetOp1();
        }

        // Fix up the COMMA's type if needed.
        if (genActualType(parent) != genActualType(commaThrow))
        {
            commaThrow->gtGetOp2()->BashToZeroConst(genActualType(parent));
            commaThrow->ChangeType(genActualType(parent));
        }

        return commaThrow;
    }

    return nullptr;
}

//----------------------------------------------------------------------------------------------
// fgMorphRetInd: Try to get rid of extra local indirections in a return tree.
//
// Arguments:
//    node - The return node that uses a local field.
//
// Return Value:
//    the original return operand if there was no optimization, or an optimized new return operand.
//
GenTree* Compiler::fgMorphRetInd(GenTreeOp* ret)
{
    assert(ret->OperIs(GT_RETURN, GT_SWIFT_ERROR_RET));
    assert(ret->GetReturnValue()->OperIs(GT_LCL_FLD));
    GenTreeLclFld* lclFld = ret->GetReturnValue()->AsLclFld();
    unsigned       lclNum = lclFld->GetLclNum();

    if (fgGlobalMorph && varTypeIsStruct(lclFld) && !lvaIsImplicitByRefLocal(lclNum))
    {
        LclVarDsc* varDsc     = lvaGetDesc(lclNum);
        unsigned   indSize    = lclFld->GetSize();
        unsigned   lclVarSize = lvaLclExactSize(lclNum);

        // TODO: change conditions in `canFold` to `indSize <= lclVarSize`, but currently do not support `BITCAST
        // int<-SIMD16` etc. Note this will also require the offset of the field to be zero.
        assert(indSize <= lclVarSize);

#if defined(TARGET_64BIT)
        bool canFold = (indSize == lclVarSize);
#else // !TARGET_64BIT
      // TODO: improve 32 bit targets handling for LONG returns if necessary, nowadays we do not support `BITCAST
      // long<->double` there.
        bool canFold = (indSize == lclVarSize) && (lclVarSize <= REGSIZE_BYTES);
#endif

        if (canFold)
        {
            // Fold even if types do not match, lowering will handle it. This allows the local
            // to remain DNER-free and be enregistered.
            assert(lclFld->GetLclOffs() == 0);
            lclFld->ChangeType(varDsc->TypeGet());
            lclFld->SetOper(GT_LCL_VAR);
        }
        else if (!varDsc->lvDoNotEnregister)
        {
            lvaSetVarDoNotEnregister(lclNum DEBUGARG(DoNotEnregisterReason::BlockOpRet));
        }
    }

    return lclFld;
}

//-------------------------------------------------------------
// fgMorphSmpOpOptional: optional post-order morping of some SMP trees
//
// Arguments:
//   tree - tree to morph
//   optAssertionPropDone - [out, optional] set true if local assertions were
//      killed/genned by the optional morphing
//
// Returns:
//    Tree, possibly updated
//
GenTree* Compiler::fgMorphSmpOpOptional(GenTreeOp* tree, bool* optAssertionPropDone)
{
    genTreeOps oper = tree->gtOper;
    GenTree*   op1  = tree->gtOp1;
    GenTree*   op2  = tree->gtOp2;
    var_types  typ  = tree->TypeGet();

    if (fgGlobalMorph && GenTree::OperIsCommutative(oper))
    {
        /* Swap the operands so that the more expensive one is 'op1' */

        if (tree->gtFlags & GTF_REVERSE_OPS)
        {
            tree->gtOp1 = op2;
            tree->gtOp2 = op1;

            op2 = op1;
            op1 = tree->gtOp1;

            tree->gtFlags &= ~GTF_REVERSE_OPS;
        }

        if (oper == op2->gtOper)
        {
            /*  Reorder nested operators at the same precedence level to be
                left-recursive. For example, change "(a+(b+c))" to the
                equivalent expression "((a+b)+c)".
             */

            /* Things are handled differently for floating-point operators */

            if (!varTypeIsFloating(tree->TypeGet()))
            {
                fgMoveOpsLeft(tree);
                op1 = tree->gtOp1;
                op2 = tree->gtOp2;
            }
        }
    }

#if REARRANGE_ADDS

    /* Change "((x+icon)+y)" to "((x+y)+icon)"
       Don't reorder floating-point operations */

    if (fgGlobalMorph && (oper == GT_ADD) && !tree->gtOverflow() && op1->OperIs(GT_ADD) && !op1->gtOverflow() &&
        varTypeIsIntegralOrI(typ))
    {
        GenTree* ad1 = op1->AsOp()->gtOp1;
        GenTree* ad2 = op1->AsOp()->gtOp2;

        if (!op2->OperIsConst() && ad2->OperIsConst())
        {
            //  This takes
            //        + (tree)
            //       / \.
            //      /   \.
            //     /     \.
            //    + (op1) op2
            //   / \.
            //  /   \.
            // ad1  ad2
            //
            // and it swaps ad2 and op2.

            // Don't create a byref pointer that may point outside of the ref object.
            // If a GC happens, the byref won't get updated. This can happen if one
            // of the int components is negative. It also requires the address generation
            // be in a fully-interruptible code region.
            if (!varTypeIsGC(ad1->TypeGet()) && !varTypeIsGC(op2->TypeGet()))
            {
                tree->gtOp2 = ad2;

                op1->AsOp()->gtOp2 = op2;
                op1->gtFlags |= op2->gtFlags & GTF_ALL_EFFECT;

                op2 = tree->gtOp2;
            }
        }
    }

#endif

    /*-------------------------------------------------------------------------
     * Perform optional oper-specific postorder morphing
     */

    switch (oper)
    {
        case GT_STOREIND:
        case GT_STORE_BLK:
        case GT_STORE_LCL_VAR:
        case GT_STORE_LCL_FLD:
            if (varTypeIsStruct(typ) && !tree->IsPhiDefn())
            {
                // Block ops handle assertion kill/gen specially.
                // See PrepareDst and PropagateAssertions
                //
                if (optAssertionPropDone != nullptr)
                {
                    *optAssertionPropDone = true;
                }

                if (tree->OperIsCopyBlkOp())
                {
                    return fgMorphCopyBlock(tree);
                }
                else
                {
                    return fgMorphInitBlock(tree);
                }
            }

            /* Special case: a cast that can be thrown away */

            // TODO-Cleanup: fgMorphSmp does a similar optimization. However, it removes only
            // one cast and sometimes there is another one after it that gets removed by this
            // code. fgMorphSmp should be improved to remove all redundant casts so this code
            // can be removed.
            if (tree->OperIs(GT_STOREIND))
            {
                if (typ == TYP_LONG)
                {
                    break;
                }

                if (op2->gtFlags & GTF_ASG)
                {
                    break;
                }

                if (op2->gtFlags & GTF_CALL)
                {
                    break;
                }

                if (op2->OperIs(GT_CAST) && !op2->gtOverflow())
                {
                    var_types srct;
                    var_types cast;
                    var_types dstt;

                    srct = op2->AsCast()->CastOp()->TypeGet();
                    cast = (var_types)op2->CastToType();
                    dstt = tree->TypeGet();

                    /* Make sure these are all ints and precision is not lost */

                    if (genTypeSize(cast) >= genTypeSize(dstt) && dstt <= TYP_INT && srct <= TYP_INT)
                    {
                        op2 = tree->gtOp2 = op2->AsCast()->CastOp();
                    }
                }
            }
            break;

        case GT_MUL:

            /* Check for the case "(val + icon) * icon" */

            if (op2->OperIs(GT_CNS_INT) && op1->OperIs(GT_ADD))
            {
                GenTree* add = op1->AsOp()->gtOp2;

                if (add->IsCnsIntOrI() && (op2->GetScaleIndexMul() != 0))
                {
                    if (tree->gtOverflow() || op1->gtOverflow())
                    {
                        break;
                    }

                    ssize_t imul = op2->AsIntCon()->gtIconVal;
                    ssize_t iadd = add->AsIntCon()->gtIconVal;

                    /* Change '(val + iadd) * imul' -> '(val * imul) + (iadd * imul)' */

                    oper = GT_ADD;
                    tree->ChangeOper(oper);

                    op2->AsIntCon()->SetValueTruncating(iadd * imul);

                    op1->ChangeOper(GT_MUL);

                    add->AsIntCon()->SetIconValue(imul);
                }
            }

            break;

        case GT_DIV:

            /* For "val / 1", just return "val" */

            if (op2->IsIntegralConst(1))
            {
                DEBUG_DESTROY_NODE(tree);
                return op1;
            }
            break;

        case GT_UDIV:
        case GT_UMOD:
            tree->CheckDivideByConstOptimized(this);
            break;

        case GT_LSH:

            /* Check for the case "(val + icon) << icon" */

            if (op2->IsCnsIntOrI() && op1->OperIs(GT_ADD) && !op1->gtOverflow())
            {
                GenTree* cns = op1->AsOp()->gtOp2;

                if (cns->IsCnsIntOrI() && (op2->GetScaleIndexShf() != 0))
                {
                    ssize_t ishf = op2->AsIntConCommon()->IconValue();
                    ssize_t iadd = cns->AsIntConCommon()->IconValue();

                    // printf("Changing '(val+icon1)<<icon2' into '(val<<icon2+icon1<<icon2)'\n");

                    /* Change "(val + iadd) << ishf" into "(val<<ishf + iadd<<ishf)" */

                    tree->ChangeOper(GT_ADD);

                    // we are reusing the shift amount node here, but the type we want is that of the shift result
                    op2->gtType = op1->gtType;
                    op2->AsIntConCommon()->SetValueTruncating(iadd << ishf);
                    op1->ChangeOper(GT_LSH);
                    cns->AsIntConCommon()->SetIconValue(ishf);
                }
            }

            break;

        case GT_INIT_VAL:
            // Initialization values for initBlk have special semantics - their lower
            // byte is used to fill the struct. However, we allow 0 as a "bare" value,
            // which enables them to get a VNForZero, and be propagated.
            if (op1->IsIntegralConst(0))
            {
                return op1;
            }
            break;

        default:
            break;
    }
    return tree;
}

#if defined(FEATURE_HW_INTRINSICS)
//------------------------------------------------------------------------
// fgMorphHWIntrinsic: Morph a GenTreeHWIntrinsic tree.
//
// Arguments:
//    tree - The tree to morph
//
// Return Value:
//    The fully morphed tree.
//
GenTree* Compiler::fgMorphHWIntrinsic(GenTreeHWIntrinsic* tree)
{
    // It is important that this follows the general flow of fgMorphSmpOp
    // * Perform required preorder processing
    // * Process the operands, in order, if any
    // * Perform required postorder morphing
    // * Perform optional postorder morphing if optimizing
    //
    // It is also important that similar checks be done where relevant, so
    // if fgMorphSmpOp does a check for fgGlobalMorph or OptimizationEnabled
    // so should this method.

    // ------------------------------------------------------------------------
    // First do any PRE-ORDER processing
    //

    bool allArgsAreConst            = true;
    bool canBenefitFromConstantProp = false;
    bool hasImmediateOperand        = false;

    // Opportunistically, avoid unexpected CSE for hwintrinsics with certain const arguments
    NamedIntrinsic intrinsicId = tree->GetHWIntrinsicId();

    if (HWIntrinsicInfo::CanBenefitFromConstantProp(intrinsicId))
    {
        canBenefitFromConstantProp = true;
    }

    if (HWIntrinsicInfo::HasImmediateOperand(intrinsicId))
    {
        hasImmediateOperand = true;
    }

#ifdef TARGET_XARCH
    if (intrinsicId == NI_Vector128_op_Division || intrinsicId == NI_Vector256_op_Division)
    {
        fgAddCodeRef(compCurBB, SCK_DIV_BY_ZERO);
        fgAddCodeRef(compCurBB, SCK_OVERFLOW);
    }
#endif // TARGET_XARCH

    // ------------------------------------------------------------------------
    // Process the operands, if any
    //

    for (GenTree** use : tree->UseEdges())
    {
        *use             = fgMorphTree(*use);
        GenTree* operand = *use;

        if (operand->OperIsConst())
        {
            if (hasImmediateOperand && operand->IsCnsIntOrI())
            {
                operand->SetDoNotCSE();
            }
            else if (canBenefitFromConstantProp && operand->IsCnsVec())
            {
                if (tree->ShouldConstantProp(operand, operand->AsVecCon()))
                {
                    operand->SetDoNotCSE();
                }
            }
        }
        else
        {
            allArgsAreConst = false;
        }

        // Promoted structs after morph must be in one of two states:
        //  a) Fully eliminated from the IR (independent promotion) OR only be
        //     used by "special" nodes (e. g. multi-reg stores).
        //  b) Marked as do-not-enregister (dependent promotion).
        //
        // So here we preserve this invariant and mark any promoted structs as do-not-enreg.
        //
        if (operand->OperIs(GT_LCL_VAR))
        {
            GenTreeLclVar* lclVar = operand->AsLclVar();

            if (lvaGetDesc(lclVar)->lvPromoted)
            {
                lvaSetVarDoNotEnregister(lclVar->GetLclNum() DEBUGARG(DoNotEnregisterReason::SimdUserForcesDep));
            }
        }
    }

    gtUpdateNodeOperSideEffects(tree);

    for (GenTree* operand : tree->Operands())
    {
        tree->AddAllEffectsFlags(operand);
    }

    // ------------------------------------------------------------------------
    // Now do POST-ORDER processing
    //

    var_types   retType         = tree->TypeGet();
    CorInfoType simdBaseJitType = tree->GetSimdBaseJitType();
    var_types   simdBaseType    = tree->GetSimdBaseType();
    unsigned    simdSize        = tree->GetSimdSize();

    // Try to fold it, maybe we get lucky,
    GenTree* morphedTree = gtFoldExpr(tree);

    if (morphedTree->OperIsHWIntrinsic())
    {
        tree = morphedTree->AsHWIntrinsic();

        if (allArgsAreConst && tree->IsVectorCreate())
        {
            // Avoid unexpected CSE for constant arguments for Vector_.Create
            // but only if all arguments are constants.

            for (GenTree* arg : tree->Operands())
            {
                arg->SetDoNotCSE();
            }
        }

        // ------------------------------------------------------------------------
        // Perform the required oper-specific postorder morphing
        //

        // If the folded tree is a vector to mask conversion, or vice versa,
        // then we want to morph the inner operand as we may have folded something
        // like xor(masktovector(op1), AllBitsSet) into masktovector(not(op1)), which
        // can unlock further optimizations over op1, like the ability to invert
        // not(cmple(op1, op2)) into cmpgt(op1, op2)

        int opIndex = 0;

        if (tree->OperIsConvertVectorToMask() || tree->OperIsConvertMaskToVector())
        {
            opIndex = 1;
        }

#if defined(TARGET_ARM64)
        if (tree->OperIsConvertVectorToMask())
        {
            opIndex = 2;
        }
#endif // TARGET_ARM64

        if (opIndex != 0)
        {
            GenTree* innerOp = tree->Op(opIndex);

            if (innerOp->OperIsHWIntrinsic())
            {
                innerOp = fgMorphHWIntrinsicRequired(innerOp->AsHWIntrinsic());

                if (innerOp->OperIsHWIntrinsic())
                {
                    innerOp = fgMorphHWIntrinsicOptional(innerOp->AsHWIntrinsic());
                }

                tree->Op(opIndex) = innerOp;
            }
        }

        morphedTree = fgMorphHWIntrinsicRequired(tree);

        if (morphedTree->OperIsHWIntrinsic())
        {
            tree = morphedTree->AsHWIntrinsic();

            // ------------------------------------------------------------------------
            // Optional morphing is done if tree transformations is permitted
            //

            morphedTree = fgMorphHWIntrinsicOptional(tree);
        }
    }

    assert(retType == morphedTree->TypeGet());
    morphedTree->SetMorphed(this);
    return morphedTree;
}

//------------------------------------------------------------------------
// fgMorphHWIntrinsicRequired: Perform required postorder morphing of a GenTreeHWIntrinsic tree.
//
// Arguments:
//    tree - The tree to morph
//
// Return Value:
//    The morphed tree.
//
GenTree* Compiler::fgMorphHWIntrinsicRequired(GenTreeHWIntrinsic* tree)
{
    NamedIntrinsic intrinsic       = tree->GetHWIntrinsicId();
    var_types      retType         = tree->TypeGet();
    CorInfoType    simdBaseJitType = tree->GetSimdBaseJitType();
    var_types      simdBaseType    = tree->GetSimdBaseType();
    unsigned       simdSize        = tree->GetSimdSize();

    bool       isScalar = false;
    genTreeOps oper     = tree->GetOperForHWIntrinsicId(&isScalar);

    if (tree->isCommutativeHWIntrinsic())
    {
        assert(tree->GetOperandCount() == 2);

        GenTree*& op1 = tree->Op(1);
        GenTree*& op2 = tree->Op(2);

        if (op1->OperIsConst())
        {
            // Move constants from op1 to op2 for commutative operations
            std::swap(op1, op2);
        }

        if (((oper == GT_EQ) || (oper == GT_NE)) && op1->OperIsHWIntrinsic() && op2->IsCnsVec())
        {
            GenTreeHWIntrinsic* op1Intrinsic   = op1->AsHWIntrinsic();
            NamedIntrinsic      op1IntrinsicId = op1Intrinsic->GetHWIntrinsicId();
            var_types           op1Type        = op1Intrinsic->TypeGet();

            if (HWIntrinsicInfo::ReturnsPerElementMask(op1IntrinsicId) &&
                (genTypeSize(simdBaseType) == genTypeSize(op1Intrinsic->GetSimdBaseType())))
            {
                // This optimization is only safe if we know the other node produces
                // AllBitsSet or Zero per element and if the outer comparison is the
                // same size as what the other node produces for its mask

                bool reverseCond = false;

                if (oper == GT_EQ)
                {
                    // Handle `Mask == Zero` and `Zero == Mask` for integral types
                    if (op2->IsVectorZero())
                    {
                        reverseCond = true;
                    }
                }
                else if (oper == GT_NE)
                {
                    // Handle `Mask != AllBitsSet` and `AllBitsSet != Mask` for integral types
                    if (op2->IsVectorAllBitsSet())
                    {
                        reverseCond = true;
                    }
                }

                if (reverseCond)
                {
                    GenTree* newNode = nullptr;

                    if (op1Intrinsic->OperIsConvertVectorToMask())
                    {
#if defined(TARGET_XARCH)
                        op1 = op1Intrinsic->Op(1);
#elif defined(TARGET_ARM64)
                        op1 = op1Intrinsic->Op(2);
                        DEBUG_DESTROY_NODE(op1Intrinsic->Op(1));
#else
#error Unsupported platform
#endif // !TARGET_XARCH && !TARGET_ARM64

                        op1Type = op1->TypeGet();
                        DEBUG_DESTROY_NODE(op1Intrinsic);
                    }

                    if (op1Type == TYP_MASK)
                    {
#if defined(TARGET_XARCH)
                        newNode = gtNewSimdHWIntrinsicNode(op1Type, op1, NI_AVX512_NotMask, simdBaseJitType, simdSize);
#endif // TARGET_XARCH
                    }
                    else
                    {
                        newNode = gtNewSimdUnOpNode(GT_NOT, op1Type, op1, simdBaseJitType, simdSize);

#if defined(TARGET_XARCH)
                        newNode->AsHWIntrinsic()->Op(2)->SetMorphed(this);
#endif // TARGET_XARCH
                    }

                    if (newNode != nullptr)
                    {
                        DEBUG_DESTROY_NODE(op2);
                        DEBUG_DESTROY_NODE(tree);

                        if (op1Type != retType)
                        {
                            newNode = fgMorphHWIntrinsicRequired(newNode->AsHWIntrinsic());

                            if (retType == TYP_MASK)
                            {
                                newNode = gtNewSimdCvtVectorToMaskNode(retType, newNode, simdBaseJitType, simdSize);
                            }
                            else
                            {
                                newNode = gtNewSimdCvtMaskToVectorNode(retType, newNode, simdBaseJitType, simdSize);
                            }
                        }

                        return fgMorphHWIntrinsicRequired(newNode->AsHWIntrinsic());
                    }
                }
            }
        }
    }
    else if (GenTree::OperIsCompare(oper))
    {
        assert(tree->GetOperandCount() == 2);

        GenTree* op1 = tree->Op(1);
        GenTree* op2 = tree->Op(2);

        if (!isScalar && op1->IsCnsVec())
        {
            // Move constant vectors from op1 to op2 for comparison operations
            // Noting that we can't handle scalar operations since they can copy upper bits from op1

            genTreeOps newOper = GenTree::SwapRelop(oper);
            var_types  lookupType =
                GenTreeHWIntrinsic::GetLookupTypeForCmpOp(this, newOper, retType, simdBaseType, simdSize);
            NamedIntrinsic newId = GenTreeHWIntrinsic::GetHWIntrinsicIdForCmpOp(this, newOper, lookupType, op2, op1,
                                                                                simdBaseType, simdSize, isScalar);

            if (newId != NI_Illegal)
            {
                tree->ResetHWIntrinsicId(newId, op2, op1);

                if (lookupType != retType)
                {
                    assert(varTypeIsSIMD(retType));
                    assert(varTypeIsMask(lookupType));

                    tree->gtType = lookupType;
                    tree = gtNewSimdCvtMaskToVectorNode(retType, tree, simdBaseJitType, simdSize)->AsHWIntrinsic();
                    return fgMorphHWIntrinsicRequired(tree);
                }
            }
        }
    }
#if defined(TARGET_XARCH)
    else
    {
        // We have some other comparison intrinsics that don't map to the simple forms

        switch (intrinsic)
        {
            case NI_AVX_Compare:
            case NI_AVX512_CompareMask:
            {
                assert(tree->GetOperandCount() == 3);

                GenTree* op1 = tree->Op(1);
                GenTree* op2 = tree->Op(2);
                GenTree* op3 = tree->Op(3);

                if (!op1->IsCnsVec() || !op3->IsCnsIntOrI())
                {
                    break;
                }

                FloatComparisonMode mode    = static_cast<FloatComparisonMode>(op3->AsIntConCommon()->IntegralValue());
                FloatComparisonMode newMode = mode;

                switch (mode)
                {
                    case FloatComparisonMode::UnorderedEqualNonSignaling:
                    case FloatComparisonMode::OrderedNotEqualNonSignaling:
                    case FloatComparisonMode::UnorderedEqualSignaling:
                    case FloatComparisonMode::OrderedNotEqualSignaling:
                    {
                        tree->Op(1) = op2;
                        tree->Op(2) = op1;
                        break;
                    }

                    default:
                    {
                        // Other modes should either have been normalized or
                        // will be out of range values and can't be handled
                        break;
                    }
                }
                break;
            }

            default:
            {
                break;
            }
        }
    }
#endif // TARGET_XARCH

    switch (oper)
    {
        // Transforms:
        // 1. v1 - cns2 to v1 + (-cns2); for integers
        // 2. 0 - v1 to -v1; for integers on supported platforms
        // 3. cns1 - v2 to (-v2) + cns1; for integers
        case GT_SUB:
        {
            if (!fgGlobalMorph)
            {
                break;
            }

            if (!varTypeIsIntegral(simdBaseType))
            {
                break;
            }

            GenTree* op1 = tree->Op(1);
            GenTree* op2 = tree->Op(2);

            if (op2->IsCnsVec())
            {
                op2->AsVecCon()->EvaluateUnaryInPlace(GT_NEG, isScalar, simdBaseType);
                fgUpdateConstTreeValueNumber(op2);

                NamedIntrinsic addIntrinsic =
                    GenTreeHWIntrinsic::GetHWIntrinsicIdForBinOp(this, GT_ADD, op1, op2, simdBaseType, simdSize,
                                                                 isScalar);

                tree->ChangeHWIntrinsicId(addIntrinsic, op1, op2);
                return fgMorphHWIntrinsicRequired(tree);
            }
            else if (op1->IsCnsVec())
            {
                if (op1->IsVectorZero())
                {
#if defined(TARGET_ARM64)
                    // xarch doesn't have a native GT_NEG representation for integers and itself uses (Zero - v1)
                    op2 = gtNewSimdUnOpNode(GT_NEG, retType, op2, simdBaseJitType, simdSize);

                    DEBUG_DESTROY_NODE(op1);
                    DEBUG_DESTROY_NODE(tree);

                    return fgMorphHWIntrinsicRequired(op2->AsHWIntrinsic());
#endif // TARGET_ARM64
                }
                else
                {
                    op2 = gtNewSimdUnOpNode(GT_NEG, retType, op2, simdBaseJitType, simdSize);

#if defined(TARGET_XARCH)
                    if (varTypeIsFloating(simdBaseType))
                    {
                        op2->AsHWIntrinsic()->Op(2)->SetMorphed(this);
                    }
                    else
                    {
                        op2->AsHWIntrinsic()->Op(1)->SetMorphed(this);
                    }
#endif // TARGET_XARCH

                    NamedIntrinsic addIntrinsic =
                        GenTreeHWIntrinsic::GetHWIntrinsicIdForBinOp(this, GT_ADD, op2, op1, simdBaseType, simdSize,
                                                                     isScalar);

                    tree->ChangeHWIntrinsicId(addIntrinsic, op2, op1);

                    op2 = fgMorphHWIntrinsicRequired(op2->AsHWIntrinsic());
                    op2->SetMorphed(this);
                    tree->Op(1) = op2;

                    return fgMorphHWIntrinsicRequired(tree);
                }
            }
            break;
        }

#if defined(TARGET_ARM64)
        // Transforms:
        // 1. (v1 ^ AllBitsSet) to ~v1; on supported platforms
        // 2. (v1 ^ -0.0) to -v1; for floating-point on supported platforms
        case GT_XOR:
        {
            GenTree* op1 = tree->Op(1);
            GenTree* op2 = tree->Op(2);

            if (op2->IsVectorAllBitsSet())
            {
                // xarch doesn't have a native GT_NOT representation and itself uses (v1 ^ AllBitsSet)
                op1 = gtNewSimdUnOpNode(GT_NOT, retType, op1, simdBaseJitType, simdSize);

                DEBUG_DESTROY_NODE(op2);
                DEBUG_DESTROY_NODE(tree);

                return fgMorphHWIntrinsicRequired(op1->AsHWIntrinsic());
            }

            if (varTypeIsFloating(simdBaseType) && op2->IsVectorNegativeZero(simdBaseType))
            {
                // xarch doesn't have a native GT_NEG representation for floating-point and itself uses (v1 ^ -0.0)
                op1 = gtNewSimdUnOpNode(GT_NEG, retType, op1, simdBaseJitType, simdSize);

                DEBUG_DESTROY_NODE(op2);
                DEBUG_DESTROY_NODE(tree);

                return fgMorphHWIntrinsicRequired(op1->AsHWIntrinsic());
            }
            break;
        }
#endif // TARGET_ARM64

        default:
        {
            break;
        }
    }

    if (opts.OptimizationEnabled())
    {
        return fgOptimizeHWIntrinsic(tree);
    }
    return tree;
}

//------------------------------------------------------------------------
// fgMorphHWIntrinsicOptional: Perform optional postorder morphing of a GenTreeHWIntrinsic tree.
//
// Arguments:
//    tree                 - The tree to morph
//
// Return Value:
//    The morphed tree.
//
GenTree* Compiler::fgMorphHWIntrinsicOptional(GenTreeHWIntrinsic* tree)
{
    return tree;
}
#endif // FEATURE_HW_INTRINSICS

//------------------------------------------------------------------------
// fgMorphModToZero: Transform 'a % 1' into the equivalent '0'.
//
// Arguments:
//    tree - The GT_MOD/GT_UMOD tree to morph
//
// Returns:
//    The morphed tree, will be a GT_COMMA or a zero constant node.
//    Can return null if the transformation did not happen.
//
GenTree* Compiler::fgMorphModToZero(GenTreeOp* tree)
{
    assert(tree->OperIs(GT_MOD, GT_UMOD));
    assert(tree->gtOp2->IsIntegralConst(1));

    if (opts.OptimizationDisabled())
        return nullptr;

    // Do not transform this if there are side effects and we are not in global morph.
    // If we want to allow this, we need to update value numbers for the GT_COMMA.
    if (!fgGlobalMorph && ((tree->gtGetOp1()->gtFlags & GTF_SIDE_EFFECT) != 0))
        return nullptr;

    JITDUMP("\nMorphing MOD/UMOD [%06u] to Zero\n", dspTreeID(tree));

    GenTree* op1 = tree->gtGetOp1();
    GenTree* op2 = tree->gtGetOp2();

    op2->AsIntConCommon()->SetIntegralValue(0);
    fgUpdateConstTreeValueNumber(op2);

    GenTree* const zero = op2;

    GenTree* op1SideEffects = nullptr;
    gtExtractSideEffList(op1, &op1SideEffects, GTF_ALL_EFFECT);
    if (op1SideEffects != nullptr)
    {
        GenTree* comma = gtNewOperNode(GT_COMMA, zero->TypeGet(), op1SideEffects, zero);
        comma->SetMorphed(this);
        DEBUG_DESTROY_NODE(tree);
        return comma;
    }
    else
    {
        zero->SetMorphed(this);
        DEBUG_DESTROY_NODE(tree->gtOp1);
        DEBUG_DESTROY_NODE(tree);
        return zero;
    }
}

//------------------------------------------------------------------------
// fgMorphModToSubMulDiv: Transform a % b into the equivalent a - (a / b) * b
// (see ECMA III 3.55 and III.3.56).
//
// Arguments:
//    tree - The GT_MOD/GT_UMOD tree to morph
//
// Returns:
//    The morphed tree
//
// Notes:
//    For ARM64 we don't have a remainder instruction so this transform is
//    always done. For XARCH this transform is done if we know that magic
//    division will be used, in that case this transform allows CSE to
//    eliminate the redundant div from code like "x = a / 3; y = a % 3;".
//
//    Before:
//        *  RETURN    int
//        \--*  MOD       int
//           +--*  MUL       int
//           |  +--*  LCL_VAR   int    V00 arg0
//           |  \--*  LCL_VAR   int    V00 arg0
//           \--*  LCL_VAR   int    V01 arg1
//    After:
//        *  RETURN    int
//        \--*  COMMA     int
//           +--*  STORE_LCL_VAR int    V03 tmp1
//           |  \--*  MUL       int
//           |     +--*  LCL_VAR   int    V00 arg0
//           |     \--*  LCL_VAR   int    V00 arg0
//           \--*  SUB       int
//              +--*  LCL_VAR   int    V03 tmp1
//              \--*  MUL       int
//                 +--*  DIV       int
//                 |  +--*  LCL_VAR   int    V03 tmp1
//                 |  \--*  LCL_VAR   int    V01 arg1
//                 \--*  LCL_VAR   int    V01 arg1
GenTree* Compiler::fgMorphModToSubMulDiv(GenTreeOp* tree)
{
    JITDUMP("\nMorphing MOD/UMOD [%06u] to Sub/Mul/Div\n", dspTreeID(tree));

    if (tree->OperIs(GT_MOD))
    {
        tree->SetOper(GT_DIV);
    }
    else if (tree->OperIs(GT_UMOD))
    {
        tree->SetOper(GT_UDIV);
    }
    else
    {
        noway_assert(!"Illegal gtOper in fgMorphModToSubMulDiv");
    }

    GenTreeOp* const div = tree;

    GenTree* opA = div->gtGetOp1();
    GenTree* opB = div->gtGetOp2();
    if (div->IsReverseOp())
    {
        std::swap(opA, opB);
    }

    TempInfo tempInfos[2];
    int      tempInfoCount = 0;

    // This transform runs in pre-morph so we cannot rely on GTF_GLOB_REF.
    // Furthermore, this logic is somewhat complicated since the divisor and
    // dividend are arbitrary nodes. For instance, if we spill the divisor and
    // the dividend is a local, we need to spill the dividend too unless the
    // divisor could not cause it to be reassigned.
    // There is even more complexity due to needing to handle GTF_REVERSE_OPS.
    //
    // This could be slightly better via GTF_CALL and GTF_ASG checks on the
    // divisor but the diffs of this were minor and the extra complexity seemed
    // not worth it.
    bool spillA;
    bool spillB;
    if (opB->IsInvariant() || opB->OperIsLocal())
    {
        spillB = false;
        spillA = !opA->IsInvariant() && !opA->OperIsLocal();
    }
    else
    {
        spillB = true;
        spillA = !opA->IsInvariant();
    }

    if (spillA)
    {
        tempInfos[tempInfoCount] = fgMakeTemp(opA);
        opA                      = tempInfos[tempInfoCount].load;
        tempInfoCount++;
    }

    if (spillB)
    {
        tempInfos[tempInfoCount] = fgMakeTemp(opB);
        opB                      = tempInfos[tempInfoCount].load;
        tempInfoCount++;
    }

    GenTree* dividend = div->IsReverseOp() ? opB : opA;
    GenTree* divisor  = div->IsReverseOp() ? opA : opB;

    div->gtOp1 = gtCloneExpr(dividend);
    div->gtOp2 = gtCloneExpr(divisor);

    var_types      type = div->gtType;
    GenTree* const mul  = gtNewOperNode(GT_MUL, type, div, divisor);
    GenTree* const sub  = gtNewOperNode(GT_SUB, type, dividend, mul);

    GenTree* result = sub;
    // We loop backwards as it is easier to create new commas
    // within one another for their sequence order.
    for (int i = tempInfoCount - 1; i >= 0; i--)
    {
        result = gtNewOperNode(GT_COMMA, type, tempInfos[i].store, result);
    }

    result->SetMorphed(this);
    optRecordSsaUses(result, compCurBB);
    div->CheckDivideByConstOptimized(this);
    return result;
}

//------------------------------------------------------------------------
// fgMorphUModToAndSub: Transform a % b into the equivalent a & (b - 1).
// '%' must be unsigned (GT_UMOD).
// 'a' and 'b' must be integers.
// 'b' must be a constant and a power of two.
//
// Arguments:
//    tree - The GT_UMOD tree to morph
//
// Returns:
//    The morphed tree
//
// Notes:
//    This is more optimized than calling fgMorphModToSubMulDiv.
//
GenTree* Compiler::fgMorphUModToAndSub(GenTreeOp* tree)
{
    JITDUMP("\nMorphing UMOD [%06u] to And/Sub\n", dspTreeID(tree));

    assert(tree->OperIs(GT_UMOD));
    assert(tree->gtOp2->IsIntegralConstUnsignedPow2());

    const var_types type = tree->TypeGet();

    const size_t   cnsValue = (static_cast<size_t>(tree->gtOp2->AsIntConCommon()->IntegralValue())) - 1;
    GenTree* const newTree  = gtNewOperNode(GT_AND, type, tree->gtOp1, gtNewIconNodeWithVN(this, cnsValue, type));

    newTree->SetMorphed(this);
    DEBUG_DESTROY_NODE(tree->gtOp2);
    DEBUG_DESTROY_NODE(tree);
    return newTree;
}

//------------------------------------------------------------------------------
// fgOperIsBitwiseRotationRoot : Check if the operation can be a root of a bitwise rotation tree.
//
//
// Arguments:
//    oper  - Operation to check
//
// Return Value:
//    True if the operation can be a root of a bitwise rotation tree; false otherwise.

bool Compiler::fgOperIsBitwiseRotationRoot(genTreeOps oper)
{
    return (oper == GT_OR) || (oper == GT_XOR);
}

//------------------------------------------------------------------------------
// fgRecognizeAndMorphBitwiseRotation : Check if the tree represents a left or right rotation. If so, return
//                                      an equivalent GT_ROL or GT_ROR tree; otherwise, return the original tree.
//
// Arguments:
//    tree  - tree to check for a rotation pattern
//
// Return Value:
//    An equivalent GT_ROL or GT_ROR tree if a pattern is found; "nullptr" otherwise.
//
// Assumption:
//    The input is a GT_OR or a GT_XOR tree.

GenTree* Compiler::fgRecognizeAndMorphBitwiseRotation(GenTree* tree)
{
    //
    // Check for a rotation pattern, e.g.,
    //
    //                         OR                      ROL
    //                      /      \                   / \.
    //                    LSH      RSZ      ->        x   y
    //                    / \      / \.
    //                   x  AND   x  AND
    //                      / \      / \.
    //                     y  31   ADD  31
    //                             / \.
    //                            NEG 32
    //                             |
    //                             y
    // The patterns recognized:
    // (x << (y & M)) op (x >>> ((-y + N) & M))
    // (x >>> ((-y + N) & M)) op (x << (y & M))
    //
    // (x << y) op (x >>> (-y + N))
    // (x >> > (-y + N)) op (x << y)
    //
    // (x >>> (y & M)) op (x << ((-y + N) & M))
    // (x << ((-y + N) & M)) op (x >>> (y & M))
    //
    // (x >>> y) op (x << (-y + N))
    // (x << (-y + N)) op (x >>> y)
    //
    // (x << c1) op (x >>> c2)
    // (x >>> c1) op (x << c2)
    //
    // where
    // c1 and c2 are const
    // c1 + c2 == bitsize(x)
    // N == bitsize(x)
    // M is const
    // M & (N - 1) == N - 1
    // op is either | or ^

    if (((tree->gtFlags & GTF_PERSISTENT_SIDE_EFFECTS) != 0) || ((tree->gtFlags & GTF_ORDER_SIDEEFF) != 0))
    {
        // We can't do anything if the tree has stores, calls, or volatile reads. Note that we allow
        // GTF_EXCEPT side effect since any exceptions thrown by the original tree will be thrown by
        // the transformed tree as well.
        return nullptr;
    }

    genTreeOps oper = tree->OperGet();
    assert(fgOperIsBitwiseRotationRoot(oper));

    // Check if we have an LSH on one side of the OR and an RSZ on the other side.
    GenTree* op1            = tree->gtGetOp1();
    GenTree* op2            = tree->gtGetOp2();
    GenTree* leftShiftTree  = nullptr;
    GenTree* rightShiftTree = nullptr;
    if (op1->OperIs(GT_LSH) && op2->OperIs(GT_RSZ))
    {
        leftShiftTree  = op1;
        rightShiftTree = op2;
    }
    else if (op1->OperIs(GT_RSZ) && op2->OperIs(GT_LSH))
    {
        leftShiftTree  = op2;
        rightShiftTree = op1;
    }
    else
    {
        return nullptr;
    }

    // Check if the trees representing the value to shift are identical.
    // We already checked that there are no side effects above.
    if (GenTree::Compare(leftShiftTree->gtGetOp1(), rightShiftTree->gtGetOp1()))
    {
        GenTree*  rotatedValue           = leftShiftTree->gtGetOp1();
        var_types rotatedValueActualType = genActualType(rotatedValue->gtType);
        ssize_t   rotatedValueBitSize    = genTypeSize(rotatedValueActualType) * 8;
        noway_assert((rotatedValueBitSize == 32) || (rotatedValueBitSize == 64));
        GenTree* leftShiftIndex  = leftShiftTree->gtGetOp2();
        GenTree* rightShiftIndex = rightShiftTree->gtGetOp2();

        // The shift index may be masked. At least (rotatedValueBitSize - 1) lower bits
        // shouldn't be masked for the transformation to be valid. If additional
        // higher bits are not masked, the transformation is still valid since the result
        // of MSIL shift instructions is unspecified if the shift amount is greater or equal
        // than the width of the value being shifted.
        ssize_t minimalMask    = rotatedValueBitSize - 1;
        ssize_t leftShiftMask  = -1;
        ssize_t rightShiftMask = -1;

        if (leftShiftIndex->OperIs(GT_AND))
        {
            if (leftShiftIndex->gtGetOp2()->IsCnsIntOrI())
            {
                leftShiftMask  = leftShiftIndex->gtGetOp2()->AsIntCon()->gtIconVal;
                leftShiftIndex = leftShiftIndex->gtGetOp1();
            }
            else
            {
                return nullptr;
            }
        }

        if (rightShiftIndex->OperIs(GT_AND))
        {
            if (rightShiftIndex->gtGetOp2()->IsCnsIntOrI())
            {
                rightShiftMask  = rightShiftIndex->gtGetOp2()->AsIntCon()->gtIconVal;
                rightShiftIndex = rightShiftIndex->gtGetOp1();
            }
            else
            {
                return nullptr;
            }
        }

        if (((minimalMask & leftShiftMask) != minimalMask) || ((minimalMask & rightShiftMask) != minimalMask))
        {
            // The shift index is overmasked, e.g., we have
            // something like (x << y & 15) or
            // (x >> (32 - y) & 15 with 32 bit x.
            // The transformation is not valid.
            return nullptr;
        }

        GenTree*   shiftIndexWithAdd    = nullptr;
        GenTree*   shiftIndexWithoutAdd = nullptr;
        genTreeOps rotateOp             = GT_NONE;
        GenTree*   rotateIndex          = nullptr;

        if (leftShiftIndex->OperIs(GT_ADD))
        {
            shiftIndexWithAdd    = leftShiftIndex;
            shiftIndexWithoutAdd = rightShiftIndex;
            rotateOp             = GT_ROR;
        }
        else if (rightShiftIndex->OperIs(GT_ADD))
        {
            shiftIndexWithAdd    = rightShiftIndex;
            shiftIndexWithoutAdd = leftShiftIndex;
            rotateOp             = GT_ROL;
        }

        if (shiftIndexWithAdd != nullptr)
        {
            if (shiftIndexWithAdd->gtGetOp2()->IsCnsIntOrI())
            {
                if (shiftIndexWithAdd->gtGetOp2()->AsIntCon()->gtIconVal == rotatedValueBitSize)
                {
                    if (shiftIndexWithAdd->gtGetOp1()->OperGet() == GT_NEG)
                    {
                        if (GenTree::Compare(shiftIndexWithAdd->gtGetOp1()->gtGetOp1(), shiftIndexWithoutAdd))
                        {
                            // We found one of these patterns:
                            // (x << (y & M)) | (x >>> ((-y + N) & M))
                            // (x << y) | (x >>> (-y + N))
                            // (x >>> (y & M)) | (x << ((-y + N) & M))
                            // (x >>> y) | (x << (-y + N))
                            // where N == bitsize(x), M is const, and
                            // M & (N - 1) == N - 1

#ifndef TARGET_64BIT
                            if (!shiftIndexWithoutAdd->IsCnsIntOrI() && (rotatedValueBitSize == 64))
                            {
                                // TODO-X86-CQ: we need to handle variable-sized long shifts specially on x86.
                                // GT_LSH, GT_RSH, and GT_RSZ have helpers for this case. We may need
                                // to add helpers for GT_ROL and GT_ROR.
                                return nullptr;
                            }
#endif

                            rotateIndex = shiftIndexWithoutAdd;
                        }
                    }
                }
            }
        }
        else if ((leftShiftIndex->IsCnsIntOrI() && rightShiftIndex->IsCnsIntOrI()))
        {
            if (leftShiftIndex->AsIntCon()->gtIconVal + rightShiftIndex->AsIntCon()->gtIconVal == rotatedValueBitSize)
            {
                // We found this pattern:
                // (x << c1) | (x >>> c2)
                // where c1 and c2 are const and c1 + c2 == bitsize(x)
                rotateOp    = GT_ROL;
                rotateIndex = leftShiftIndex;
            }
        }

        if (rotateIndex != nullptr)
        {
            noway_assert(GenTree::OperIsRotate(rotateOp));

            GenTreeFlags inputTreeEffects = tree->gtFlags & GTF_ALL_EFFECT;

            // We can use the same tree only during global morph; reusing the tree in a later morph
            // may invalidate value numbers.
            if (fgGlobalMorph)
            {
                tree->AsOp()->gtOp1 = rotatedValue;
                tree->AsOp()->gtOp2 = rotateIndex;
                tree->ChangeOper(rotateOp);

                unsigned childFlags = 0;
                for (GenTree* op : tree->Operands())
                {
                    childFlags |= (op->gtFlags & GTF_ALL_EFFECT);
                }

                // The parent's flags should be a superset of its operands' flags
                noway_assert((inputTreeEffects & childFlags) == childFlags);
            }
            else
            {
                tree = gtNewOperNode(rotateOp, rotatedValueActualType, rotatedValue, rotateIndex);
                noway_assert(inputTreeEffects == (tree->gtFlags & GTF_ALL_EFFECT));
            }

            return tree;
        }
    }

    return nullptr;
}

#if !defined(TARGET_64BIT)
//------------------------------------------------------------------------------
// fgRecognizeAndMorphLongMul : Check for and morph long multiplication with 32 bit operands.
//
// Uses "GenTree::IsValidLongMul" to check for the long multiplication pattern. Will swap
// operands if the first one is a constant and the second one is not, even for trees which
// end up not being eligibile for long multiplication.
//
// Arguments:
//    mul  -  GT_MUL tree to check for a long multiplication opportunity
//
// Return Value:
//    The original tree, with operands possibly swapped, if it is not eligible for long multiplication.
//    Tree with GTF_MUL_64RSLT set, side effect flags propagated, and children morphed if it is.
//
GenTreeOp* Compiler::fgRecognizeAndMorphLongMul(GenTreeOp* mul)
{
    assert(mul->OperIs(GT_MUL));
    assert(mul->TypeIs(TYP_LONG));

    GenTree* op1 = mul->gtGetOp1();
    GenTree* op2 = mul->gtGetOp2();

    // "IsValidLongMul" and decomposition do not handle constant op1.
    if (op1->IsIntegralConst())
    {
        std::swap(op1, op2);
        mul->gtOp1 = op1;
        mul->gtOp2 = op2;
    }

    if (!mul->IsValidLongMul())
    {
        return mul;
    }

    // MUL_LONG needs to do the work the casts would have done.
    mul->ClearUnsigned();
    if (op1->IsUnsigned())
    {
        mul->SetUnsigned();
    }

    // "IsValidLongMul" returned "true", so this GT_MUL cannot overflow.
    mul->ClearOverflow();
    mul->Set64RsltMul();

    return fgMorphLongMul(mul);
}

//------------------------------------------------------------------------------
// fgMorphLongMul : Morphs GT_MUL nodes marked with GTF_MUL_64RSLT.
//
// Morphs *only* the operands of casts that compose the long mul to
// avoid them being folded always.
//
// Arguments:
//    mul  -  GT_MUL tree to morph operands of
//
// Return Value:
//    The original tree, with operands morphed and flags propagated.
//
GenTreeOp* Compiler::fgMorphLongMul(GenTreeOp* mul)
{
    INDEBUG(mul->DebugCheckLongMul());

    GenTree* op1 = mul->gtGetOp1();
    GenTree* op2 = mul->gtGetOp2();

    // Morph the operands. We cannot allow the casts to go away, so we morph their operands directly.
    op1->AsCast()->CastOp() = fgMorphTree(op1->AsCast()->CastOp());
    op1->SetAllEffectsFlags(op1->AsCast()->CastOp());

    if (op2->OperIs(GT_CAST))
    {
        op2->AsCast()->CastOp() = fgMorphTree(op2->AsCast()->CastOp());
        op2->SetAllEffectsFlags(op2->AsCast()->CastOp());
    }

    mul->SetAllEffectsFlags(op1, op2);
    op1->SetDoNotCSE();
    op1->SetMorphed(this);
    op2->SetDoNotCSE();
    op2->SetMorphed(this);

    return mul;
}
#endif // !defined(TARGET_64BIT)

/*****************************************************************************
 *
 *  Transform the given tree for code generation and return an equivalent tree.
 */

GenTree* Compiler::fgMorphTree(GenTree* tree, MorphAddrContext* mac)
{
    assert(tree);
    tree->ClearMorphed();

#ifdef DEBUG
    if (verbose)
    {
        if ((unsigned)JitConfig.JitBreakMorphTree() == tree->gtTreeID)
        {
            noway_assert(!"JitBreakMorphTree hit");
        }
    }
#endif

#ifdef DEBUG
    int thisMorphNum = 0;
    if (verbose && treesBeforeAfterMorph)
    {
        thisMorphNum = morphNum++;
        printf("\nfgMorphTree (before %d):\n", thisMorphNum);
        gtDispTree(tree);
    }
#endif

    bool optAssertionPropDone = false;

    /*-------------------------------------------------------------------------
     * fgMorphTree() can potentially replace a tree with another, and the
     * caller has to store the return value correctly.
     * Turn this on to always make copy of "tree" here to shake out
     * hidden/unupdated references.
     */

#ifdef DEBUG

    if (compStressCompile(STRESS_GENERIC_CHECK, 0))
    {
        GenTree* copy;

        if (GenTree::s_gtNodeSizes[tree->gtOper] == TREE_NODE_SZ_SMALL)
        {
            copy = gtNewLargeOperNode(GT_ADD, TYP_INT);
        }
        else
        {
            copy = new (this, GT_CALL) GenTreeCall(TYP_INT);
        }

        copy->ReplaceWith(tree, this);

#if defined(LATE_DISASM)
        // GT_CNS_INT is considered small, so ReplaceWith() won't copy all fields
        if (tree->IsIconHandle())
        {
            copy->AsIntCon()->gtCompileTimeHandle = tree->AsIntCon()->gtCompileTimeHandle;
        }
#endif

        DEBUG_DESTROY_NODE(tree);
        tree = copy;
    }
#endif // DEBUG

    if (fgGlobalMorph)
    {
        /* Before morphing the tree, we try to propagate any active assertions */
        if (optLocalAssertionProp)
        {
            /* Do we have any active assertions? */

            if (optAssertionCount > 0)
            {
                GenTree* newTree = tree;
                while (newTree != nullptr)
                {
                    tree = newTree;
                    /* newTree is non-Null if we propagated an assertion */
                    newTree = optAssertionProp(apLocal, tree, nullptr, nullptr);
                }
                assert(tree != nullptr);
            }
        }
        assert(tree != nullptr);
    }

    /* Figure out what kind of a node we have */

    unsigned const kind = tree->OperKind();

    /* Is this a constant node? */

    if (tree->OperIsConst())
    {
        tree = fgMorphConst(tree);
        goto DONE;
    }

    /* Is this a leaf node? */

    if (kind & GTK_LEAF)
    {
        tree = fgMorphLeaf(tree);
        goto DONE;
    }

    /* Is it a 'simple' unary/binary operator? */

    if (kind & GTK_SMPOP)
    {
        tree = fgMorphSmpOp(tree, mac, &optAssertionPropDone);
        goto DONE;
    }

    /* See what kind of a special operator we have here */

    switch (tree->OperGet())
    {
        case GT_CALL:
            if (tree->OperMayThrow(this))
            {
                tree->gtFlags |= GTF_EXCEPT;
            }
            else
            {
                tree->gtFlags &= ~GTF_EXCEPT;
            }
            tree = fgMorphCall(tree->AsCall());
            break;

#if defined(FEATURE_HW_INTRINSICS)
        case GT_HWINTRINSIC:
            tree = fgMorphHWIntrinsic(tree->AsHWIntrinsic());
            break;
#endif // FEATURE_HW_INTRINSICS

        case GT_ARR_ELEM:
            tree->AsArrElem()->gtArrObj = fgMorphTree(tree->AsArrElem()->gtArrObj);

            unsigned dim;
            for (dim = 0; dim < tree->AsArrElem()->gtArrRank; dim++)
            {
                tree->AsArrElem()->gtArrInds[dim] = fgMorphTree(tree->AsArrElem()->gtArrInds[dim]);
            }

            tree->gtFlags &= ~GTF_CALL;

            tree->gtFlags |= tree->AsArrElem()->gtArrObj->gtFlags & GTF_ALL_EFFECT;

            for (dim = 0; dim < tree->AsArrElem()->gtArrRank; dim++)
            {
                tree->gtFlags |= tree->AsArrElem()->gtArrInds[dim]->gtFlags & GTF_ALL_EFFECT;
            }

            if (fgGlobalMorph)
            {
                fgAddCodeRef(compCurBB, SCK_RNGCHK_FAIL);
            }
            break;

        case GT_PHI:
            tree->gtFlags &= ~GTF_ALL_EFFECT;
            for (GenTreePhi::Use& use : tree->AsPhi()->Uses())
            {
                use.SetNode(fgMorphTree(use.GetNode()));
                tree->gtFlags |= use.GetNode()->gtFlags & GTF_ALL_EFFECT;
            }
            break;

        case GT_FIELD_LIST:
            tree->gtFlags &= ~GTF_ALL_EFFECT;
            for (GenTreeFieldList::Use& use : tree->AsFieldList()->Uses())
            {
                use.SetNode(fgMorphTree(use.GetNode()));
                tree->gtFlags |= (use.GetNode()->gtFlags & GTF_ALL_EFFECT);
            }
            break;

        case GT_CMPXCHG:
            tree->AsCmpXchg()->Addr()      = fgMorphTree(tree->AsCmpXchg()->Addr());
            tree->AsCmpXchg()->Data()      = fgMorphTree(tree->AsCmpXchg()->Data());
            tree->AsCmpXchg()->Comparand() = fgMorphTree(tree->AsCmpXchg()->Comparand());
            gtUpdateNodeSideEffects(tree);
            break;

        case GT_SELECT:
            tree->AsConditional()->gtCond = fgMorphTree(tree->AsConditional()->gtCond);
            tree->AsConditional()->gtOp1  = fgMorphTree(tree->AsConditional()->gtOp1);
            tree->AsConditional()->gtOp2  = fgMorphTree(tree->AsConditional()->gtOp2);

            tree->gtFlags &= (~GTF_EXCEPT & ~GTF_CALL);

            tree->gtFlags |= tree->AsConditional()->gtCond->gtFlags & GTF_ALL_EFFECT;
            tree->gtFlags |= tree->AsConditional()->gtOp1->gtFlags & GTF_ALL_EFFECT;
            tree->gtFlags |= tree->AsConditional()->gtOp2->gtFlags & GTF_ALL_EFFECT;

            // Try to fold away any constants etc.
            tree = gtFoldExpr(tree);

            break;

        default:
#ifdef DEBUG
            gtDispTree(tree);
#endif
            noway_assert(!"unexpected operator");
    }
DONE:

    fgMorphTreeDone(tree, optAssertionPropDone DEBUGARG(thisMorphNum));

    return tree;
}

//------------------------------------------------------------------------
// fgKillDependentAssertionsSingle: Kill all assertions specific to lclNum
//
// Arguments:
//    lclNum - The varNum of the lclVar for which we're killing assertions.
//    tree   - (DEBUG only) the tree responsible for killing its assertions.
//
void Compiler::fgKillDependentAssertionsSingle(unsigned lclNum DEBUGARG(GenTree* tree))
{
    // Active dependent assertions are killed here
    //
    ASSERT_TP killed = GetAssertionDep(lclNum);

#ifdef DEBUG
    bool hasKills = !BitVecOps::IsEmptyIntersection(apTraits, apLocal, killed);
    if (hasKills)
    {
        AssertionIndex index = optAssertionCount;
        while (killed && (index > 0))
        {
            if (BitVecOps::IsMember(apTraits, killed, index - 1))
            {
                AssertionDsc* curAssertion = optGetAssertion(index);
                noway_assert((curAssertion->op1.lclNum == lclNum) ||
                             ((curAssertion->op2.kind == O2K_LCLVAR_COPY) && (curAssertion->op2.lclNum == lclNum)));
                if (verbose)
                {
                    printf("\nThe store ");
                    printTreeID(tree);
                    printf(" using V%02u removes: ", curAssertion->op1.lclNum);
                    optPrintAssertion(curAssertion, index);
                }
            }

            index--;
        }
    }
#endif

    BitVecOps::DiffD(apTraits, apLocal, killed);
    BitVecOps::DiffD(apTraits, apLocalPostorder, killed);
}

//------------------------------------------------------------------------
// fgKillDependentAssertions: Kill all dependent assertions with regard to lclNum.
//
// Arguments:
//    lclNum - The varNum of the lclVar for which we're killing assertions.
//    tree   - (DEBUG only) the tree responsible for killing its assertions.
//
// Notes:
//    For structs and struct fields, it will invalidate the children and parent
//    respectively.
//    Calls fgKillDependentAssertionsSingle to kill the assertions for a single lclVar.
//
void Compiler::fgKillDependentAssertions(unsigned lclNum DEBUGARG(GenTree* tree))
{
    if (BitVecOps::IsEmpty(apTraits, apLocal) && BitVecOps::IsEmpty(apTraits, apLocalPostorder))
    {
        return;
    }

    LclVarDsc* const varDsc = lvaGetDesc(lclNum);

    if (varDsc->lvPromoted)
    {
        noway_assert(varTypeIsStruct(varDsc));

        // Kill the field locals.
        for (unsigned i = varDsc->lvFieldLclStart; i < varDsc->lvFieldLclStart + varDsc->lvFieldCnt; ++i)
        {
            fgKillDependentAssertionsSingle(i DEBUGARG(tree));
        }

        // Kill the struct local itself.
        fgKillDependentAssertionsSingle(lclNum DEBUGARG(tree));
    }
    else if (varDsc->lvIsStructField)
    {
        // Kill the field local.
        fgKillDependentAssertionsSingle(lclNum DEBUGARG(tree));

        // Kill the parent struct.
        fgKillDependentAssertionsSingle(varDsc->lvParentLcl DEBUGARG(tree));
    }
    else
    {
        fgKillDependentAssertionsSingle(lclNum DEBUGARG(tree));
    }
}

//------------------------------------------------------------------------
// fgAssertionGen: generate local assertions for morphed tree
//
// Arguments:
//   tree - tree to examine for local assertions
//
// Notes:
//   wraps optAssertionGen to work with local assertion prop
//
void Compiler::fgAssertionGen(GenTree* tree)
{
    assert(optLocalAssertionProp);
    INDEBUG(unsigned oldAssertionCount = optAssertionCount;);
    optAssertionGen(tree);

    // Helper to note when an existing assertion has been
    // brought back to life.
    //
    auto announce = [&](AssertionIndex apIndex, const char* condition) {
#ifdef DEBUG
        if (verbose)
        {
            if (oldAssertionCount == optAssertionCount)
            {
                if (!BitVecOps::IsMember(apTraits, apLocal, apIndex - 1))
                {
                    // This tree resurrected an existing assertion.
                    // We call that out here since assertion prop won't.
                    //
                    printf("GenTreeNode creates %sassertion:\n", condition);
                    gtDispTree(tree, nullptr, nullptr, true);
                    printf("In " FMT_BB " New Local ", compCurBB->bbNum);
                    optPrintAssertion(optGetAssertion(apIndex), apIndex);
                }
                else
                {
                    // This tree re-asserted an already live assertion
                }
            }
            else
            {
                // This tree has created a new assertion.
                // Assertion prop will have already described it.
            }
        }
#endif
    };

    // If this tree creates an assignment of 0 or 1 to an int local, also create a [0..1] subrange
    // assertion for that local, in case this local is used as a bool.
    //
    auto addImpliedAssertions = [=](AssertionIndex index, ASSERT_TP& assertions) {
        AssertionDsc* const assertion = optGetAssertion(index);
        if ((assertion->assertionKind == OAK_EQUAL) && (assertion->op1.kind == O1K_LCLVAR) &&
            (assertion->op2.kind == O2K_CONST_INT))
        {
            LclVarDsc* const lclDsc = lvaGetDesc(assertion->op1.lclNum);

            if (varTypeIsIntegral(lclDsc->TypeGet()))
            {
                ssize_t iconVal = assertion->op2.u1.iconVal;
                if ((iconVal == 0) || (iconVal == 1))
                {
                    AssertionDsc extraAssertion = {OAK_SUBRANGE};
                    extraAssertion.op1.kind     = O1K_LCLVAR;
                    extraAssertion.op1.lclNum   = assertion->op1.lclNum;
                    extraAssertion.op2.kind     = O2K_SUBRANGE;
                    extraAssertion.op2.u2       = IntegralRange(SymbolicIntegerValue::Zero, SymbolicIntegerValue::One);

                    AssertionIndex extraIndex = optFinalizeCreatingAssertion(&extraAssertion);
                    if (extraIndex != NO_ASSERTION_INDEX)
                    {
                        unsigned const bvIndex = extraIndex - 1;
                        BitVecOps::AddElemD(apTraits, assertions, bvIndex);
                        announce(extraIndex, "[bool range] ");
                    }
                }
            }
        }
    };

    // For BBJ_COND nodes, we have two assertion out BVs.
    // apLocal will be stored on bbAssertionOutIfFalse and be used for false successors.
    // apLocalIfTrue will be stored on bbAssertionOutIfTrue and be used for true successors.
    //
    const bool makeCondAssertions =
        tree->OperIs(GT_JTRUE) && compCurBB->KindIs(BBJ_COND) && (compCurBB->NumSucc() == 2);

    // Initialize apLocalIfTrue if we might look for it later,
    // even if it ends up identical to apLocal.
    //
    if (makeCondAssertions)
    {
        apLocalIfTrue = BitVecOps::MakeCopy(apTraits, apLocal);
    }

    if (!tree->GeneratesAssertion())
    {
        return;
    }

    AssertionInfo info = tree->GetAssertionInfo();

    if (makeCondAssertions)
    {
        // Update apLocal and apLocalIfTrue with suitable assertions
        // from the JTRUE
        //
        assert(optCrossBlockLocalAssertionProp);

        AssertionIndex ifFalseAssertionIndex;
        AssertionIndex ifTrueAssertionIndex;

        if (info.AssertionHoldsOnFalseEdge())
        {
            ifFalseAssertionIndex = info.GetAssertionIndex();
            ifTrueAssertionIndex  = optFindComplementary(ifFalseAssertionIndex);
        }
        else
        {
            ifTrueAssertionIndex  = info.GetAssertionIndex();
            ifFalseAssertionIndex = optFindComplementary(ifTrueAssertionIndex);
        }

        if (ifTrueAssertionIndex != NO_ASSERTION_INDEX)
        {
            announce(ifTrueAssertionIndex, "[if true] ");
            unsigned const bvIndex = ifTrueAssertionIndex - 1;
            BitVecOps::AddElemD(apTraits, apLocalIfTrue, bvIndex);
            addImpliedAssertions(ifTrueAssertionIndex, apLocalIfTrue);
        }

        if (ifFalseAssertionIndex != NO_ASSERTION_INDEX)
        {
            announce(ifFalseAssertionIndex, "[if false] ");
            unsigned const bvIndex = ifFalseAssertionIndex - 1;
            BitVecOps::AddElemD(apTraits, apLocal, ifFalseAssertionIndex - 1);
            addImpliedAssertions(ifFalseAssertionIndex, apLocal);
        }
    }
    else
    {
        AssertionIndex const apIndex = tree->GetAssertionInfo().GetAssertionIndex();
        announce(apIndex, "");
        unsigned const bvIndex = apIndex - 1;
        BitVecOps::AddElemD(apTraits, apLocal, bvIndex);
        addImpliedAssertions(apIndex, apLocal);
    }
}

//------------------------------------------------------------------------
// fgMorphTreeDone: complete the morphing of a tree node
//
// Arguments:
//    tree - the tree after morphing
//
// Notes:
//    Simple version where assertion kill/gen has not yet been done.
//
void Compiler::fgMorphTreeDone(GenTree* tree)
{
    fgMorphTreeDone(tree, false);
}

//------------------------------------------------------------------------
// fgMorphTreeDone: complete the morphing of a tree node
//
// Arguments:
//   tree - the tree after morphing
//   optAssertionPropDone - true if local assertion prop was done already
//   isMorphedTree - true if caller should have marked tree as morphed
//   morphNum - counts invocations of fgMorphTree
//
// Notes:
//  This function is called to complete the morphing of a tree node.
//  It should only be called once for each node.
//
//  When local assertion prop is active assertions are killed and generated
//  based on tree (unless optAssertionPropDone is true).
//
void Compiler::fgMorphTreeDone(GenTree* tree, bool optAssertionPropDone DEBUGARG(int morphNum))
{
#ifdef DEBUG
    if (verbose && treesBeforeAfterMorph)
    {
        printf("\nfgMorphTree (after %d):\n", morphNum);
        gtDispTree(tree);
        printf(""); // in our logic this causes a flush
    }
#endif

    if (!fgGlobalMorph)
    {
        return;
    }

    tree->SetMorphed(this);

    // Note "tree" may generate new assertions that we
    // miss if we did them early... perhaps we should skip
    // kills but rerun gens.
    //
    if (tree->OperIsConst() || !optLocalAssertionProp || optAssertionPropDone)
    {
        return;
    }

    // Kill active assertions
    //
    if (optAssertionCount > 0)
    {
        auto visitDef = [=](GenTreeLclVarCommon* lcl) {
            fgKillDependentAssertions(lcl->GetLclNum() DEBUGARG(tree));
            return GenTree::VisitResult::Continue;
        };

        tree->VisitLocalDefNodes(this, visitDef);
    }

    // Generate assertions
    //
    fgAssertionGen(tree);
}

//------------------------------------------------------------------------
// fgFoldConditional: try and fold conditionals and optimize BBJ_COND or
//   BBJ_SWITCH blocks.
//
// Arguments:
//   block - block to examine
//
// Returns:
//   FoldResult indicating what changes were made, if any
//
Compiler::FoldResult Compiler::fgFoldConditional(BasicBlock* block)
{
    FoldResult result = FoldResult::FOLD_DID_NOTHING;

    // We don't want to make any code unreachable
    //
    if (opts.OptimizationDisabled())
    {
        return result;
    }

    if (block->KindIs(BBJ_COND))
    {
        noway_assert(block->bbStmtList != nullptr && block->bbStmtList->GetPrevStmt() != nullptr);

        Statement* lastStmt = block->lastStmt();

        noway_assert(lastStmt->GetNextStmt() == nullptr);

        if (lastStmt->GetRootNode()->OperIs(GT_CALL))
        {
            noway_assert(fgRemoveRestOfBlock);

            // Unconditional throw - transform the basic block into a BBJ_THROW
            //
            fgConvertBBToThrowBB(block);
            result = FoldResult::FOLD_CHANGED_CONTROL_FLOW;
            JITDUMP("\nConditional folded at " FMT_BB "\n", block->bbNum);
            JITDUMP(FMT_BB " becomes a BBJ_THROW\n", block->bbNum);

            return result;
        }

        noway_assert(lastStmt->GetRootNode()->OperIs(GT_JTRUE));

        /* Did we fold the conditional */

        noway_assert(lastStmt->GetRootNode()->AsOp()->gtOp1);
        GenTree* condTree;
        condTree = lastStmt->GetRootNode()->AsOp()->gtOp1;
        GenTree* cond;
        cond = condTree->gtEffectiveVal();

        if (cond->OperIsConst())
        {
            /* Yupee - we folded the conditional!
             * Remove the conditional statement */

            noway_assert(cond->OperIs(GT_CNS_INT));
            noway_assert((block->GetFalseTarget()->countOfInEdges() > 0) &&
                         (block->GetTrueTarget()->countOfInEdges() > 0));

            if (condTree != cond)
            {
                // Preserve any side effects
                assert(condTree->OperIs(GT_COMMA));
                lastStmt->SetRootNode(condTree);
                result = FoldResult::FOLD_ALTERED_LAST_STMT;
            }
            else
            {
                // no side effects, remove the jump entirely
                fgRemoveStmt(block, lastStmt);
                result = FoldResult::FOLD_REMOVED_LAST_STMT;
            }

            FlowEdge *retainedEdge, *removedEdge;

            if (cond->AsIntCon()->gtIconVal != 0)
            {
                retainedEdge = block->GetTrueEdge();
                removedEdge  = block->GetFalseEdge();
            }
            else
            {
                retainedEdge = block->GetFalseEdge();
                removedEdge  = block->GetTrueEdge();
            }

            fgRemoveRefPred(removedEdge);
            block->SetKindAndTargetEdge(BBJ_ALWAYS, retainedEdge);
            fgRepairProfileCondToUncond(block, retainedEdge, removedEdge);

            JITDUMP("\nConditional folded at " FMT_BB "\n", block->bbNum);
            JITDUMP(FMT_BB " becomes a %s", block->bbNum, "BBJ_ALWAYS");
            JITDUMP(" to " FMT_BB, block->GetTarget()->bbNum);
            JITDUMP("\n");
        }
    }
    else if (block->KindIs(BBJ_SWITCH))
    {
        noway_assert(block->bbStmtList != nullptr && block->bbStmtList->GetPrevStmt() != nullptr);

        Statement* lastStmt = block->lastStmt();

        noway_assert(lastStmt->GetNextStmt() == nullptr);

        if (lastStmt->GetRootNode()->OperIs(GT_CALL))
        {
            noway_assert(fgRemoveRestOfBlock);

            // Unconditional throw - transform the basic block into a BBJ_THROW
            //
            fgConvertBBToThrowBB(block);
            result = FoldResult::FOLD_CHANGED_CONTROL_FLOW;
            JITDUMP("\nConditional folded at " FMT_BB "\n", block->bbNum);
            JITDUMP(FMT_BB " becomes a BBJ_THROW\n", block->bbNum);

            return result;
        }

        noway_assert(lastStmt->GetRootNode()->OperIs(GT_SWITCH));

        // Did we fold the conditional

        noway_assert(lastStmt->GetRootNode()->AsOp()->gtOp1);
        GenTree* condTree = lastStmt->GetRootNode()->AsOp()->gtOp1;
        GenTree* cond     = condTree->gtEffectiveVal();

        if (cond->OperIsConst())
        {
            // Yupee - we folded the conditional!
            // Remove the conditional statement

            noway_assert(cond->OperIs(GT_CNS_INT));

            if (condTree != cond)
            {
                // Preserve any side effects
                assert(condTree->OperIs(GT_COMMA));
                lastStmt->SetRootNode(condTree);
                result = FoldResult::FOLD_ALTERED_LAST_STMT;
            }
            else
            {
                // no side effects, remove the switch entirely
                fgRemoveStmt(block, lastStmt);
                result = FoldResult::FOLD_REMOVED_LAST_STMT;
            }

            // modify the flow graph

            // Find the actual jump target
            size_t     switchVal           = (size_t)cond->AsIntCon()->gtIconVal;
            unsigned   jumpCnt             = block->GetSwitchTargets()->GetCaseCount();
            FlowEdge** jumpTab             = block->GetSwitchTargets()->GetCases();
            bool       foundVal            = false;
            bool       profileInconsistent = false;

            for (unsigned val = 0; val < jumpCnt; val++, jumpTab++)
            {
                FlowEdge* curEdge = *jumpTab;

                assert(curEdge->getDestinationBlock()->countOfInEdges() > 0);

                BasicBlock* const targetBlock = curEdge->getDestinationBlock();
                if (block->hasProfileWeight() && targetBlock->hasProfileWeight())
                {
                    targetBlock->decreaseBBProfileWeight(curEdge->getLikelyWeight());
                    profileInconsistent |= (targetBlock->NumSucc() > 0);
                }

                // If val matches switchVal or we are at the last entry and
                // we never found the switch value then set the new jump dest

                if ((val == switchVal) || (!foundVal && (val == jumpCnt - 1)))
                {
                    block->SetKindAndTargetEdge(BBJ_ALWAYS, curEdge);
                    foundVal = true;

                    if (block->hasProfileWeight() && targetBlock->hasProfileWeight())
                    {
                        targetBlock->increaseBBProfileWeight(block->bbWeight);
                        profileInconsistent |= (targetBlock->NumSucc() > 0);
                    }
                }
                else
                {
                    // Remove 'curEdge'
                    fgRemoveRefPred(curEdge);
                }
            }

            if (profileInconsistent)
            {
                JITDUMP("Flow change out of " FMT_BB " needs to be propagated. Data %s inconsistent.\n", block->bbNum,
                        fgPgoConsistent ? "is now" : "was already");
                fgPgoConsistent = false;
            }

            assert(foundVal);
#ifdef DEBUG
            if (verbose)
            {
                printf("\nConditional folded at " FMT_BB "\n", block->bbNum);
                printf(FMT_BB " becomes a %s", block->bbNum, "BBJ_ALWAYS");
                printf(" to " FMT_BB, block->GetTarget()->bbNum);
                printf("\n");
            }
#endif
        }
    }
    return result;
}

//------------------------------------------------------------------------
// fgMorphBlockStmt: morph a single statement in a block.
//
// Arguments:
//    block                       - block containing the statement
//    stmt                        - statement to morph
//    msg                         - string to identify caller in a dump
//    allowFGChange               - whether or not the flow graph can be changed
//    invalidateDFSTreeOnFGChange - whether or not the DFS tree should be invalidated
//                                  by this function if it makes a flow graph change
//
// Returns:
//    true if 'stmt' was removed from the block.
//    false if 'stmt' is still in the block (even if other statements were removed).
//
// Notes:
//   Can be called anytime, unlike fgMorphStmts() which should only be called once.
//
bool Compiler::fgMorphBlockStmt(BasicBlock*     block,
                                Statement* stmt DEBUGARG(const char* msg),
                                bool            allowFGChange,
                                bool            invalidateDFSTreeOnFGChange)
{
    assert(block != nullptr);
    assert(stmt != nullptr);

    // Reset some ambient state
    fgRemoveRestOfBlock = false;
    compCurBB           = block;
    compCurStmt         = stmt;

    GenTree* morph = fgMorphTree(stmt->GetRootNode());

    // Check for morph as a GT_COMMA with an unconditional throw
    if (fgIsCommaThrow(morph, true))
    {
#ifdef DEBUG
        if (verbose)
        {
            printf("Folding a top-level fgIsCommaThrow stmt\n");
            printf("Removing op2 as unreachable:\n");
            gtDispTree(morph->AsOp()->gtOp2);
            printf("\n");
        }
#endif
        // Use the call as the new stmt
        morph = morph->AsOp()->gtOp1;
        noway_assert(morph->OperIs(GT_CALL));
    }

    // we can get a throw as a statement root
    if (fgIsThrow(morph))
    {
#ifdef DEBUG
        if (verbose)
        {
            printf("We have a top-level fgIsThrow stmt\n");
            printf("Removing the rest of block as unreachable:\n");
        }
#endif
        noway_assert((morph->gtFlags & GTF_COLON_COND) == 0);
        fgRemoveRestOfBlock = true;
    }

    stmt->SetRootNode(morph);

    // Can the entire tree be removed?
    bool removedStmt = fgCheckRemoveStmt(block, stmt);

    // Or this is the last statement of a conditional branch that was just folded?
    if (allowFGChange && !removedStmt && (stmt->GetNextStmt() == nullptr) && !fgRemoveRestOfBlock)
    {
        FoldResult const fr = fgFoldConditional(block);
        if (invalidateDFSTreeOnFGChange && (fr != FoldResult::FOLD_DID_NOTHING))
        {
            fgInvalidateDfsTree();
        }
        removedStmt = (fr == FoldResult::FOLD_REMOVED_LAST_STMT);
    }

    if (!removedStmt)
    {
        // Have to re-do the evaluation order since for example some later code does not expect constants as op1
        gtSetStmtInfo(stmt);

        // This may be called both when the nodes are linked and when they aren't.
        if (fgNodeThreading == NodeThreading::AllTrees)
        {
            fgSetStmtSeq(stmt);
        }
    }

#ifdef DEBUG
    if (verbose)
    {
        printf("%s %s tree:\n", msg, (removedStmt ? "removed" : "morphed"));
        gtDispTree(morph);
        printf("\n");
    }
#endif

    if (fgRemoveRestOfBlock)
    {
        // Remove the rest of the stmts in the block
        for (Statement* removeStmt : StatementList(stmt->GetNextStmt()))
        {
            fgRemoveStmt(block, removeStmt);
        }

        // The rest of block has been removed and we will always throw an exception.
        //
        // For compDbgCode, we prepend an empty BB as the firstBB, it is BBJ_ALWAYS.
        // We should not convert it to a ThrowBB.
        if (allowFGChange && ((block != fgFirstBB) || !fgFirstBB->HasFlag(BBF_INTERNAL)))
        {
            // Convert block to a throw bb, or make it rarely run if already a throw.
            //
            const bool isThrow = block->KindIs(BBJ_THROW);
            fgConvertBBToThrowBB(block);

            if (!isThrow && invalidateDFSTreeOnFGChange)
            {
                fgInvalidateDfsTree();
            }
        }

#ifdef DEBUG
        if (verbose)
        {
            printf("\n%s Block " FMT_BB " becomes a throw block.\n", msg, block->bbNum);
        }
#endif
        fgRemoveRestOfBlock = false;
    }

    return removedStmt;
}

//------------------------------------------------------------------------
// fgMorphStmtBlockOps: Morph all block ops in the specified statement.
//
// Arguments:
//    stmt - the statement
//
void Compiler::fgMorphStmtBlockOps(BasicBlock* block, Statement* stmt)
{
    struct Visitor : GenTreeVisitor<Visitor>
    {
        enum
        {
            DoPostOrder = true,
        };

        Visitor(Compiler* comp)
            : GenTreeVisitor(comp)
        {
        }

        fgWalkResult PostOrderVisit(GenTree** use, GenTree* user)
        {
            if ((*use)->OperIsBlkOp())
            {
                if ((*use)->OperIsInitBlkOp())
                {
                    *use = m_compiler->fgMorphInitBlock(*use);
                }
                else
                {
                    *use = m_compiler->fgMorphCopyBlock(*use);
                }
            }

            return WALK_CONTINUE;
        }
    };

    compCurBB   = block;
    compCurStmt = stmt;
    Visitor visitor(this);
    visitor.WalkTree(stmt->GetRootNodePointer(), nullptr);

    gtSetStmtInfo(stmt);

    if (fgNodeThreading == NodeThreading::AllTrees)
    {
        fgSetStmtSeq(stmt);
    }
}

//------------------------------------------------------------------------
// fgMorphStmts: Morph all statements in a block
//
// Arguments:
//    block - block in question
//
void Compiler::fgMorphStmts(BasicBlock* block)
{
    fgRemoveRestOfBlock = false;
    fgHasNoReturnCall   = false;

    for (Statement* const stmt : block->Statements())
    {
        if (fgRemoveRestOfBlock)
        {
            fgRemoveStmt(block, stmt);
            continue;
        }

        fgMorphStmt      = stmt;
        compCurStmt      = stmt;
        GenTree* oldTree = stmt->GetRootNode();

        if (optLocalAssertionProp)
        {
            BitVecOps::Assign(apTraits, apLocalPostorder, apLocal);
        }

#ifdef DEBUG

        unsigned oldHash = verbose ? gtHashValue(oldTree) : DUMMY_INIT(~0);

        if (verbose)
        {
            printf("\nfgMorphTree " FMT_BB ", " FMT_STMT " (before)\n", block->bbNum, stmt->GetID());
            gtDispTree(oldTree);
        }
#endif

        /* Morph this statement tree */

        GenTree* morphedTree = fgMorphTree(oldTree);

        // Has fgMorphStmt been sneakily changed ?

        if ((stmt->GetRootNode() != oldTree) || (block != compCurBB))
        {
            if (stmt->GetRootNode() != oldTree)
            {
                /* This must be tailcall. Ignore 'morphedTree' and carry on with
                the tail-call node */

                morphedTree = stmt->GetRootNode();
            }

            noway_assert(compTailCallUsed);
            noway_assert(morphedTree->OperIs(GT_CALL));
            GenTreeCall* call = morphedTree->AsCall();
            // Could be
            //   - a fast call made as jmp in which case block will be ending with
            //   BBJ_RETURN (as we need epilog) and marked as containing a jmp.
            //   - a tailcall dispatched via JIT helper, on x86, in which case
            //   block will be ending with BBJ_THROW.
            //   - a tail call dispatched via runtime help (IL stubs), in which
            //   case there will not be any tailcall and the block will be ending
            //   with BBJ_RETURN (as normal control flow)
            noway_assert((call->IsFastTailCall() && compCurBB->KindIs(BBJ_RETURN) && compCurBB->HasFlag(BBF_HAS_JMP)) ||
                         (call->IsTailCallViaJitHelper() && compCurBB->KindIs(BBJ_THROW)) ||
                         (!call->IsTailCall() && compCurBB->KindIs(BBJ_RETURN)));
        }

#ifdef DEBUG
        if (compStressCompile(STRESS_CLONE_EXPR, 30))
        {
            // Clone all the trees to stress gtCloneExpr()

            if (verbose)
            {
                printf("\nfgMorphTree (stressClone from):\n");
                gtDispTree(morphedTree);
            }

            morphedTree = gtCloneExpr(morphedTree);
            noway_assert(morphedTree != nullptr);
            morphedTree->SetMorphed(this, /* doChildren*/ true);

            if (verbose)
            {
                printf("\nfgMorphTree (stressClone to):\n");
                gtDispTree(morphedTree);
            }
        }

        /* If the hash value changes. we modified the tree during morphing */
        if (verbose)
        {
            unsigned newHash = gtHashValue(morphedTree);
            if (newHash != oldHash)
            {
                printf("\nfgMorphTree " FMT_BB ", " FMT_STMT " (after)\n", block->bbNum, stmt->GetID());
                gtDispTree(morphedTree);
            }
        }
#endif

        /* Check for morphedTree as a GT_COMMA with an unconditional throw */
        if (fgIsCommaThrow(morphedTree, true))
        {
            /* Use the call as the new stmt */
            morphedTree = morphedTree->AsOp()->gtOp1;
            noway_assert(morphedTree->OperIs(GT_CALL));
            noway_assert((morphedTree->gtFlags & GTF_COLON_COND) == 0);

            fgRemoveRestOfBlock = true;
        }

        stmt->SetRootNode(morphedTree);

        if (fgHasNoReturnCall)
        {
            fgHasNoReturnCall = false;

            if ((fgGetTopLevelQmark(stmt->GetRootNode()) == nullptr) && gtRemoveTreesAfterNoReturnCall(block, stmt))
            {
                fgRemoveRestOfBlock = true;
                morphedTree         = stmt->GetRootNode();
            }
        }

        if (fgRemoveRestOfBlock)
        {
            continue;
        }

        /* Has the statement been optimized away */

        if (fgCheckRemoveStmt(block, stmt))
        {
            continue;
        }

        /* Check if this block ends with a conditional branch that can be folded */

        if (fgFoldConditional(block) != FoldResult::FOLD_DID_NOTHING)
        {
            continue;
        }

        if (ehBlockHasExnFlowDsc(block))
        {
            continue;
        }
    }

    if (fgRemoveRestOfBlock)
    {
        if (block->KindIs(BBJ_COND, BBJ_SWITCH))
        {
            Statement* first = block->firstStmt();
            noway_assert(first);
            Statement* lastStmt = block->lastStmt();
            noway_assert(lastStmt && lastStmt->GetNextStmt() == nullptr);
            GenTree* last = lastStmt->GetRootNode();

            if ((block->KindIs(BBJ_COND) && last->OperIs(GT_JTRUE)) ||
                (block->KindIs(BBJ_SWITCH) && last->OperIs(GT_SWITCH)))
            {
                GenTree* op1 = last->AsOp()->gtOp1;

                if (op1->OperIsCompare())
                {
                    /* Unmark the comparison node with GTF_RELOP_JMP_USED */
                    op1->gtFlags &= ~GTF_RELOP_JMP_USED;
                }

                lastStmt->SetRootNode(fgMorphTree(op1));
            }
        }

        /* Mark block as a BBJ_THROW block */
        fgConvertBBToThrowBB(block);
    }

#if FEATURE_FASTTAILCALL
    GenTreeCall* recursiveTailCall = nullptr;
    if (block->endsWithTailCallConvertibleToLoop(this, &recursiveTailCall))
    {
        fgMorphRecursiveFastTailCallIntoLoop(block, recursiveTailCall);
    }
#endif

    // Reset this back so that it doesn't leak out impacting other blocks
    fgRemoveRestOfBlock = false;
}

//------------------------------------------------------------------------
// MorphUnreachbleInfo: construct info for unreachability tracking during morph
//
// Arguments:
//    comp - compiler object
//
Compiler::MorphUnreachableInfo::MorphUnreachableInfo(Compiler* comp)
    : m_traits(comp->m_dfsTree->GetPostOrderCount(), comp)
    , m_vec(BitVecOps::MakeEmpty(&m_traits)){};

//------------------------------------------------------------------------
// SetUnreachable: during morph, mark a block as unreachable
//
// Arguments:
//    block - block in question
//
void Compiler::MorphUnreachableInfo::SetUnreachable(BasicBlock* block)
{
    BitVecOps::AddElemD(&m_traits, m_vec, block->bbPostorderNum);
}

//------------------------------------------------------------------------
// IsUnreachable: during morph, see if a block is now known to be unreachable
//
// Arguments:
//    block - block in question
//
// Returns:
//    true if so
//
bool Compiler::MorphUnreachableInfo::IsUnreachable(BasicBlock* block)
{
    return BitVecOps::IsMember(&m_traits, m_vec, block->bbPostorderNum);
}

//------------------------------------------------------------------------
// fgMorphBlock: Morph a basic block
//
// Arguments:
//    block - block in question
//    unreachableInfo - [optional] info on blocks proven unreachable
//
void Compiler::fgMorphBlock(BasicBlock* block, MorphUnreachableInfo* unreachableInfo)
{
    JITDUMP("\nMorphing " FMT_BB "\n", block->bbNum);

    if (optLocalAssertionProp)
    {
        if (!optCrossBlockLocalAssertionProp)
        {
            // Each block starts with an empty table, and no available assertions
            //
            optAssertionReset(0);
            BitVecOps::ClearD(apTraits, apLocal);
            BitVecOps::ClearD(apTraits, apLocalPostorder);
        }
        else
        {
            // Determine if this block can leverage assertions from its pred blocks.
            //
            // Some blocks are ineligible.
            //
            bool canUsePredAssertions = !block->HasFlag(BBF_CAN_ADD_PRED) && !bbIsHandlerBeg(block);

            // Validate all preds have valid info
            //
            if (!canUsePredAssertions)
            {
                JITDUMP(FMT_BB " ineligible for cross-block\n", block->bbNum);
            }
            else
            {
                bool hasPredAssertions = false;
                bool isReachable =
                    (block == fgFirstBB) || (block == genReturnBB) || (opts.IsOSR() && (block == fgEntryBB));

                for (BasicBlock* const pred : block->PredBlocks())
                {
                    assert(m_dfsTree->Contains(pred)); // We should have removed dead blocks before this.

                    // A smaller pred postorder number means the pred appears later in the reverse postorder.
                    // An equal number means pred == block (block is a self-loop).
                    // Either way the assertion info is not available, and we must assume the worst.
                    //
                    if (pred->bbPostorderNum <= block->bbPostorderNum)
                    {
                        JITDUMP(FMT_BB " pred " FMT_BB " not processed; clearing assertions in\n", block->bbNum,
                                pred->bbNum);
                        hasPredAssertions = false;

                        // Generally we must assume this pred will be reachable.
                        //
                        isReachable = true;
                        break;
                    }

                    // This pred was reachable in the pre-morph DFS, but might have
                    // become unreachable during morph. If so, we can ignore its assertion state.
                    //
                    if (unreachableInfo->IsUnreachable(pred))
                    {
                        JITDUMP("Pred " FMT_BB " is no longer reachable\n", pred->bbNum);
                        continue;
                    }

                    // Since we have a reachable pred, this blocks is also reachable.
                    //
                    isReachable = true;

                    // This pred is reachable and has available assertions.
                    // If the pred is (a non-degenerate) BBJ_COND, fetch the appropriate out set.
                    //
                    ASSERT_TP  assertionsOut;
                    const bool useCondAssertions = pred->KindIs(BBJ_COND) && (pred->NumSucc() == 2);

                    if (useCondAssertions)
                    {
                        if (block == pred->GetTrueTarget())
                        {
                            JITDUMP("Using `if true` assertions from pred " FMT_BB "\n", pred->bbNum);
                            assertionsOut = pred->bbAssertionOutIfTrue;
                        }
                        else
                        {
                            assert(block == pred->GetFalseTarget());
                            JITDUMP("Using `if false` assertions from pred " FMT_BB "\n", pred->bbNum);
                            assertionsOut = pred->bbAssertionOutIfFalse;
                        }
                    }
                    else
                    {
                        assertionsOut = pred->bbAssertionOut;
                    }

                    // If this is the first pred, copy (or share, when block is the only successor).
                    // If this is a subsequent pred, intersect.
                    //
                    if (!hasPredAssertions)
                    {
                        if (pred->NumSucc() == 1)
                        {
                            apLocal = assertionsOut;
                        }
                        else
                        {
                            apLocal = BitVecOps::MakeCopy(apTraits, assertionsOut);
                        }
                        hasPredAssertions = true;
                    }
                    else
                    {
                        BitVecOps::IntersectionD(apTraits, apLocal, assertionsOut);
                    }
                }

                if (!hasPredAssertions)
                {
                    // Either no preds, or some preds w/o assertions.
                    //
                    canUsePredAssertions = false;
                }

                // If there wasn't any reachable pred, this block is also not
                // reachable. Note we exclude handler entries above, since we don't
                // do the correct assertion tracking for handlers. Thus there is
                // no need to consider reachable EH preds.
                //
                if (!isReachable)
                {
                    JITDUMP(FMT_BB " has no reachable preds, marking as unreachable\n", block->bbNum);
                    unreachableInfo->SetUnreachable(block);

                    // Remove the block's IR and flow edges but don't mark the block as removed.
                    // Convert to BBJ_THROW. But leave CALLFINALLY(RET) alone.
                    //
                    // If we clear out the block, there is nothing to morph, so just return.
                    //
                    if (!block->KindIs(BBJ_CALLFINALLY, BBJ_CALLFINALLYRET))
                    {
                        fgUnreachableBlock(block);
                        block->RemoveFlags(BBF_REMOVED);
                        block->SetKindAndTargetEdge(BBJ_THROW);
                        return;
                    }
                }
            }

            if (!canUsePredAssertions)
            {
                apLocal = BitVecOps::MakeEmpty(apTraits);
            }

            BitVecOps::Assign(apTraits, apLocalPostorder, apLocal);

            JITDUMPEXEC(optDumpAssertionIndices("Assertions in: ", apLocal));
        }
    }

    // Make the current basic block address available globally.
    compCurBB = block;

    // Process all statement trees in the basic block.
    fgMorphStmts(block);

    // Do we need to merge the result of this block into a single return block?
    if (block->KindIs(BBJ_RETURN) && !block->HasFlag(BBF_HAS_JMP))
    {
        if ((genReturnBB != nullptr) && (genReturnBB != block))
        {
            fgMergeBlockReturn(block);
        }
    }

    // Publish the live out state.
    //
    if (optCrossBlockLocalAssertionProp && (block->NumSucc() > 0))
    {
        assert(optLocalAssertionProp);

        if (block->KindIs(BBJ_COND))
        {
            // We don't need to make a copy of the if true set; this BV
            // was freshly copied in fgAssertionGen
            //
            block->bbAssertionOutIfTrue  = apLocalIfTrue;
            block->bbAssertionOutIfFalse = BitVecOps::MakeCopy(apTraits, apLocal);
        }
        else
        {
            block->bbAssertionOut = BitVecOps::MakeCopy(apTraits, apLocal);
        }
    }

    compCurBB = nullptr;
}

//------------------------------------------------------------------------
// fgMorphBlocks: Morph all blocks in the method
//
// Returns:
//   Suitable phase status.
//
// Note:
//   Morph almost always changes IR, so we don't actually bother to
//   track if it made any changes.
//
PhaseStatus Compiler::fgMorphBlocks()
{
    // This is the one and only global morph phase
    //
    fgGlobalMorph = true;

    if (fgPgoConsistent)
    {
        Metrics.ProfileConsistentBeforeMorph = 1;
    }

    if (opts.OptimizationEnabled())
    {
        // Local assertion prop is enabled if we are optimizing.
        //
        optAssertionInit(/* isLocalProp*/ true);
        apLocal          = BitVecOps::MakeEmpty(apTraits);
        apLocalPostorder = BitVecOps::MakeEmpty(apTraits);
    }
    else
    {
        // Not optimizing. No assertion prop.
        //
        optLocalAssertionProp           = false;
        optCrossBlockLocalAssertionProp = false;
    }

    if (!compEnregLocals())
    {
        // Morph is checking if lvDoNotEnregister is already set for some optimizations.
        // If we are running without `CLFLG_REGVAR` flag set (`compEnregLocals() == false`)
        // then we already know that we won't enregister any locals and it is better to set
        // this flag before we start reading it.
        // The main reason why this flag is not set is that we are running in minOpts.
        lvSetMinOptsDoNotEnreg();
    }

    // Morph all blocks.
    //
    if (!optLocalAssertionProp)
    {
        // If we aren't optimizing, we just morph in normal bbNext order.
        //
        for (BasicBlock* block : Blocks())
        {
            fgMorphBlock(block);
        }
    }
    else
    {
        // Disallow general creation of new blocks or edges as it
        // would invalidate RPO.
        //
        // Removal of edges, or altering dup counts, is OK.
        //
        INDEBUG(fgSafeBasicBlockCreation = false;);
        INDEBUG(fgSafeFlowEdgeCreation = false;);

        // We will track which blocks become unreachable during morph
        //
        MorphUnreachableInfo unreachableInfo(this);

        // Allow edge creation to genReturnBB (target of return merging)
        // and the first IL BB (target for tail call to loop).
        // This will also disallow dataflow into these blocks.
        //
        if (genReturnBB != nullptr)
        {
            genReturnBB->SetFlags(BBF_CAN_ADD_PRED);
        }
        // TODO-Cleanup: Remove this by transforming tailcalls to loops earlier.
        BasicBlock* firstILBB = opts.IsOSR() ? fgEntryBB : fgGetFirstILBlock();
        firstILBB->SetFlags(BBF_CAN_ADD_PRED);

        // Remember this so we can sanity check that no new blocks will get created.
        //
        unsigned const bbNumMax = fgBBNumMax;

        // Morph the blocks in RPO.
        //
        for (unsigned i = m_dfsTree->GetPostOrderCount(); i != 0; i--)
        {
            BasicBlock* const block = m_dfsTree->GetPostOrder(i - 1);
            fgMorphBlock(block, &unreachableInfo);
        }
        assert(bbNumMax == fgBBNumMax);

        // Re-enable block and edge creation, and revoke
        // special treatment of genReturnBB and the "first" bb
        //
        INDEBUG(fgSafeBasicBlockCreation = true;);
        INDEBUG(fgSafeFlowEdgeCreation = true;);

        if (genReturnBB != nullptr)
        {
            genReturnBB->RemoveFlags(BBF_CAN_ADD_PRED);
        }
        firstILBB->RemoveFlags(BBF_CAN_ADD_PRED);
    }

    // Under OSR, we no longer need to specially protect the original method entry
    //
    if (opts.IsOSR() && (fgEntryBB != nullptr))
    {
        JITDUMP("OSR: un-protecting original method entry " FMT_BB "\n", fgEntryBB->bbNum);
        assert(fgEntryBBExtraRefs == 1);
        assert(fgEntryBB->bbRefs >= 1);
        fgEntryBB->bbRefs--;
        fgEntryBBExtraRefs = 0;

        // The original method entry will now be checked for profile consistency.
        // If the entry has inconsistent incoming weight, flag the profile as inconsistent.
        //
        if (fgEntryBB->hasProfileWeight())
        {
            const weight_t incomingWeight = fgEntryBB->computeIncomingWeight();
            if (!fgProfileWeightsConsistent(incomingWeight, fgEntryBB->bbWeight))
            {
                JITDUMP("OSR: Original method entry " FMT_BB " has inconsistent weight. Data %s inconsistent.\n",
                        fgEntryBB->bbNum, fgPgoConsistent ? "is now" : "was already");
                fgPgoConsistent = false;
            }
        }

        // We don't need to remember this block anymore.
        fgEntryBB = nullptr;
    }

    // We don't maintain `genReturnBB` after this point.
    if (genReturnBB != nullptr)
    {
        // It no longer needs special "keep" treatment.
        genReturnBB->RemoveFlags(BBF_DONT_REMOVE);
        genReturnBB = nullptr;
    }

    // We are done with the global morphing phase
    //
    fgInvalidateDfsTree();
    fgGlobalMorph     = false;
    fgGlobalMorphDone = true;
    compCurBB         = nullptr;

#ifdef DEBUG
    if (optLocalAssertionProp)
    {
        JITDUMP("morph assertion stats: %u table size, %u assertions, %u dropped\n", optMaxAssertionCount,
                optAssertionCount, optAssertionOverflow);
    }
#endif

    if (optLocalAssertionProp)
    {
        Metrics.LocalAssertionCount     = optAssertionCount;
        Metrics.LocalAssertionOverflow  = optAssertionOverflow;
        Metrics.MorphTrackedLocals      = lvaTrackedCount;
        Metrics.MorphLocals             = lvaCount;
        optLocalAssertionProp           = false;
        optCrossBlockLocalAssertionProp = false;
    }

    // We may have converted a tailcall into a loop, in which case the first BB
    // may no longer be canonical.
    fgCanonicalizeFirstBB();

    if (fgPgoConsistent)
    {
        Metrics.ProfileConsistentAfterMorph = 1;
    }

    INDEBUG(fgPostGlobalMorphChecks();)

    return PhaseStatus::MODIFIED_EVERYTHING;
}

#ifdef DEBUG

//------------------------------------------------------------------------
// fgPostGlobalMorphChecks: Make sure all nodes were morphed
//
void Compiler::fgPostGlobalMorphChecks()
{
    struct Visitor : GenTreeVisitor<Visitor>
    {
        enum
        {
            DoPostOrder = true,
        };

        Visitor(Compiler* comp)
            : GenTreeVisitor(comp)
        {
        }

        fgWalkResult PostOrderVisit(GenTree** use, GenTree* user)
        {
            assert((*use)->WasMorphed());
            assert((*use)->gtMorphCount <= 5);
            return WALK_CONTINUE;
        }
    };

    Visitor v(this);

    for (BasicBlock* const block : Blocks())
    {
        for (Statement* const stmt : block->Statements())
        {
            v.WalkTree(stmt->GetRootNodePointer(), nullptr);
        }
    }
}

#endif

//------------------------------------------------------------------------
// fgGetFirstILBB: Obtain the first basic block that was created due to IL.
//
// Returns:
//   The basic block, skipping the init BB.
//
// Remarks:
//   TODO-Cleanup: Refactor users to be able to remove this function.
//
BasicBlock* Compiler::fgGetFirstILBlock()
{
    BasicBlock* firstILBB = fgFirstBB;
    while (firstILBB->HasFlag(BBF_INTERNAL))
    {
        assert(firstILBB->KindIs(BBJ_ALWAYS));
        firstILBB = firstILBB->GetTarget();
        assert((firstILBB != nullptr) && (firstILBB != fgFirstBB));
    }

    return firstILBB;
}

//------------------------------------------------------------------------
// gtRemoveTreesAfterNoReturnCall:
//   Given a statement that may contain a no-return call, try to find it and
//   make it the root node of the statement. To do so extract all side effects
//   of nodes executed before the no-return call into separate statements, and
//   delete all nodes that would be executed after it.
//
// Returns:
//   block - The block containing the statement
//   stmt  - The statement that may contain a no-return call
//
// Returns:
//   True if a no-return call was found and made into the root of "stmt", with
//   new side effecting statements potentially created before it.
//
bool Compiler::gtRemoveTreesAfterNoReturnCall(BasicBlock* block, Statement* stmt)
{
    class Visitor final : public GenTreeVisitor<Visitor>
    {
        BasicBlock* m_bb;
        Statement*  m_stmt;

        struct UseInfo
        {
            GenTree** Use;
            GenTree*  User;
        };
        ArrayStack<UseInfo> m_useStack;

    public:
        enum
        {
            DoPreOrder        = true,
            DoPostOrder       = true,
            UseExecutionOrder = true
        };

        Visitor(Compiler* compiler, BasicBlock* bb, Statement* stmt)
            : GenTreeVisitor(compiler)
            , m_bb(bb)
            , m_stmt(stmt)
            , m_useStack(compiler->getAllocator(CMK_ArrayStack))
        {
        }

        fgWalkResult PreOrderVisit(GenTree** use, GenTree* user)
        {
            assert(!(*use)->OperIs(GT_QMARK));
            m_useStack.Push(UseInfo{use, user});
            return WALK_CONTINUE;
        }

        fgWalkResult PostOrderVisit(GenTree** use, GenTree* user)
        {
            if (!(*use)->IsCall() || !(*use)->AsCall()->IsNoReturn())
            {
                while (m_useStack.Top(0).Use != use)
                {
                    m_useStack.Pop();
                }

                return WALK_CONTINUE;
            }

            JITDUMP("Removing trees after no-return call [%06u]\n", Compiler::dspTreeID(*use));

            // Extract side effects of all siblings and ancestor's siblings.
            for (int i = 0; i < m_useStack.Height() - 1; i++)
            {
                const UseInfo& useInf = m_useStack.BottomRef(i);
                if (useInf.Use == use)
                {
                    // Got to the no-return call, future uses are its operands.
                    break;
                }

                // If this has the same user as the next node then it is a
                // sibling of an ancestor, and is thus not on the path that
                // contains the split node.
                if (m_useStack.BottomRef(i + 1).User == useInf.User)
                {
                    JITDUMP("Extracting side effects of (ancestor) sibling [%06u]:", Compiler::dspTreeID(*useInf.Use));

                    GenTree* sideEffects = nullptr;
                    m_compiler->gtExtractSideEffList(*useInf.Use, &sideEffects);
                    if (sideEffects != nullptr)
                    {
                        Statement* newStmt = m_compiler->fgNewStmtFromTree(sideEffects);
                        m_compiler->fgInsertStmtBefore(m_bb, m_stmt, newStmt);
                        JITDUMP("\n");
                        DISPSTMT(newStmt);
                    }
                    else
                    {
                        JITDUMP(" none\n");
                    }
                }
            }

            m_stmt->SetRootNode(*use);
            JITDUMP("New final statement:\n");
            DISPSTMT(m_stmt);

            return WALK_ABORT;
        }
    };

    Visitor visitor(this, block, stmt);
    return visitor.WalkTree(stmt->GetRootNodePointer(), nullptr) == WALK_ABORT;
}

//------------------------------------------------------------------------
// fgMergeBlockReturn: assign the block return value (if any) into the single return temp
//   and branch to the single return block.
//
// Arguments:
//   block - the block to process.
//
// Notes:
//   A block is not guaranteed to have a last stmt if its jump kind is BBJ_RETURN.
//   For example a method returning void could have an empty block with jump kind BBJ_RETURN.
//   Such blocks do materialize as part of in-lining.
//
//   A block with jump kind BBJ_RETURN does not necessarily need to end with GT_RETURN.
//   It could end with a tail call or rejected tail call or monitor.exit or a GT_INTRINSIC.
//   For now it is safe to explicitly check whether last stmt is GT_RETURN if genReturnLocal
//   is BAD_VAR_NUM.
//
void Compiler::fgMergeBlockReturn(BasicBlock* block)
{
    assert(block->KindIs(BBJ_RETURN) && !block->HasFlag(BBF_HAS_JMP));
    assert((genReturnBB != nullptr) && (genReturnBB != block));

    // TODO: Need to characterize the last top level stmt of a block ending with BBJ_RETURN.

    Statement* lastStmt = block->lastStmt();
    GenTree*   ret      = (lastStmt != nullptr) ? lastStmt->GetRootNode() : nullptr;

    if ((ret != nullptr) && ret->OperIs(GT_RETURN, GT_SWIFT_ERROR_RET) && ((ret->gtFlags & GTF_RET_MERGED) != 0))
    {
        // This return was generated during epilog merging, so leave it alone
    }
    else
    {
        // We'll jump to the genReturnBB.

#if !defined(TARGET_X86)
        if (info.compFlags & CORINFO_FLG_SYNCH)
        {
            fgConvertSyncReturnToLeave(block);
        }
        else
#endif // !TARGET_X86
        {
            FlowEdge* const newEdge = fgAddRefPred(genReturnBB, block);
            block->SetKindAndTargetEdge(BBJ_ALWAYS, newEdge);
            fgReturnCount--;
        }

#ifdef SWIFT_SUPPORT
        // If merging GT_SWIFT_ERROR_RET nodes, ensure the error operand is stored to the merged return error local,
        // so the correct error value is retrieved in the merged return block.
        if ((ret != nullptr) && ret->OperIs(GT_SWIFT_ERROR_RET))
        {
            assert(genReturnErrorLocal != BAD_VAR_NUM);
            const DebugInfo& di              = lastStmt->GetDebugInfo();
            GenTree*         swiftErrorStore = gtNewTempStore(genReturnErrorLocal, ret->gtGetOp1());
            swiftErrorStore->SetMorphed(this);
            Statement* const newStmt = gtNewStmt(swiftErrorStore, di);
            fgInsertStmtBefore(block, lastStmt, newStmt);
        }
#endif // SWIFT_SUPPORT

        if (genReturnLocal != BAD_VAR_NUM)
        {
            // replace the GT_RETURN/GT_SWIFT_ERROR_RET node to be a STORE_LCL_VAR that stores the return value into
            // genReturnLocal.

            // Method must be returning a value other than TYP_VOID.
            noway_assert(compMethodHasRetVal());

            // This block must be ending with a GT_RETURN/GT_SWIFT_ERROR_RET
            noway_assert(lastStmt != nullptr);
            noway_assert(lastStmt->GetNextStmt() == nullptr);
            noway_assert(ret != nullptr);

            // Return node must have non-null operand as the method is returning the value assigned to
            // genReturnLocal
            GenTree* const retVal = ret->AsOp()->GetReturnValue();
            noway_assert(retVal != nullptr);

            Statement*       pAfterStatement = lastStmt;
            const DebugInfo& di              = lastStmt->GetDebugInfo();
            GenTree* tree = gtNewTempStore(genReturnLocal, retVal, CHECK_SPILL_NONE, &pAfterStatement, di, block);
            // TODO: assertion gen/kill?
            tree->SetMorphed(this);
            if (tree->OperIsCopyBlkOp())
            {
                tree = fgMorphCopyBlock(tree);
            }
            else if (tree->OperIsInitBlkOp())
            {
                tree = fgMorphInitBlock(tree);
            }

            if (pAfterStatement == lastStmt)
            {
                lastStmt->SetRootNode(tree);
            }
            else
            {
                // gtNewTempStore inserted additional statements after last
                fgRemoveStmt(block, lastStmt);
                Statement* newStmt = gtNewStmt(tree, di);
                fgInsertStmtAfter(block, pAfterStatement, newStmt);
                lastStmt = newStmt;
            }
        }
        else if ((ret != nullptr) && ret->OperIs(GT_RETURN, GT_SWIFT_ERROR_RET))
        {
            // This block ends with a GT_RETURN/GT_SWIFT_ERROR_RET
            noway_assert(lastStmt != nullptr);
            noway_assert(lastStmt->GetNextStmt() == nullptr);

            // Must be a void return node with null operand; delete it as this block branches to
            // oneReturn block
            GenTree* const retVal = ret->AsOp()->GetReturnValue();
            noway_assert(ret->TypeIs(TYP_VOID));
            noway_assert(retVal == nullptr);

            if (opts.compDbgCode && lastStmt->GetDebugInfo().IsValid())
            {
                // We can't remove the return as it might remove a sequence point. Convert it to a NOP.
                ret->gtBashToNOP();
            }
            else
            {
                fgRemoveStmt(block, lastStmt);
            }
        }

        JITDUMP("\nUpdate " FMT_BB " to jump to common return block.\n", block->bbNum);
        DISPBLOCK(block);

        if (block->hasProfileWeight())
        {
            weight_t const oldWeight = genReturnBB->hasProfileWeight() ? genReturnBB->bbWeight : BB_ZERO_WEIGHT;
            weight_t const newWeight = oldWeight + block->bbWeight;

            JITDUMP("merging profile weight " FMT_WT " from " FMT_BB " to common return " FMT_BB "\n", block->bbWeight,
                    block->bbNum, genReturnBB->bbNum);

            genReturnBB->setBBProfileWeight(newWeight);
            DISPBLOCK(genReturnBB);
        }
    }
}

/*****************************************************************************
 *
 *  Make some decisions about the kind of code to generate.
 */

void Compiler::fgSetOptions()
{
#ifdef DEBUG
    /* Should we force fully interruptible code ? */
    if (JitConfig.JitFullyInt() || compStressCompile(STRESS_GENERIC_VARN, 30))
    {
        noway_assert(!codeGen->isGCTypeFixed());
        SetInterruptible(true);
    }
#endif

    if (opts.compDbgCode)
    {
        assert(!codeGen->isGCTypeFixed());
        SetInterruptible(true); // debugging is easier this way ...
    }

    /* Assume we won't need an explicit stack frame if this is allowed */

    if (compLocallocUsed)
    {
        codeGen->setFramePointerRequired(true);
    }

#ifdef TARGET_X86

    if (compTailCallUsed)
        codeGen->setFramePointerRequired(true);

#endif // TARGET_X86

    if (!opts.genFPopt)
    {
        codeGen->setFramePointerRequired(true);
    }

    // If there is EH, we need a frame pointer.
    // Note this may premature... we can eliminate all EH after morph, sometimes.
    //
    if (compHndBBtabCount > 0)
    {
        codeGen->setFramePointerRequiredEH(true);

#ifdef TARGET_X86
        if (UsesFunclets())
        {
            assert(!codeGen->isGCTypeFixed());
            // Enforce fully interruptible codegen for funclet unwinding
            SetInterruptible(true);
        }
#endif // TARGET_X86
    }

    if (compMethodRequiresPInvokeFrame())
    {
        codeGen->setFramePointerRequired(true); // Setup of Pinvoke frame currently requires an EBP style frame
    }

    if (info.compPublishStubParam)
    {
        codeGen->setFramePointerRequiredGCInfo(true);
    }

    if (compIsProfilerHookNeeded())
    {
        codeGen->setFramePointerRequired(true);
    }

    if (info.compIsVarArgs)
    {
        // Code that initializes lvaVarargsBaseOfStkArgs requires this to be EBP relative.
        codeGen->setFramePointerRequiredGCInfo(true);
    }

    if (lvaReportParamTypeArg())
    {
        codeGen->setFramePointerRequiredGCInfo(true);
    }

    // printf("method will %s be fully interruptible\n", GetInterruptible() ? "   " : "not");
}

/*****************************************************************************/

GenTree* Compiler::fgInitThisClass()
{
    noway_assert(!compIsForInlining());

    CORINFO_LOOKUP_KIND kind;
    info.compCompHnd->getLocationOfThisType(info.compMethodHnd, &kind);

    if (!kind.needsRuntimeLookup)
    {
        return fgGetSharedCCtor(info.compClassHnd);
    }
    else
    {
#ifdef FEATURE_READYTORUN
        // Only NativeAOT understands CORINFO_HELP_READYTORUN_GENERIC_STATIC_BASE. Don't do this on CoreCLR.
        if (IsNativeAot())
        {
            CORINFO_RESOLVED_TOKEN resolvedToken;
            memset(&resolvedToken, 0, sizeof(resolvedToken));

            // We are in a shared method body, but maybe we don't need a runtime lookup after all.
            // This covers the case of a generic method on a non-generic type.
            if (!(info.compClassAttr & CORINFO_FLG_SHAREDINST))
            {
                resolvedToken.hClass = info.compClassHnd;
                fgSetPreferredInitCctor();
                return impReadyToRunHelperToTree(&resolvedToken, m_preferredInitCctor, TYP_BYREF);
            }

            // We need a runtime lookup.
            GenTree* ctxTree = getRuntimeContextTree(kind.runtimeLookupKind);

            // CORINFO_HELP_READYTORUN_GENERIC_STATIC_BASE with a zeroed out resolvedToken means "get the static
            // base of the class that owns the method being compiled". If we're in this method, it means we're not
            // inlining and there's no ambiguity.
            return impReadyToRunHelperToTree(&resolvedToken, CORINFO_HELP_READYTORUN_GENERIC_STATIC_BASE, TYP_BYREF,
                                             &kind, ctxTree);
        }
#endif

        // Collectible types requires that for shared generic code, if we use the generic context parameter
        // that we report it. (This is a conservative approach, we could detect some cases particularly when the
        // context parameter is this that we don't need the eager reporting logic.)
        lvaGenericsContextInUse = true;

        switch (kind.runtimeLookupKind)
        {
            case CORINFO_LOOKUP_THISOBJ:
            {
                // This code takes a this pointer; but we need to pass the static method desc to get the right point in
                // the hierarchy
                GenTree* vtTree = gtNewLclvNode(info.compThisArg, TYP_REF);
                vtTree->gtFlags |= GTF_VAR_CONTEXT;
                // Vtable pointer of this object
                vtTree             = gtNewMethodTableLookup(vtTree);
                GenTree* methodHnd = gtNewIconEmbMethHndNode(info.compMethodHnd);

                return gtNewHelperCallNode(CORINFO_HELP_INITINSTCLASS, TYP_VOID, vtTree, methodHnd);
            }

            case CORINFO_LOOKUP_CLASSPARAM:
            {
                GenTree* vtTree = gtNewLclvNode(info.compTypeCtxtArg, TYP_I_IMPL);
                vtTree->gtFlags |= GTF_VAR_CONTEXT;
                return gtNewHelperCallNode(CORINFO_HELP_INITCLASS, TYP_VOID, vtTree);
            }

            case CORINFO_LOOKUP_METHODPARAM:
            {
                GenTree* methHndTree = gtNewLclvNode(info.compTypeCtxtArg, TYP_I_IMPL);
                methHndTree->gtFlags |= GTF_VAR_CONTEXT;
                return gtNewHelperCallNode(CORINFO_HELP_INITINSTCLASS, TYP_VOID, gtNewIconNode(0), methHndTree);
            }

            default:
                noway_assert(!"Unknown LOOKUP_KIND");
                UNREACHABLE();
        }
    }
}

#ifdef DEBUG

//------------------------------------------------------------------------
// fgPreExpandQmarkChecks: Verify that the importer has created GT_QMARK nodes
// in a way we can process them. The following
//
// Returns:
//    Suitable phase status.
//
// Remarks:
//   The following is allowed:
//   1. A top level qmark. Top level qmark is of the form:
//       a) (bool) ? (void) : (void) OR
//       b) V0N = (bool) ? (type) : (type)
//
//   2. Recursion is allowed at the top level, i.e., a GT_QMARK can be a child
//      of either op1 of colon or op2 of colon but not a child of any other
//      operator.
//
void Compiler::fgPreExpandQmarkChecks(GenTree* expr)
{
    GenTree* topQmark = fgGetTopLevelQmark(expr);

    // If the top level Qmark is null, then scan the tree to make sure
    // there are no qmarks within it.
    if (topQmark == nullptr)
    {
        assert(!gtTreeContainsOper(expr, GT_QMARK) && "Illegal QMARK");
    }
    else
    {
        // We could probably expand the cond node also, but don't think the extra effort is necessary,
        // so let's just assert the cond node of a top level qmark doesn't have further top level qmarks.
        assert(!gtTreeContainsOper(topQmark->gtGetOp1(), GT_QMARK) && "Illegal QMARK");

        fgPreExpandQmarkChecks(topQmark->gtGetOp2()->gtGetOp1());
        fgPreExpandQmarkChecks(topQmark->gtGetOp2()->gtGetOp2());
    }
}

//------------------------------------------------------------------------
// fgPostExpandQmarkChecks: Make sure we don't have any more GT_QMARK nodes.
//
void Compiler::fgPostExpandQmarkChecks()
{
    for (BasicBlock* const block : Blocks())
    {
        for (Statement* const stmt : block->Statements())
        {
            GenTree* expr = stmt->GetRootNode();
            assert(!gtTreeContainsOper(expr, GT_QMARK) && "QMARKs are disallowed beyond morph");
        }
    }
}

#endif // DEBUG

//------------------------------------------------------------------------
// fgGetTopLevelQmark:
//    Get the top level GT_QMARK node in a given expression.
//
// Arguments:
//    expr  - the tree, a root node that may contain a top level qmark.
//    ppDst - [optional] if the top level GT_QMARK node is stored into
//            a local, then this is that store node. Otherwise nullptr.
//
// Returns:
//    The GT_QMARK node, or nullptr if there is no top level qmark.
//
GenTreeQmark* Compiler::fgGetTopLevelQmark(GenTree* expr, GenTree** ppDst /* = NULL */)
{
    if (ppDst != nullptr)
    {
        *ppDst = nullptr;
    }

    GenTreeQmark* topQmark = nullptr;

    if (expr->OperIs(GT_QMARK))
    {
        topQmark = expr->AsQmark();
    }
    else if (expr->OperIsLocalStore() && expr->AsLclVarCommon()->Data()->OperIs(GT_QMARK))
    {
        topQmark = expr->AsLclVarCommon()->Data()->AsQmark();

        if (ppDst != nullptr)
        {
            *ppDst = expr;
        }
    }

    return topQmark;
}

//------------------------------------------------------------------------
// fgExpandQmarkStmt: expand a qmark into control flow
//
// Arguments:
//   block - block containing the qmark
//   stmt  - statement containing the qmark
//
// Returns:
//   true if the expansion introduced a throwing block
//
// Notes:
//
//  Expand a statement with a top level qmark node. There are three cases, based
//  on whether the qmark has both "true" and "false" arms, or just one of them.
//
//     S0;
//     C ? T : F;
//     S1;
//
//     Generates ===>
//
//                       bbj_always
//                       +---->------+
//                 false |           |
//     S0 -->-- ~C -->-- T   F -->-- S1
//              |            |
//              +--->--------+
//              bbj_cond(true)
//
//     -----------------------------------------
//
//     S0;
//     C ? T : NOP;
//     S1;
//
//     Generates ===>
//
//                 false
//     S0 -->-- ~C -->-- T -->-- S1
//              |                |
//              +-->-------------+
//              bbj_cond(true)
//
//     -----------------------------------------
//
//     S0;
//     C ? NOP : F;
//     S1;
//
//     Generates ===>
//
//                false
//     S0 -->-- C -->-- F -->-- S1
//              |               |
//              +-->------------+
//              bbj_cond(true)
//
//  If the qmark assigns to a variable, then create tmps for "then"
//  and "else" results and assign the temp to the variable as a writeback step.
//
bool Compiler::fgExpandQmarkStmt(BasicBlock* block, Statement* stmt)
{
    bool     introducedThrow = false;
    GenTree* expr            = stmt->GetRootNode();

    // Retrieve the Qmark node to be expanded.
    GenTree*      dst   = nullptr;
    GenTreeQmark* qmark = fgGetTopLevelQmark(expr, &dst);
    if (qmark == nullptr)
    {
        return false;
    }

#ifdef DEBUG
    if (verbose)
    {
        printf("\nExpanding top-level qmark in " FMT_BB " (before)\n", block->bbNum);
        fgDispBasicBlocks(block, block, true);
    }
#endif // DEBUG

    // Retrieve the operands.
    GenTree* condExpr  = qmark->gtGetOp1();
    GenTree* trueExpr  = qmark->gtGetOp2()->AsColon()->ThenNode();
    GenTree* falseExpr = qmark->gtGetOp2()->AsColon()->ElseNode();

    assert(!varTypeIsFloating(condExpr->TypeGet()));

    bool hasTrueExpr  = !trueExpr->OperIs(GT_NOP);
    bool hasFalseExpr = !falseExpr->OperIs(GT_NOP);
    assert(hasTrueExpr || hasFalseExpr); // We expect to have at least one arm of the qmark!

    // Create remainder, cond and "else" blocks. After this, the blocks are in this order:
    //     block ... condBlock ... elseBlock ... remainderBlock
    //
    // We need to remember flags that exist on 'block' that we want to propagate to 'remainderBlock',
    // if they are going to be cleared by fgSplitBlockAfterStatement(). We currently only do this only
    // for the GC safe point bit, the logic being that if 'block' was marked gcsafe, then surely
    // remainderBlock will still be GC safe.
    BasicBlockFlags propagateFlagsToRemainder = block->GetFlagsRaw() & BBF_GC_SAFE_POINT;
    // Conservatively propagate BBF_COPY_PROPAGATE flags to all blocks
    BasicBlockFlags propagateFlagsToAll = block->GetFlagsRaw() & BBF_COPY_PROPAGATE;
    BasicBlock*     remainderBlock      = fgSplitBlockAfterStatement(block, stmt);

    BasicBlock* condBlock = fgNewBBafter(BBJ_ALWAYS, block, true);
    BasicBlock* elseBlock = fgNewBBafter(BBJ_ALWAYS, condBlock, true);

    // Update flowgraph
    fgRedirectEdge(block->TargetEdgeRef(), condBlock);
    condBlock->SetTargetEdge(fgAddRefPred(elseBlock, condBlock));
    elseBlock->SetTargetEdge(fgAddRefPred(remainderBlock, elseBlock));

    // Propagate flow from block into condBlock.
    // Leave flow out of remainderBlock intact, as it will post-dominate block.
    condBlock->inheritWeight(block);

    // These blocks are only internal if 'block' is (but they've been set as internal by fgNewBBafter).
    // If they're not internal, mark them as imported to avoid asserts about un-imported blocks.
    if (!block->HasFlag(BBF_INTERNAL))
    {
        condBlock->RemoveFlags(BBF_INTERNAL);
        elseBlock->RemoveFlags(BBF_INTERNAL);
        condBlock->SetFlags(BBF_IMPORTED);
        elseBlock->SetFlags(BBF_IMPORTED);
    }

    block->RemoveFlags(BBF_NEEDS_GCPOLL);
    remainderBlock->SetFlags(propagateFlagsToRemainder | propagateFlagsToAll);

    condBlock->SetFlags(propagateFlagsToAll);
    elseBlock->SetFlags(propagateFlagsToAll);

    BasicBlock* thenBlock = nullptr;
    if (hasTrueExpr && hasFalseExpr)
    {
        //                       bbj_always
        //                       +---->------+
        //                 false |           |
        //     S0 -->-- ~C -->-- T   F -->-- S1
        //              |            |
        //              +--->--------+
        //              bbj_cond(true)
        //
        // TODO: Remove unnecessary condition reversal
        gtReverseCond(condExpr);

        thenBlock = fgNewBBafter(BBJ_ALWAYS, condBlock, true);
        thenBlock->SetFlags(propagateFlagsToAll);
        if (!block->HasFlag(BBF_INTERNAL))
        {
            thenBlock->RemoveFlags(BBF_INTERNAL);
            thenBlock->SetFlags(BBF_IMPORTED);
        }

        const unsigned thenLikelihood = qmark->ThenNodeLikelihood();
        const unsigned elseLikelihood = qmark->ElseNodeLikelihood();

        thenBlock->SetTargetEdge(fgAddRefPred(remainderBlock, thenBlock));

        assert(condBlock->TargetIs(elseBlock));
        FlowEdge* const thenEdge = fgAddRefPred(thenBlock, condBlock);
        FlowEdge* const elseEdge = condBlock->GetTargetEdge();
        condBlock->SetCond(elseEdge, thenEdge);
        thenBlock->inheritWeightPercentage(condBlock, thenLikelihood);
        elseBlock->inheritWeightPercentage(condBlock, elseLikelihood);
        thenEdge->setLikelihood(thenLikelihood / 100.0);
        elseEdge->setLikelihood(elseLikelihood / 100.0);
    }
    else if (hasTrueExpr)
    {
        //                 false
        //     S0 -->-- ~C -->-- T -->-- S1
        //              |                |
        //              +-->-------------+
        //              bbj_cond(true)
        //
        // TODO: Remove unnecessary condition reversal
        gtReverseCond(condExpr);

        const unsigned thenLikelihood = qmark->ThenNodeLikelihood();
        const unsigned elseLikelihood = qmark->ElseNodeLikelihood();

        assert(condBlock->TargetIs(elseBlock));
        FlowEdge* const thenEdge = fgAddRefPred(remainderBlock, condBlock);
        FlowEdge* const elseEdge = condBlock->GetTargetEdge();
        condBlock->SetCond(thenEdge, elseEdge);

        // Since we have no false expr, use the one we'd already created.
        thenBlock = elseBlock;
        elseBlock = nullptr;

        thenBlock->inheritWeightPercentage(condBlock, thenLikelihood);
        thenEdge->setLikelihood(thenLikelihood / 100.0);
        elseEdge->setLikelihood(elseLikelihood / 100.0);
    }
    else if (hasFalseExpr)
    {
        //                false
        //     S0 -->-- C -->-- F -->-- S1
        //              |               |
        //              +-->------------+
        //              bbj_cond(true)
        //
        const unsigned thenLikelihood = qmark->ThenNodeLikelihood();
        const unsigned elseLikelihood = qmark->ElseNodeLikelihood();

        assert(condBlock->TargetIs(elseBlock));
        FlowEdge* const thenEdge = fgAddRefPred(remainderBlock, condBlock);
        FlowEdge* const elseEdge = condBlock->GetTargetEdge();
        condBlock->SetCond(thenEdge, elseEdge);

        elseBlock->inheritWeightPercentage(condBlock, elseLikelihood);
        thenEdge->setLikelihood(thenLikelihood / 100.0);
        elseEdge->setLikelihood(elseLikelihood / 100.0);
    }

    assert(condBlock->KindIs(BBJ_COND));

    GenTree*   jmpTree = gtNewOperNode(GT_JTRUE, TYP_VOID, qmark->gtGetOp1());
    Statement* jmpStmt = fgNewStmtFromTree(jmpTree, stmt->GetDebugInfo());
    fgInsertStmtAtEnd(condBlock, jmpStmt);

    // Remove the original qmark statement.
    fgRemoveStmt(block, stmt);

    // Since we have top level qmarks, we either have a dst for it in which case
    // we need to create tmps for true and falseExprs, else just don't bother assigning.
    unsigned dstLclNum = BAD_VAR_NUM;
    if (dst != nullptr)
    {
        dstLclNum = dst->AsLclVarCommon()->GetLclNum();
        assert(dst->OperIsLocalStore());
    }
    else
    {
        assert(qmark->TypeIs(TYP_VOID));
    }

    if (hasTrueExpr)
    {
        if (trueExpr->OperIs(GT_CALL) && trueExpr->AsCall()->IsNoReturn())
        {
            Statement* trueStmt = fgNewStmtFromTree(trueExpr, stmt->GetDebugInfo());
            fgInsertStmtAtEnd(thenBlock, trueStmt);
            fgConvertBBToThrowBB(thenBlock);
            introducedThrow = true;
        }
        else
        {
            if (dst != nullptr)
            {
                trueExpr = dst->OperIs(GT_STORE_LCL_FLD) ? gtNewStoreLclFldNode(dstLclNum, dst->TypeGet(),
                                                                                dst->AsLclFld()->GetLclOffs(), trueExpr)
                                                         : gtNewStoreLclVarNode(dstLclNum, trueExpr)->AsLclVarCommon();
            }
            Statement* trueStmt = fgNewStmtFromTree(trueExpr, stmt->GetDebugInfo());
            fgInsertStmtAtEnd(thenBlock, trueStmt);
        }
    }

    // Assign the falseExpr into the dst or tmp, insert in elseBlock
    if (hasFalseExpr)
    {
        if (falseExpr->OperIs(GT_CALL) && falseExpr->AsCall()->IsNoReturn())
        {
            Statement* falseStmt = fgNewStmtFromTree(falseExpr, stmt->GetDebugInfo());
            fgInsertStmtAtEnd(elseBlock, falseStmt);
            fgConvertBBToThrowBB(elseBlock);
            introducedThrow = true;
        }
        else
        {
            if (dst != nullptr)
            {
                falseExpr =
                    dst->OperIs(GT_STORE_LCL_FLD)
                        ? gtNewStoreLclFldNode(dstLclNum, dst->TypeGet(), dst->AsLclFld()->GetLclOffs(), falseExpr)
                        : gtNewStoreLclVarNode(dstLclNum, falseExpr)->AsLclVarCommon();
            }
            Statement* falseStmt = fgNewStmtFromTree(falseExpr, stmt->GetDebugInfo());
            fgInsertStmtAtEnd(elseBlock, falseStmt);
        }
    }

#ifdef DEBUG
    if (verbose)
    {
        printf("\nExpanding top-level qmark in " FMT_BB " (after)\n", block->bbNum);
        fgDispBasicBlocks(block, remainderBlock, true);
    }
#endif // DEBUG

    return introducedThrow;
}

/*****************************************************************************
 *
 *  Expand GT_QMARK nodes from the flow graph into basic blocks.
 *
 */

void Compiler::fgExpandQmarkNodes()
{
    bool introducedThrows = false;

    if (compQmarkUsed)
    {
        for (BasicBlock* const block : Blocks())
        {
            for (Statement* const stmt : block->Statements())
            {
                GenTree* expr = stmt->GetRootNode();
#ifdef DEBUG
                fgPreExpandQmarkChecks(expr);
#endif
                introducedThrows |= fgExpandQmarkStmt(block, stmt);
            }
        }
#ifdef DEBUG
        fgPostExpandQmarkChecks();
#endif
    }
    compQmarkRationalized = true;

    // TODO: if qmark expansion created throw blocks, try and merge them
    //
    if (introducedThrows)
    {
        JITDUMP("Qmark expansion created new throw blocks\n");
    }
}

//------------------------------------------------------------------------
// fgPromoteStructs: promote structs to collections of per-field locals
//
// Returns:
//    Suitable phase status.
//
PhaseStatus Compiler::fgPromoteStructs()
{
    if (!opts.OptEnabled(CLFLG_STRUCTPROMOTE))
    {
        JITDUMP("  promotion opt flag not enabled\n");
        return PhaseStatus::MODIFIED_NOTHING;
    }

    if (fgNoStructPromotion)
    {
        JITDUMP("  promotion disabled by JitNoStructPromotion\n");
        return PhaseStatus::MODIFIED_NOTHING;
    }

#ifdef DEBUG
    if (compStressCompile(STRESS_NO_OLD_PROMOTION, 10))
    {
        JITDUMP("  skipping due to stress\n");
        return PhaseStatus::MODIFIED_NOTHING;
    }
#endif

    if (info.compIsVarArgs)
    {
        JITDUMP("  promotion disabled because of varargs\n");
        return PhaseStatus::MODIFIED_NOTHING;
    }

#ifdef DEBUG
    if (verbose)
    {
        printf("\nlvaTable before fgPromoteStructs\n");
        lvaTableDump();
    }
#endif // DEBUG

    // The lvaTable might grow as we grab temps. Make a local copy here.
    unsigned startLvaCount = lvaCount;

    //
    // Loop through the original lvaTable. Looking for struct locals to be promoted.
    //
    lvaStructPromotionInfo structPromotionInfo;
    bool                   tooManyLocalsReported = false;
    bool                   madeChanges           = false;

    // Clear the structPromotionHelper, since it is used during inlining, at which point it
    // may be conservative about looking up SIMD info.
    // We don't want to preserve those conservative decisions for the actual struct promotion.
    structPromotionHelper->Clear();

    for (unsigned lclNum = 0; lclNum < startLvaCount; lclNum++)
    {
        // Whether this var got promoted
        bool       promotedVar = false;
        LclVarDsc* varDsc      = lvaGetDesc(lclNum);

        // If we have marked this as lvUsedInSIMDIntrinsic, then we do not want to promote
        // its fields.  Instead, we will attempt to enregister the entire struct.
        if (varTypeIsSIMD(varDsc) && (varDsc->lvIsUsedInSIMDIntrinsic() || isOpaqueSIMDLclVar(varDsc)))
        {
            varDsc->lvRegStruct = true;
        }
        // Don't promote if we have reached the tracking limit.
        else if (lvaHaveManyLocals())
        {
            // Print the message first time when we detected this condition
            if (!tooManyLocalsReported)
            {
                JITDUMP("Stopped promoting struct fields, due to too many locals.\n");
            }
            tooManyLocalsReported = true;
        }
        else if (varTypeIsStruct(varDsc))
        {
            assert(structPromotionHelper != nullptr);
            promotedVar = structPromotionHelper->TryPromoteStructVar(lclNum);
        }

        madeChanges |= promotedVar;

        if (!promotedVar && varTypeIsSIMD(varDsc) && !varDsc->lvFieldAccessed)
        {
            // Even if we have not used this in a SIMD intrinsic, if it is not being promoted,
            // we will treat it as a reg struct.
            varDsc->lvRegStruct = true;
        }
    }

#ifdef DEBUG
    if (verbose && madeChanges)
    {
        printf("\nlvaTable after fgPromoteStructs\n");
        lvaTableDump();
    }
#endif // DEBUG

    return madeChanges ? PhaseStatus::MODIFIED_EVERYTHING : PhaseStatus::MODIFIED_NOTHING;
}

//------------------------------------------------------------------------
// fgMarkImplicitByRefCopyOmissionCandidates:
//   Find and mark all locals that are passed as implicit byref args and are
//   candidates for last-use copy omission.
//
// Remarks:
//   We must mark these locals beforehand to avoid potential reordering with
//   the call that ends up getting the address of the local. For example, if we
//   waited until morph it would be possible for morph to reorder the two
//   occurrences of V00 in
//
//     [000015] --CXG------      ▌  CALL      void   Program:Foo(int,int)
//     [000010] ----------- arg0 ├──▌  LCL_FLD   int    V00 loc0         [+0]
//     [000012] --CXG------ arg1 └──▌  CALL      int    Program:Bar(S):int
//     [000011] ----------- arg0    └──▌  LCL_VAR   struct<S, 32> V00 loc0          (last use)
//
//   to end up with
//
//     [000015] --CXG+-----             ▌  CALL      void   Program:Foo(int,int)
//     [000037] DACXG------ arg1 setup  ├──▌  STORE_LCL_VAR int    V04 tmp3
//     [000012] --CXG+-----             │  └──▌  CALL      int    Program:Bar(S):int
//     [000011] -----+----- arg0 in rcx │     └──▌  LCL_ADDR  long   V00 loc0         [+0]
//     [000038] ----------- arg1 in rdx ├──▌  LCL_VAR   int    V04 tmp3
//     [000010] -----+----- arg0 in rcx └──▌  LCL_FLD   int   (AX) V00 loc0         [+0]
//
//   If Bar mutates V00 then this is a problem.
//
// Returns:
//    Suitable phase status.
//
PhaseStatus Compiler::fgMarkImplicitByRefCopyOmissionCandidates()
{
#if FEATURE_IMPLICIT_BYREFS && !defined(UNIX_AMD64_ABI)
    if (!fgDidEarlyLiveness)
    {
        return PhaseStatus::MODIFIED_NOTHING;
    }

    struct Visitor : GenTreeVisitor<Visitor>
    {
        enum
        {
            DoPreOrder        = true,
            UseExecutionOrder = true,
        };

        Visitor(Compiler* comp)
            : GenTreeVisitor(comp)
        {
        }

        fgWalkResult PreOrderVisit(GenTree** use, GenTree* user)
        {
            GenTree* node = *use;
            if ((node->gtFlags & GTF_CALL) == 0)
            {
                return WALK_SKIP_SUBTREES;
            }

            if (!node->IsCall())
            {
                return WALK_CONTINUE;
            }

            GenTreeCall* call = node->AsCall();

            for (CallArg& arg : call->gtArgs.Args())
            {
                if (!varTypeIsStruct(arg.GetSignatureType()))
                {
                    continue;
                }

                GenTree* argNode = arg.GetNode()->gtEffectiveVal();
                if (!argNode->OperIsLocalRead())
                {
                    continue;
                }

                unsigned   lclNum = argNode->AsLclVarCommon()->GetLclNum();
                LclVarDsc* varDsc = m_compiler->lvaGetDesc(lclNum);

                if (varDsc->lvIsLastUseCopyOmissionCandidate)
                {
                    // Already a candidate.
                    continue;
                }

                if (varDsc->lvIsImplicitByRef)
                {
                    // While implicit byrefs are candidates, they are handled
                    // specially and do not need GTF_GLOB_REF (the indirections
                    // added on top already always get them). If we marked them
                    // as a candidate fgMorphLeafLocal would add GTF_GLOB_REF
                    // to the local containing the address, which is
                    // conservative.
                    continue;
                }

                if (varDsc->lvPromoted || varDsc->lvIsStructField || ((argNode->gtFlags & GTF_VAR_DEATH) == 0))
                {
                    // Not a candidate.
                    continue;
                }

                if (!call->gtArgs.IsAbiInformationDetermined())
                {
                    call->gtArgs.DetermineABIInfo(m_compiler, call);
                }

                if (!arg.AbiInfo.IsPassedByReference())
                {
                    continue;
                }

                JITDUMP("Marking V%02u as a candidate for last-use copy omission [%06u]\n", lclNum, dspTreeID(argNode));
                varDsc->lvIsLastUseCopyOmissionCandidate = 1;
            }

            return WALK_CONTINUE;
        }
    };

    Visitor visitor(this);
    for (BasicBlock* bb : Blocks())
    {
        for (Statement* stmt : bb->Statements())
        {
            // Does this have any calls?
            if ((stmt->GetRootNode()->gtFlags & GTF_CALL) == 0)
            {
                continue;
            }

            // If so, check for any struct last use and only do the expensive
            // tree walk if one exists.
            for (GenTreeLclVarCommon* lcl : stmt->LocalsTreeList())
            {
                if (!varTypeIsStruct(lcl) || !lcl->OperIsLocalRead())
                {
                    continue;
                }

                if ((lcl->gtFlags & GTF_VAR_DEATH) != 0)
                {
                    visitor.WalkTree(stmt->GetRootNodePointer(), nullptr);
                    break;
                }
            }
        }
    }
#endif

    return PhaseStatus::MODIFIED_NOTHING;
}

//------------------------------------------------------------------------
// fgRetypeImplicitByRefArgs: Update the types on implicit byref parameters' `LclVarDsc`s (from
//                            struct to pointer).  Also choose (based on address-exposed analysis)
//                            which struct promotions of implicit byrefs to keep or discard.
//                            For those which are kept, insert the appropriate initialization code.
//                            For those which are to be discarded, annotate the promoted field locals
//                            so that fgMorphExpandImplicitByRefArg will know to rewrite their
//                            appearances using indirections off the pointer parameters.
//
// Returns:
//    Suitable phase status
//
PhaseStatus Compiler::fgRetypeImplicitByRefArgs()
{
    bool madeChanges = false;

#if FEATURE_IMPLICIT_BYREFS

    for (unsigned lclNum = 0; lclNum < info.compArgsCount; lclNum++)
    {
        LclVarDsc* varDsc = lvaGetDesc(lclNum);

        if (lvaIsImplicitByRefLocal(lclNum))
        {
            madeChanges = true;

            if (varDsc->lvPromoted)
            {
                // This implicit-by-ref was promoted; create a new temp to represent the
                // promoted struct before rewriting this parameter as a pointer.
                unsigned newLclNum = lvaGrabTemp(false DEBUGARG("Promoted implicit byref"));
                // Update varDsc since lvaGrabTemp might have re-allocated the var dsc array.
                varDsc = lvaGetDesc(lclNum);

                lvaSetStruct(newLclNum, varDsc->GetLayout(), true);

                // Copy the struct promotion annotations to the new temp.
                LclVarDsc* newVarDsc       = lvaGetDesc(newLclNum);
                newVarDsc->lvPromoted      = true;
                newVarDsc->lvFieldLclStart = varDsc->lvFieldLclStart;
                newVarDsc->lvFieldCnt      = varDsc->lvFieldCnt;
                newVarDsc->lvContainsHoles = varDsc->lvContainsHoles;
#ifdef DEBUG
                newVarDsc->lvKeepType = true;
#endif // DEBUG

                // Propagate address-taken-ness and do-not-enregister-ness.
                newVarDsc->SetAddressExposed(varDsc->IsAddressExposed() DEBUGARG(varDsc->GetAddrExposedReason()));
                newVarDsc->lvDoNotEnregister       = varDsc->lvDoNotEnregister;
                newVarDsc->lvLiveInOutOfHndlr      = varDsc->lvLiveInOutOfHndlr;
                newVarDsc->lvSingleDef             = varDsc->lvSingleDef;
                newVarDsc->lvSingleDefRegCandidate = varDsc->lvSingleDefRegCandidate;
                newVarDsc->lvSpillAtSingleDef      = varDsc->lvSpillAtSingleDef;
#ifdef DEBUG
                newVarDsc->SetDoNotEnregReason(varDsc->GetDoNotEnregReason());
#endif // DEBUG

                // If the promotion is dependent, the promoted temp would just be committed
                // to memory anyway, so we'll rewrite its appearances to be indirections
                // through the pointer parameter, the same as we'd do for this
                // parameter if it weren't promoted at all (otherwise the initialization
                // of the new temp would just be a needless memcpy at method entry).
                //
                // Otherwise, see how many appearances there are. We keep two early ref counts: total
                // number of references to the struct or some field, and how many of these are
                // arguments to calls. We undo promotion unless we see enough non-call uses.
                //
                const unsigned totalAppearances = varDsc->lvRefCnt(RCS_EARLY);
                const unsigned callAppearances  = (unsigned)varDsc->lvRefCntWtd(RCS_EARLY);
                assert(totalAppearances >= callAppearances);
                const unsigned nonCallAppearances = totalAppearances - callAppearances;

                bool undoPromotion = ((lvaGetPromotionType(newVarDsc) == PROMOTION_TYPE_DEPENDENT) ||
                                      (nonCallAppearances <= varDsc->lvFieldCnt));

#ifdef DEBUG
                // Above is a profitability heuristic; either value of
                // undoPromotion should lead to correct code. So,
                // under stress, make different decisions at times.
                if (compStressCompile(STRESS_BYREF_PROMOTION, 25))
                {
                    undoPromotion = !undoPromotion;
                    JITDUMP("Stress -- changing byref undo promotion for V%02u to %s undo\n", lclNum,
                            undoPromotion ? "" : "NOT");
                }
#endif // DEBUG

                JITDUMP("%s promotion of implicit by-ref V%02u: %s total: %u non-call: %u fields: %u\n",
                        undoPromotion ? "Undoing" : "Keeping", lclNum,
                        (lvaGetPromotionType(newVarDsc) == PROMOTION_TYPE_DEPENDENT) ? "dependent;" : "",
                        totalAppearances, nonCallAppearances, varDsc->lvFieldCnt);

                if (!undoPromotion)
                {
                    // Insert IR that initializes the temp from the parameter.
                    // The first BB should already be a valid insertion point,
                    // which is a precondition for this phase when optimizing.
                    assert(fgFirstBB->bbPreds == nullptr);
                    GenTree* addr  = gtNewLclvNode(lclNum, TYP_BYREF);
                    GenTree* data  = varDsc->TypeIs(TYP_STRUCT) ? gtNewBlkIndir(varDsc->GetLayout(), addr)
                                                                : gtNewIndir(varDsc->TypeGet(), addr);
                    GenTree* store = gtNewStoreLclVarNode(newLclNum, data);
                    fgNewStmtAtBeg(fgFirstBB, store);
                }

                // Update the locals corresponding to the promoted fields.
                unsigned fieldLclStart = varDsc->lvFieldLclStart;
                unsigned fieldCount    = varDsc->lvFieldCnt;
                unsigned fieldLclStop  = fieldLclStart + fieldCount;

                for (unsigned fieldLclNum = fieldLclStart; fieldLclNum < fieldLclStop; ++fieldLclNum)
                {
                    LclVarDsc* fieldVarDsc = lvaGetDesc(fieldLclNum);

                    if (undoPromotion)
                    {
                        // Leave lvParentLcl pointing to the parameter so that fgMorphExpandImplicitByRefArg
                        // will know to rewrite appearances of this local.
                        assert(fieldVarDsc->lvParentLcl == lclNum);
                    }
                    else
                    {
                        // Set the new parent.
                        fieldVarDsc->lvParentLcl = newLclNum;
                    }

                    fieldVarDsc->lvIsParam = false;
                    // The fields shouldn't inherit any register preferences from
                    // the parameter which is really a pointer to the struct.
                    fieldVarDsc->lvIsRegArg      = false;
                    fieldVarDsc->lvIsMultiRegArg = false;
                    // Promoted fields of implicit byrefs can't be OSR locals.
                    //
                    if (fieldVarDsc->lvIsOSRLocal)
                    {
                        assert(opts.IsOSR());
                        fieldVarDsc->lvIsOSRLocal        = false;
                        fieldVarDsc->lvIsOSRExposedLocal = false;
                    }
                }

                // Hijack lvFieldLclStart to record the new temp number.
                // It will get fixed up in fgMarkDemotedImplicitByRefArgs.
                varDsc->lvFieldLclStart = newLclNum;
                // Go ahead and clear lvFieldCnt -- either we're promoting
                // a replacement temp or we're not promoting this arg, and
                // in either case the parameter is now a pointer that doesn't
                // have these fields.
                varDsc->lvFieldCnt = 0;

                // Hijack lvPromoted to communicate to fgMorphExpandImplicitByRefArg
                // whether references to the struct should be rewritten as
                // indirections off the pointer (not promoted) or references
                // to the new struct local (promoted).
                varDsc->lvPromoted = !undoPromotion;
            }
            else
            {
                // The "undo promotion" path above clears lvPromoted for args that struct
                // promotion wanted to promote but that aren't considered profitable to
                // rewrite.  It hijacks lvFieldLclStart to communicate to
                // fgMarkDemotedImplicitByRefArgs that it needs to clean up annotations left
                // on such args for fgMorphExpandImplicitByRefArg to consult in the interim.
                // Here we have an arg that was simply never promoted, so make sure it doesn't
                // have nonzero lvFieldLclStart, since that would confuse the aforementioned
                // functions.
                assert(varDsc->lvFieldLclStart == 0);
            }

            // Since the parameter in this position is really a pointer, its type is TYP_BYREF.
            varDsc->lvType = TYP_BYREF;

            // The struct parameter may have had its address taken, but the pointer parameter
            // cannot -- any uses of the struct parameter's address are uses of the pointer
            // parameter's value, and there's no way for the MSIL to reference the pointer
            // parameter's address.  So clear the address-taken bit for the parameter.
            varDsc->CleanAddressExposed();
            varDsc->lvDoNotEnregister = 0;

#ifdef DEBUG
            // This should not be converted to a double in stress mode,
            // because it is really a pointer
            varDsc->lvKeepType = 1;

            if (verbose)
            {
                printf("Changing the lvType for struct parameter V%02d to TYP_BYREF.\n", lclNum);
            }
#endif // DEBUG
        }
    }

#endif // FEATURE_IMPLICIT_BYREFS

    return madeChanges ? PhaseStatus::MODIFIED_EVERYTHING : PhaseStatus::MODIFIED_NOTHING;
}

//------------------------------------------------------------------------
// fgMarkDemotedImplicitByRefArgs: Clear annotations for any implicit byrefs that struct promotion
//                                 asked to promote.  Appearances of these have now been rewritten
//                                 (by fgMorphExpandImplicitByRefArg) using indirections from
//                                 the pointer parameter or references to the promotion temp, as
//                                 appropriate.
//
void Compiler::fgMarkDemotedImplicitByRefArgs()
{
    JITDUMP("\n*************** In fgMarkDemotedImplicitByRefArgs()\n");

#if FEATURE_IMPLICIT_BYREFS

    for (unsigned lclNum = 0; lclNum < info.compArgsCount; lclNum++)
    {
        LclVarDsc* varDsc = lvaGetDesc(lclNum);

        if (lvaIsImplicitByRefLocal(lclNum))
        {
            JITDUMP("Clearing annotation for V%02d\n", lclNum);

            if (varDsc->lvPromoted)
            {
                // The parameter is simply a pointer now, so clear lvPromoted.  It was left set by
                // fgRetypeImplicitByRefArgs to communicate to fgMorphExpandImplicitByRefArg that
                // appearances of this arg needed to be rewritten to a new promoted struct local.
                varDsc->lvPromoted = false;

                // Clear the lvFieldLclStart value that was set by fgRetypeImplicitByRefArgs
                // to tell fgMorphExpandImplicitByRefArg which local is the new promoted struct one.
                varDsc->lvFieldLclStart = 0;
            }
            else if (varDsc->lvFieldLclStart != 0)
            {
                // We created new temps to represent a promoted struct corresponding to this
                // parameter, but decided not to go through with the promotion and have
                // rewritten all uses as indirections off the pointer parameter.
                // We stashed the pointer to the new struct temp in lvFieldLclStart; make
                // note of that and clear the annotation.
                unsigned structLclNum   = varDsc->lvFieldLclStart;
                varDsc->lvFieldLclStart = 0;

                // The temp struct is now unused; set flags appropriately so that we
                // won't allocate space for it on the stack.
                LclVarDsc* structVarDsc = lvaGetDesc(structLclNum);
                structVarDsc->CleanAddressExposed();
#ifdef DEBUG
                structVarDsc->lvUnusedStruct          = true;
                structVarDsc->lvUndoneStructPromotion = true;
#endif // DEBUG

                unsigned fieldLclStart = structVarDsc->lvFieldLclStart;
                unsigned fieldCount    = structVarDsc->lvFieldCnt;
                unsigned fieldLclStop  = fieldLclStart + fieldCount;

                for (unsigned fieldLclNum = fieldLclStart; fieldLclNum < fieldLclStop; ++fieldLclNum)
                {
                    JITDUMP("Fixing pointer for field V%02d from V%02d to V%02d\n", fieldLclNum, lclNum, structLclNum);

                    // Fix the pointer to the parent local.
                    LclVarDsc* fieldVarDsc = lvaGetDesc(fieldLclNum);
                    assert(fieldVarDsc->lvParentLcl == lclNum);
                    fieldVarDsc->lvParentLcl = structLclNum;

                    // The field local is now unused; set flags appropriately so that
                    // we won't allocate stack space for it.
                    fieldVarDsc->CleanAddressExposed();
                }
            }
        }
    }

#endif // FEATURE_IMPLICIT_BYREFS
}

//------------------------------------------------------------------------
// fgCanTailCallViaJitHelper: check whether we can use the faster tailcall
// JIT helper on x86.
//
// Arguments:
//   call - the tailcall
//
// Return Value:
//    'true' if we can; or 'false' if we should use the generic tailcall mechanism.
//
bool Compiler::fgCanTailCallViaJitHelper(GenTreeCall* call)
{
#if !defined(TARGET_X86) || defined(UNIX_X86_ABI)
    // On anything except windows X86 we have no faster mechanism available.
    return false;
#else
    // For R2R make sure we go through portable mechanism that the 'EE' side
    // will properly turn into a runtime JIT.
    if (IsAot())
    {
        return false;
    }

    // The JIT helper does not properly handle the case where localloc was used.
    if (compLocallocUsed)
    {
        return false;
    }

    // Delegate calls may go through VSD stub in rare cases. Those look at the
    // call site so we cannot use the JIT helper.
    if (call->IsDelegateInvoke())
    {
        return false;
    }

    return true;
#endif
}

//------------------------------------------------------------------------
// fgMorphReduceAddOps: reduce successive variable adds into a single multiply,
// e.g., i + i + i + i => i * 4.
//
// Arguments:
//    tree - tree for reduction
//
// Return Value:
//    reduced tree if pattern matches, original tree otherwise
//
GenTree* Compiler::fgMorphReduceAddOps(GenTree* tree)
{
    // ADD(_, V0) starts the pattern match.
    if (!tree->OperIs(GT_ADD) || tree->gtOverflow())
    {
        return tree;
    }

#ifndef TARGET_64BIT
    // Transforming 64-bit ADD to 64-bit MUL on 32-bit system results in replacing
    // ADD ops with a helper function call. Don't apply optimization in that case.
    if (tree->TypeIs(TYP_LONG))
    {
        return tree;
    }
#endif

    GenTree* lclVarTree = tree->AsOp()->gtOp2;
    GenTree* consTree   = tree->AsOp()->gtOp1;

    GenTree* op1 = consTree;
    GenTree* op2 = lclVarTree;

    if (!op2->OperIs(GT_LCL_VAR) || !varTypeIsIntegral(op2))
    {
        return tree;
    }

    int      foldCount = 0;
    unsigned lclNum    = op2->AsLclVarCommon()->GetLclNum();

    // Search for pattern of shape ADD(ADD(ADD(lclNum, lclNum), lclNum), lclNum).
    while (true)
    {
        // ADD(lclNum, lclNum), end of tree
        if (op1->OperIs(GT_LCL_VAR) && op1->AsLclVarCommon()->GetLclNum() == lclNum && op2->OperIs(GT_LCL_VAR) &&
            op2->AsLclVarCommon()->GetLclNum() == lclNum)
        {
            foldCount += 2;
            break;
        }
        // ADD(ADD(X, Y), lclNum), keep descending
        else if (op1->OperIs(GT_ADD) && !op1->gtOverflow() && op2->OperIs(GT_LCL_VAR) &&
                 op2->AsLclVarCommon()->GetLclNum() == lclNum)
        {
            foldCount++;
            op2 = op1->AsOp()->gtOp2;
            op1 = op1->AsOp()->gtOp1;
        }
        // Any other case is a pattern we won't attempt to fold for now.
        else
        {
            return tree;
        }
    }

    // V0 + V0 ... + V0 becomes V0 * foldCount, where postorder transform will optimize
    // accordingly
    consTree->BashToConst(foldCount, tree->TypeGet());

    GenTree* morphed = gtNewOperNode(GT_MUL, tree->TypeGet(), lclVarTree, consTree);
    DEBUG_DESTROY_NODE(tree);

    return morphed;
}

//------------------------------------------------------------------------
// Compiler::MorphMDArrayTempCache::TempList::GetTemp: return a local variable number to use as a temporary variable
// in multi-dimensional array operation expansion.
//
// A temp is either re-used from the cache, or allocated and added to the cache.
//
// Returns:
//      A local variable temp number.
//
unsigned Compiler::MorphMDArrayTempCache::TempList::GetTemp()
{
    if (m_nextAvail != nullptr)
    {
        unsigned tmp = m_nextAvail->tmp;
        JITDUMP("Reusing temp V%02u\n", tmp);
        m_nextAvail = m_nextAvail->next;
        return tmp;
    }
    else
    {
        unsigned newTmp  = m_compiler->lvaGrabTemp(true DEBUGARG("MD array shared temp"));
        Node*    newNode = new (m_compiler, CMK_Unknown) Node(newTmp);
        assert(m_insertPtr != nullptr);
        assert(*m_insertPtr == nullptr);
        *m_insertPtr = newNode;
        m_insertPtr  = &newNode->next;
        return newTmp;
    }
}

//------------------------------------------------------------------------
// Compiler::MorphMDArrayTempCache::GrabTemp: return a local variable number to use as a temporary variable
// in multi-dimensional array operation expansion.
//
// Arguments:
//      type - type of temp to get
//
// Returns:
//      A local variable temp number.
//
unsigned Compiler::MorphMDArrayTempCache::GrabTemp(var_types type)
{
    switch (genActualType(type))
    {
        case TYP_INT:
            return intTemps.GetTemp();
        case TYP_REF:
            return refTemps.GetTemp();
        default:
            unreached();
    }
}

//------------------------------------------------------------------------
// fgMorphArrayOpsStmt: Tree walk a statement to morph GT_ARR_ELEM.
//
// The nested `MorphMDArrayVisitor::PostOrderVisit()` does the morphing.
//
// See the comment for `fgMorphArrayOps()` for more details of the transformation.
//
// Arguments:
//      pTempCache - pointer to the temp locals cache
//      block - BasicBlock where the statement lives
//      stmt - statement to walk
//
// Returns:
//      True if anything changed, false if the IR was unchanged.
//
bool Compiler::fgMorphArrayOpsStmt(MorphMDArrayTempCache* pTempCache, BasicBlock* block, Statement* stmt)
{
    class MorphMDArrayVisitor final : public GenTreeVisitor<MorphMDArrayVisitor>
    {
    public:
        enum
        {
            DoPostOrder = true
        };

        MorphMDArrayVisitor(Compiler* compiler, BasicBlock* block, MorphMDArrayTempCache* pTempCache)
            : GenTreeVisitor<MorphMDArrayVisitor>(compiler)
            , m_changed(false)
            , m_block(block)
            , m_pTempCache(pTempCache)
        {
        }

        bool Changed() const
        {
            return m_changed;
        }

        fgWalkResult PostOrderVisit(GenTree** use, GenTree* user)
        {
            GenTree* const node = *use;

            if (!node->OperIs(GT_ARR_ELEM))
            {
                return Compiler::WALK_CONTINUE;
            }

            GenTreeArrElem* const arrElem = node->AsArrElem();

            JITDUMP("Morphing GT_ARR_ELEM [%06u] in " FMT_BB " of '%s'\n", dspTreeID(arrElem), m_block->bbNum,
                    m_compiler->info.compFullName);
            DISPTREE(arrElem);

            // impArrayAccessIntrinsic() ensures the following.
            assert((2 <= arrElem->gtArrRank) && (arrElem->gtArrRank <= GT_ARR_MAX_RANK));
            assert(arrElem->gtArrObj->TypeIs(TYP_REF));
            assert(arrElem->TypeIs(TYP_BYREF));

            for (unsigned i = 0; i < arrElem->gtArrRank; i++)
            {
                assert(arrElem->gtArrInds[i] != nullptr);

                // We cast the index operands to TYP_INT in the importer.
                // Note that the offset calculation needs to be TYP_I_IMPL, as multiplying the linearized index
                // by the array element scale might overflow (although does .NET support array objects larger than
                // 2GB in size?).
                assert(genActualType(arrElem->gtArrInds[i]->TypeGet()) == TYP_INT);
            }

            // The order of evaluation of a[i,j,k] is: a, i, j, k. That is, if any of the i, j, k throw an
            // exception, it needs to happen before accessing `a`. For example, `a` could be null, but `i`
            // could be an expression throwing an exception, and that exception needs to be thrown before
            // indirecting using `a` (such as reading a dimension length or lower bound).
            //
            // First, we need to make temp copies of the index expressions that have side-effects. We
            // always make a copy of the array object (below) so we can multi-use it.
            //
            GenTree* idxToUse[GT_ARR_MAX_RANK];
            unsigned idxToCopy[GT_ARR_MAX_RANK];
            bool     anyIdxWithSideEffects = false;
            for (unsigned i = 0; i < arrElem->gtArrRank; i++)
            {
                GenTree* idx = arrElem->gtArrInds[i];
                if ((idx->gtFlags & GTF_ALL_EFFECT) == 0)
                {
                    // No side-effect; just use it.
                    idxToUse[i]  = idx;
                    idxToCopy[i] = BAD_VAR_NUM;
                }
                else
                {
                    // Side-effect; create a temp.
                    // unsigned newIdxLcl    = m_compiler->lvaGrabTemp(true DEBUGARG("MD array index copy"));
                    unsigned newIdxLcl    = m_pTempCache->GrabTemp(idx->TypeGet());
                    GenTree* newIdx       = m_compiler->gtNewLclvNode(newIdxLcl, genActualType(idx));
                    idxToUse[i]           = newIdx;
                    idxToCopy[i]          = newIdxLcl;
                    anyIdxWithSideEffects = true;
                }
            }

            // `newArrLcl` is set to the lclvar with a copy of the array object, if needed. The creation/copy of the
            // array object to this lcl is done as a top-level comma if needed.
            unsigned arrLcl    = BAD_VAR_NUM;
            unsigned newArrLcl = BAD_VAR_NUM;
            GenTree* arrObj    = arrElem->gtArrObj;
            unsigned rank      = arrElem->gtArrRank;

            // We are going to multiply reference the array object; create a new local var if necessary.
            if (arrObj->OperIs(GT_LCL_VAR))
            {
                arrLcl = arrObj->AsLclVar()->GetLclNum();
            }
            else
            {
                // arrLcl = newArrLcl = m_compiler->lvaGrabTemp(true DEBUGARG("MD array copy"));
                arrLcl = newArrLcl = m_pTempCache->GrabTemp(TYP_REF);
            }

            GenTree* fullTree = nullptr;

            // Work from outer-to-inner rank (i.e., slowest-changing to fastest-changing index), building up the offset
            // tree.
            for (unsigned i = 0; i < arrElem->gtArrRank; i++)
            {
                GenTree* idx = idxToUse[i];
                assert((idx->gtFlags & GTF_ALL_EFFECT) == 0); // We should have taken care of side effects earlier.

                GenTreeMDArr* const mdArrLowerBound =
                    m_compiler->gtNewMDArrLowerBound(m_compiler->gtNewLclvNode(arrLcl, TYP_REF), i, rank);
                // unsigned       effIdxLcl = m_compiler->lvaGrabTemp(true DEBUGARG("MD array effective index"));
                unsigned            effIdxLcl    = m_pTempCache->GrabTemp(TYP_INT);
                GenTree* const      effIndex     = m_compiler->gtNewOperNode(GT_SUB, TYP_INT, idx, mdArrLowerBound);
                GenTree* const      effIdxLclDef = m_compiler->gtNewTempStore(effIdxLcl, effIndex);
                GenTreeMDArr* const mdArrLength =
                    m_compiler->gtNewMDArrLen(m_compiler->gtNewLclvNode(arrLcl, TYP_REF), i, rank);
                GenTreeBoundsChk* const arrBndsChk = new (m_compiler, GT_BOUNDS_CHECK)
                    GenTreeBoundsChk(m_compiler->gtNewLclvNode(effIdxLcl, TYP_INT), mdArrLength, SCK_RNGCHK_FAIL);
                GenTree* const boundsCheckComma =
                    m_compiler->gtNewOperNode(GT_COMMA, TYP_INT, arrBndsChk,
                                              m_compiler->gtNewLclvNode(effIdxLcl, TYP_INT));
                GenTree* const idxComma = m_compiler->gtNewOperNode(GT_COMMA, TYP_INT, effIdxLclDef, boundsCheckComma);

                // If it's not the first index, accumulate with the previously created calculation.
                if (i > 0)
                {
                    assert(fullTree != nullptr);

                    GenTreeMDArr* const mdArrLengthScale =
                        m_compiler->gtNewMDArrLen(m_compiler->gtNewLclvNode(arrLcl, TYP_REF), i, rank);
                    GenTree* const scale    = m_compiler->gtNewOperNode(GT_MUL, TYP_INT, fullTree, mdArrLengthScale);
                    GenTree* const effIndex = m_compiler->gtNewOperNode(GT_ADD, TYP_INT, scale, idxComma);

                    fullTree = effIndex;
                }
                else
                {
                    fullTree = idxComma;
                }
            }

#ifdef TARGET_64BIT
            // Widen the linearized index on 64-bit targets; subsequent math will be done in TYP_I_IMPL.
            assert(fullTree->TypeIs(TYP_INT));
            fullTree = m_compiler->gtNewCastNode(TYP_I_IMPL, fullTree, true, TYP_I_IMPL);
#endif // TARGET_64BIT

            // Now scale by element size and add offset from array object to array data base.

            unsigned       elemScale  = arrElem->gtArrElemSize;
            unsigned       dataOffset = m_compiler->eeGetMDArrayDataOffset(arrElem->gtArrRank);
            GenTree* const scale =
                m_compiler->gtNewOperNode(GT_MUL, TYP_I_IMPL, fullTree,
                                          m_compiler->gtNewIconNode(static_cast<ssize_t>(elemScale), TYP_I_IMPL));
            GenTree* const scalePlusOffset =
                m_compiler->gtNewOperNode(GT_ADD, TYP_I_IMPL, scale,
                                          m_compiler->gtNewIconNode(static_cast<ssize_t>(dataOffset), TYP_I_IMPL));
            GenTree* fullExpansion = m_compiler->gtNewOperNode(GT_ADD, TYP_BYREF, scalePlusOffset,
                                                               m_compiler->gtNewLclvNode(arrLcl, TYP_REF));

            // Add copies of the index expressions with side effects. Add them in reverse order, so the first index
            // ends up at the top of the tree (so, first in execution order).
            if (anyIdxWithSideEffects)
            {
                for (unsigned i = arrElem->gtArrRank; i > 0; i--)
                {
                    if (idxToCopy[i - 1] != BAD_VAR_NUM)
                    {
                        GenTree* const idxLclStore =
                            m_compiler->gtNewTempStore(idxToCopy[i - 1], arrElem->gtArrInds[i - 1]);
                        fullExpansion =
                            m_compiler->gtNewOperNode(GT_COMMA, fullExpansion->TypeGet(), idxLclStore, fullExpansion);
                    }
                }
            }

            // If we needed to create a new local for the array object, copy that before everything.
            if (newArrLcl != BAD_VAR_NUM)
            {
                GenTree* const arrLclStore = m_compiler->gtNewTempStore(newArrLcl, arrObj);
                fullExpansion =
                    m_compiler->gtNewOperNode(GT_COMMA, fullExpansion->TypeGet(), arrLclStore, fullExpansion);
            }

            JITDUMP("fgMorphArrayOpsStmt (before remorph):\n");
            DISPTREE(fullExpansion);

            *use      = fullExpansion;
            m_changed = true;

            // The GT_ARR_ELEM node is no longer needed.
            DEBUG_DESTROY_NODE(node);

            return fgWalkResult::WALK_CONTINUE;
        }

    private:
        bool                   m_changed;
        BasicBlock*            m_block;
        MorphMDArrayTempCache* m_pTempCache;
    };

    MorphMDArrayVisitor morphMDArrayVisitor(this, block, pTempCache);
    morphMDArrayVisitor.WalkTree(stmt->GetRootNodePointer(), nullptr);
    return morphMDArrayVisitor.Changed();
}

//------------------------------------------------------------------------
// fgMorphArrayOps: Morph multi-dimensional (MD) array operations in this method.
//
// GT_ARR_ELEM nodes are morphed to appropriate trees. Note that MD array `Get`, `Set`, or `Address`
// is imported as a call, and, if all required conditions are satisfied, is treated as an intrinsic
// and replaced by IR nodes, especially GT_ARR_ELEM nodes, in impArrayAccessIntrinsic().
//
// For example, a simple 2-dimensional array access like `a[i,j]` looks like:
//
// \--*  ARR_ELEM[,] byref
//    +--*  LCL_VAR   ref    V00 arg0
//    +--*  LCL_VAR   int    V01 arg1
//    \--*  LCL_VAR   int    V02 arg2
//
// This is replaced by:
//
// &a + offset + elemSize * ((i - a.GetLowerBound(0)) * a.GetLength(1) + (j - a.GetLowerBound(1)))
//
// plus the appropriate `i` and `j` bounds checks.
//
// In IR, this is:
//
// *  ADD       byref
// +--*  ADD       long
// |  +--*  MUL       long
// |  |  +--*  CAST      long <- uint
// |  |  |  \--*  ADD       int
// |  |  |     +--*  MUL       int
// |  |  |     |  +--*  COMMA     int
// |  |  |     |  |  +--*  STORE_LCL_VAR   int    V04 tmp1
// |  |  |     |  |  |  \--*  SUB       int
// |  |  |     |  |  |     +--*  LCL_VAR   int    V01 arg1
// |  |  |     |  |  |     \--*  MDARR_LOWER_BOUND int    (0)
// |  |  |     |  |  |        \--*  LCL_VAR   ref    V00 arg0
// |  |  |     |  |  \--*  COMMA     int
// |  |  |     |  |     +--*  BOUNDS_CHECK_Rng void
// |  |  |     |  |     |  +--*  LCL_VAR   int    V04 tmp1
// |  |  |     |  |     |  \--*  MDARR_LENGTH int    (0)
// |  |  |     |  |     |     \--*  LCL_VAR   ref    V00 arg0
// |  |  |     |  |     \--*  LCL_VAR   int    V04 tmp1
// |  |  |     |  \--*  MDARR_LENGTH int    (1)
// |  |  |     |     \--*  LCL_VAR   ref    V00 arg0
// |  |  |     \--*  COMMA     int
// |  |  |        +--*  STORE_LCL_VAR   int    V05 tmp2
// |  |  |        |  \--*  SUB       int
// |  |  |        |     +--*  LCL_VAR   int    V02 arg2
// |  |  |        |     \--*  MDARR_LOWER_BOUND int    (1)
// |  |  |        |        \--*  LCL_VAR   ref    V00 arg0
// |  |  |        \--*  COMMA     int
// |  |  |           +--*  BOUNDS_CHECK_Rng void
// |  |  |           |  +--*  LCL_VAR   int    V05 tmp2
// |  |  |           |  \--*  MDARR_LENGTH int    (1)
// |  |  |           |     \--*  LCL_VAR   ref    V00 arg0
// |  |  |           \--*  LCL_VAR   int    V05 tmp2
// |  |  \--*  CNS_INT   long   4
// |  \--*  CNS_INT   long   32
// \--*  LCL_VAR   ref    V00 arg0
//
// before being morphed by the usual morph transformations.
//
// Some things to consider:
// 1. MD have both a lower bound and length for each dimension (even if very few MD arrays actually have a
//    lower bound)
// 2. GT_MDARR_LOWER_BOUND(dim) represents the lower-bound value for a particular array dimension. The "effective
//    index" for a dimension is the index minus the lower bound.
// 3. GT_MDARR_LENGTH(dim) represents the length value (number of elements in a dimension) for a particular
//    array dimension.
// 4. The effective index is bounds checked against the dimension length.
// 5. The lower bound and length values are 32-bit signed integers (TYP_INT).
// 6. After constructing a "linearized index", the index is scaled by the array element size, and the offset from
//    the array object to the beginning of the array data is added.
// 7. Much of the complexity above is simply to assign temps to the various values that are used subsequently.
// 8. The index expressions are used exactly once. However, if have side effects, they need to be copied, early,
//    to preserve exception ordering.
// 9. Only the top-level operation adds the array object to the scaled, linearized index, to create the final
//    address `byref`. As usual, we need to be careful to not create an illegal byref by adding any partial index.
//    calculation.
// 10. To avoid doing unnecessary work, the importer sets the global `OMF_HAS_MDARRAYREF` flag if there are any
//    MD array expressions to expand. Also, the block flag `BBF_HAS_MDARRAYREF` is set to blocks where these exist,
//    so only those blocks are processed.
//
// Returns:
//   suitable phase status
//
PhaseStatus Compiler::fgMorphArrayOps()
{
    if ((optMethodFlags & OMF_HAS_MDARRAYREF) == 0)
    {
        JITDUMP("No multi-dimensional array references in the function\n");
        return PhaseStatus::MODIFIED_NOTHING;
    }

    // Maintain a cache of temp locals to use when we need a temp for this transformation. After each statement,
    // reset the cache, meaning we can re-use any of the temps previously allocated. The idea here is to avoid
    // creating too many temporaries, since the JIT has a limit on the number of tracked locals. A temp created
    // here in one statement will have a distinct lifetime from a temp created in another statement, so register
    // allocation is not constrained.

    bool                  changed = false;
    MorphMDArrayTempCache mdArrayTempCache(this);

    for (BasicBlock* const block : Blocks())
    {
        if (!block->HasFlag(BBF_HAS_MDARRAYREF))
        {
            // No MD array references in this block
            continue;
        }

        // Publish current block (needed for various morphing functions).
        compCurBB = block;

        for (Statement* const stmt : block->Statements())
        {
            if (fgMorphArrayOpsStmt(&mdArrayTempCache, block, stmt))
            {
                changed = true;

                // Morph the statement if there have been changes.

                GenTree* tree        = stmt->GetRootNode();
                GenTree* morphedTree = fgMorphTree(tree);

                JITDUMP("fgMorphArrayOps (after remorph):\n");
                DISPTREE(morphedTree);

                stmt->SetRootNode(morphedTree);
            }
        }

        mdArrayTempCache.Reset();
    }

    return changed ? PhaseStatus::MODIFIED_EVERYTHING : PhaseStatus::MODIFIED_NOTHING;
}
