; Licensed to the .NET Foundation under one or more agreements.
; The .NET Foundation licenses this file to you under the MIT license.

include AsmMacros.inc
include asmconstants.inc

Thread_GetInterpThreadContext  TEXTEQU <?GetInterpThreadContext@Thread@@QEAAPEAUInterpThreadContext@@XZ>

extern PInvokeImportWorker:proc
extern ThePreStub:proc
extern  ProfileEnter:proc
extern  ProfileLeave:proc
extern  ProfileTailcall:proc
extern OnHijackWorker:proc
extern JIT_RareDisableHelperWorker:proc
ifdef FEATURE_INTERPRETER
extern ExecuteInterpretedMethod:proc
extern Thread_GetInterpThreadContext:proc
endif

extern g_pPollGC:QWORD
extern g_TrapReturningThreads:DWORD

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;
;; PInvokeImportThunk
;;
;; The call in PInvokeImportPrecode points to this function.
;;
; EXTERN_C VOID __stdcall PInvokeImportThunk();
NESTED_ENTRY PInvokeImportThunk, _TEXT

        ;
        ; Allocate space for XMM parameter registers and callee scratch area.
        ;
        alloc_stack     68h

        ;
        ; Save integer parameter registers.
        ; Make sure to preserve r11 as well as it is used to pass the stack argument size from JIT
        ;
        save_reg_postrsp    rcx, 70h
        save_reg_postrsp    rdx, 78h
        save_reg_postrsp    r8,  80h
        save_reg_postrsp    r9,  88h
        save_reg_postrsp    r11,  60h

        save_xmm128_postrsp xmm0, 20h
        save_xmm128_postrsp xmm1, 30h
        save_xmm128_postrsp xmm2, 40h
        save_xmm128_postrsp xmm3, 50h
    END_PROLOGUE

        ;
        ; Call PInvokeImportWorker w/ the PInvokeMethodDesc*
        ;
        mov             rcx, METHODDESC_REGISTER
        call            PInvokeImportWorker

        ;
        ; Restore parameter registers
        ;
        mov             rcx, [rsp + 70h]
        mov             rdx, [rsp + 78h]
        mov             r8,  [rsp + 80h]
        mov             r9,  [rsp + 88h]
        mov             r11, [rsp + 60h]
        movdqa          xmm0, [rsp + 20h]
        movdqa          xmm1, [rsp + 30h]
        movdqa          xmm2, [rsp + 40h]
        movdqa          xmm3, [rsp + 50h]

        ;
        ; epilogue, rax contains the native target address
        ;
        add             rsp, 68h

    TAILJMP_RAX
NESTED_END PInvokeImportThunk, _TEXT


;------------------------------------------------
; JIT_RareDisableHelper
;
; The JIT expects this helper to preserve all
; registers that can be used for return values
;

NESTED_ENTRY JIT_RareDisableHelper, _TEXT

    alloc_stack         38h
    END_PROLOGUE

    movdqa      [rsp+20h], xmm0     ; Save xmm0
    mov         [rsp+30h], rax      ; Save rax

    call        JIT_RareDisableHelperWorker

    movdqa      xmm0, [rsp+20h]     ; Restore xmm0
    mov         rax,  [rsp+30h]     ; Restore rax

    add         rsp, 38h
    ret

NESTED_END JIT_RareDisableHelper, _TEXT


; extern "C" void setFPReturn(int fpSize, INT64 retVal);
LEAF_ENTRY setFPReturn, _TEXT
        cmp     ecx, 4
        je      setFPReturn4
        cmp     ecx, 8
        jne     setFPReturnNot8
        mov     [rsp+10h], rdx
        movsd   xmm0, real8 ptr [rsp+10h]
setFPReturnNot8:
        REPRET

setFPReturn4:
        mov     [rsp+10h], rdx
        movss   xmm0, real4 ptr [rsp+10h]
        ret
LEAF_END setFPReturn, _TEXT


; extern "C" void getFPReturn(int fpSize, INT64 *retval);
LEAF_ENTRY getFPReturn, _TEXT
        cmp     ecx, 4
        je      getFPReturn4
        cmp     ecx, 8
        jne     getFPReturnNot8
        movsd   real8 ptr [rdx], xmm0
getFPReturnNot8:
        REPRET

getFPReturn4:
        movss   real4 ptr [rdx], xmm0
        ret
LEAF_END getFPReturn, _TEXT


; A JITted method's return address was hijacked to return to us here.
; VOID OnHijackTripThread()
NESTED_ENTRY OnHijackTripThread, _TEXT

        ; Don't fiddle with this unless you change HijackFrame::UpdateRegDisplay
        ; and HijackArgs
        mov                 rdx, rsp
        push                rax ; make room for the real return address (Rip)
        push                rdx
        PUSH_CALLEE_SAVED_REGISTERS
        push_vol_reg        rcx
        push_vol_reg        rax
        mov                 rcx, rsp

        alloc_stack         30h ; make extra room for xmm0 and argument home slots
        save_xmm128_postrsp xmm0, 20h


        END_PROLOGUE

        call                OnHijackWorker

        movdqa              xmm0, [rsp + 20h]

        add                 rsp, 30h
        pop                 rax
        pop                 rcx
        POP_CALLEE_SAVED_REGISTERS
        pop                 rdx
        ret                 ; return to the correct place, adjusted by our caller
NESTED_END OnHijackTripThread, _TEXT


;
;    typedef struct _PROFILE_PLATFORM_SPECIFIC_DATA
;    {
;        FunctionID *functionId; // function ID comes in the r11 register
;        void       *rbp;
;        void       *probersp;
;        void       *ip;
;        void       *profiledRsp;
;        UINT64      rax;
;        LPVOID      hiddenArg;
;        UINT64      flt0;
;        UINT64      flt1;
;        UINT64      flt2;
;        UINT64      flt3;
;        UINT32      flags;
;    } PROFILE_PLATFORM_SPECIFIC_DATA, *PPROFILE_PLATFORM_SPECIFIC_DATA;
;
SIZEOF_PROFILE_PLATFORM_SPECIFIC_DATA   equ 8h*11 + 4h*2    ; includes fudge to make FP_SPILL right
SIZEOF_OUTGOING_ARGUMENT_HOMES          equ 8h*4
SIZEOF_FP_ARG_SPILL                     equ 10h*1

; Need to be careful to keep the stack 16byte aligned here, since we are pushing 3
; arguments that will align the stack and we just want to keep it aligned with our
; SIZEOF_STACK_FRAME

OFFSETOF_PLATFORM_SPECIFIC_DATA         equ SIZEOF_OUTGOING_ARGUMENT_HOMES

; we'll just spill into the PROFILE_PLATFORM_SPECIFIC_DATA structure
OFFSETOF_FP_ARG_SPILL                   equ SIZEOF_OUTGOING_ARGUMENT_HOMES + \
                                            SIZEOF_PROFILE_PLATFORM_SPECIFIC_DATA

SIZEOF_STACK_FRAME                      equ SIZEOF_OUTGOING_ARGUMENT_HOMES + \
                                            SIZEOF_PROFILE_PLATFORM_SPECIFIC_DATA + \
                                            SIZEOF_MAX_FP_ARG_SPILL

PROFILE_ENTER                           equ 1h
PROFILE_LEAVE                           equ 2h
PROFILE_TAILCALL                        equ 4h

; ***********************************************************
;   NOTE:
;
;   Register preservation scheme:
;
;       Preserved:
;           - all non-volatile registers
;           - rax
;           - xmm0
;
;       Not Preserved:
;           - integer argument registers (rcx, rdx, r8, r9)
;           - floating point argument registers (xmm1-3)
;           - volatile integer registers (r10, r11)
;           - volatile floating point registers (xmm4-5)
;
; ***********************************************************

; void JIT_ProfilerEnterLeaveTailcallStub(UINT_PTR ProfilerHandle)
LEAF_ENTRY JIT_ProfilerEnterLeaveTailcallStub, _TEXT
        REPRET
LEAF_END JIT_ProfilerEnterLeaveTailcallStub, _TEXT

;EXTERN_C void ProfileEnterNaked(FunctionIDOrClientID functionIDOrClientID, size_t profiledRsp);
NESTED_ENTRY ProfileEnterNaked, _TEXT
        push_nonvol_reg         rax

;       Upon entry :
;           rcx = clientInfo
;           rdx = profiledRsp

        lea                     rax, [rsp + 10h]    ; caller rsp
        mov                     r10, [rax - 8h]     ; return address

        alloc_stack             SIZEOF_STACK_FRAME

        ; correctness of return value in structure doesn't matter for enter probe


        ; setup ProfilePlatformSpecificData structure
        xor                     r8, r8;
        mov                     [rsp + OFFSETOF_PLATFORM_SPECIFIC_DATA +  0h], r8     ; r8 is null      -- struct functionId field
        save_reg_postrsp        rbp, OFFSETOF_PLATFORM_SPECIFIC_DATA +    8h          ;                 -- struct rbp field
        mov                     [rsp + OFFSETOF_PLATFORM_SPECIFIC_DATA + 10h], rax    ; caller rsp      -- struct probeRsp field
        mov                     [rsp + OFFSETOF_PLATFORM_SPECIFIC_DATA + 18h], r10    ; return address  -- struct ip field
        mov                     [rsp + OFFSETOF_PLATFORM_SPECIFIC_DATA + 20h], rdx    ;                 -- struct profiledRsp field
        mov                     [rsp + OFFSETOF_PLATFORM_SPECIFIC_DATA + 28h], r8     ; r8 is null      -- struct rax field
        mov                     [rsp + OFFSETOF_PLATFORM_SPECIFIC_DATA + 30h], r8     ; r8 is null      -- struct hiddenArg field
        movsd                   real8 ptr [rsp + OFFSETOF_PLATFORM_SPECIFIC_DATA + 38h], xmm0    ;      -- struct flt0 field
        movsd                   real8 ptr [rsp + OFFSETOF_PLATFORM_SPECIFIC_DATA + 40h], xmm1    ;      -- struct flt1 field
        movsd                   real8 ptr [rsp + OFFSETOF_PLATFORM_SPECIFIC_DATA + 48h], xmm2    ;      -- struct flt2 field
        movsd                   real8 ptr [rsp + OFFSETOF_PLATFORM_SPECIFIC_DATA + 50h], xmm3    ;      -- struct flt3 field
        mov                     r10, PROFILE_ENTER
        mov                     [rsp + OFFSETOF_PLATFORM_SPECIFIC_DATA + 58h], r10d   ; flags    ;      -- struct flags field

        ; we need to be able to restore the fp return register
        save_xmm128_postrsp     xmm0, OFFSETOF_FP_ARG_SPILL +  0h
    END_PROLOGUE

        ; rcx already contains the clientInfo
        lea                     rdx, [rsp + OFFSETOF_PLATFORM_SPECIFIC_DATA]
        call                    ProfileEnter

        ; restore fp return register
        movdqa                  xmm0, [rsp + OFFSETOF_FP_ARG_SPILL +  0h]

        ; begin epilogue
        add                     rsp, SIZEOF_STACK_FRAME
        pop                     rax
        ret
NESTED_END ProfileEnterNaked, _TEXT

;EXTERN_C void ProfileLeaveNaked(FunctionIDOrClientID functionIDOrClientID, size_t profiledRsp);
NESTED_ENTRY ProfileLeaveNaked, _TEXT
        push_nonvol_reg         rax

;       Upon entry :
;           rcx = clientInfo
;           rdx = profiledRsp

        ; need to be careful with rax here because it contains the return value which we want to harvest

        lea                     r10, [rsp + 10h]    ; caller rsp
        mov                     r11, [r10 - 8h]     ; return address

        alloc_stack             SIZEOF_STACK_FRAME

        ; correctness of argument registers in structure doesn't matter for leave probe

        ; setup ProfilePlatformSpecificData structure
        xor                     r8, r8;
        mov                     [rsp + OFFSETOF_PLATFORM_SPECIFIC_DATA +  0h], r8     ; r8 is null      -- struct functionId field
        save_reg_postrsp        rbp, OFFSETOF_PLATFORM_SPECIFIC_DATA +    8h          ;                 -- struct rbp field
        mov                     [rsp + OFFSETOF_PLATFORM_SPECIFIC_DATA + 10h], r10    ; caller rsp      -- struct probeRsp field
        mov                     [rsp + OFFSETOF_PLATFORM_SPECIFIC_DATA + 18h], r11    ; return address  -- struct ip field
        mov                     [rsp + OFFSETOF_PLATFORM_SPECIFIC_DATA + 20h], rdx    ;                 -- struct profiledRsp field
        mov                     [rsp + OFFSETOF_PLATFORM_SPECIFIC_DATA + 28h], rax    ; return value    -- struct rax field
        mov                     [rsp + OFFSETOF_PLATFORM_SPECIFIC_DATA + 30h], r8     ; r8 is null      -- struct hiddenArg field
        movsd                   real8 ptr [rsp + OFFSETOF_PLATFORM_SPECIFIC_DATA + 38h], xmm0    ;      -- struct flt0 field
        movsd                   real8 ptr [rsp + OFFSETOF_PLATFORM_SPECIFIC_DATA + 40h], xmm1    ;      -- struct flt1 field
        movsd                   real8 ptr [rsp + OFFSETOF_PLATFORM_SPECIFIC_DATA + 48h], xmm2    ;      -- struct flt2 field
        movsd                   real8 ptr [rsp + OFFSETOF_PLATFORM_SPECIFIC_DATA + 50h], xmm3    ;      -- struct flt3 field
        mov                     r10, PROFILE_LEAVE
        mov                     [rsp + OFFSETOF_PLATFORM_SPECIFIC_DATA + 58h], r10d   ; flags           -- struct flags field

        ; we need to be able to restore the fp return register
        save_xmm128_postrsp     xmm0, OFFSETOF_FP_ARG_SPILL +  0h
    END_PROLOGUE

        ; rcx already contains the clientInfo
        lea                     rdx, [rsp + OFFSETOF_PLATFORM_SPECIFIC_DATA]
        call                    ProfileLeave

        ; restore fp return register
        movdqa                  xmm0, [rsp + OFFSETOF_FP_ARG_SPILL +  0h]

        ; begin epilogue
        add                     rsp, SIZEOF_STACK_FRAME
        pop                     rax
        ret
NESTED_END ProfileLeaveNaked, _TEXT

;EXTERN_C void ProfileTailcallNaked(FunctionIDOrClientID functionIDOrClientID, size_t profiledRsp);
NESTED_ENTRY ProfileTailcallNaked, _TEXT
        push_nonvol_reg         rax

;       Upon entry :
;           rcx = clientInfo
;           rdx = profiledRsp

        lea                     rax, [rsp + 10h]    ; caller rsp
        mov                     r11, [rax - 8h]     ; return address

        alloc_stack             SIZEOF_STACK_FRAME

        ; correctness of return values and argument registers in structure
        ; doesn't matter for tailcall probe


        ; setup ProfilePlatformSpecificData structure
        xor                     r8, r8;
        mov                     [rsp + OFFSETOF_PLATFORM_SPECIFIC_DATA +  0h], r8     ; r8 is null      -- struct functionId field
        save_reg_postrsp        rbp, OFFSETOF_PLATFORM_SPECIFIC_DATA +    8h          ;                 -- struct rbp field
        mov                     [rsp + OFFSETOF_PLATFORM_SPECIFIC_DATA + 10h], rax    ; caller rsp      -- struct probeRsp field
        mov                     [rsp + OFFSETOF_PLATFORM_SPECIFIC_DATA + 18h], r11    ; return address  -- struct ip field
        mov                     [rsp + OFFSETOF_PLATFORM_SPECIFIC_DATA + 20h], rdx    ;                 -- struct profiledRsp field
        mov                     [rsp + OFFSETOF_PLATFORM_SPECIFIC_DATA + 28h], r8     ; r8 is null      -- struct rax field
        mov                     [rsp + OFFSETOF_PLATFORM_SPECIFIC_DATA + 30h], r8     ; r8 is null      -- struct hiddenArg field
        mov                     [rsp + OFFSETOF_PLATFORM_SPECIFIC_DATA + 38h], r8     ; r8 is null      -- struct flt0 field
        mov                     [rsp + OFFSETOF_PLATFORM_SPECIFIC_DATA + 40h], r8     ; r8 is null      -- struct flt1 field
        mov                     [rsp + OFFSETOF_PLATFORM_SPECIFIC_DATA + 48h], r8     ; r8 is null      -- struct flt2 field
        mov                     [rsp + OFFSETOF_PLATFORM_SPECIFIC_DATA + 50h], r8     ; r8 is null      -- struct flt3 field
        mov                     r10, PROFILE_TAILCALL
        mov                     [rsp + OFFSETOF_PLATFORM_SPECIFIC_DATA + 58h], r10d   ; flags           -- struct flags field

        ; we need to be able to restore the fp return register
        save_xmm128_postrsp     xmm0, OFFSETOF_FP_ARG_SPILL +  0h
    END_PROLOGUE

        ; rcx already contains the clientInfo
        lea                     rdx, [rsp + OFFSETOF_PLATFORM_SPECIFIC_DATA]
        call                    ProfileTailcall

        ; restore fp return register
        movdqa                  xmm0, [rsp + OFFSETOF_FP_ARG_SPILL +  0h]

        ; begin epilogue
        add                     rsp, SIZEOF_STACK_FRAME
        pop                     rax
        ret
NESTED_END ProfileTailcallNaked, _TEXT

ifdef FEATURE_TIERED_COMPILATION

extern OnCallCountThresholdReached:proc

NESTED_ENTRY OnCallCountThresholdReachedStub, _TEXT
        PROLOG_WITH_TRANSITION_BLOCK

        lea     rcx, [rsp + __PWTB_TransitionBlock] ; TransitionBlock *
        mov     rdx, rax ; stub-identifying token, see OnCallCountThresholdReachedStub
        call    OnCallCountThresholdReached

        EPILOG_WITH_TRANSITION_BLOCK_TAILCALL
        TAILJMP_RAX
NESTED_END OnCallCountThresholdReachedStub, _TEXT

extern JIT_PatchpointWorkerWorkerWithPolicy:proc

NESTED_ENTRY JIT_Patchpoint, _TEXT
        PROLOG_WITH_TRANSITION_BLOCK

        lea     rcx, [rsp + __PWTB_TransitionBlock] ; TransitionBlock *
        call    JIT_PatchpointWorkerWorkerWithPolicy

        EPILOG_WITH_TRANSITION_BLOCK_RETURN
        TAILJMP_RAX
NESTED_END JIT_Patchpoint, _TEXT

; first arg register holds iloffset, which needs to be moved to the second register, and the first register filled with NULL
LEAF_ENTRY JIT_PatchpointForced, _TEXT
        mov rdx, rcx
        xor rcx, rcx
        jmp JIT_Patchpoint
LEAF_END JIT_PatchpointForced, _TEXT

endif ; FEATURE_TIERED_COMPILATION

LEAF_ENTRY JIT_PollGC, _TEXT
    cmp [g_TrapReturningThreads], 0
    jnz             JIT_PollGCRarePath
    ret
JIT_PollGCRarePath:
    mov rax, g_pPollGC
    TAILJMP_RAX
LEAF_END JIT_PollGC, _TEXT

; rcx -This pointer
; rdx -ReturnBuffer
LEAF_ENTRY ThisPtrRetBufPrecodeWorker, _TEXT
    mov  METHODDESC_REGISTER, [METHODDESC_REGISTER + ThisPtrRetBufPrecodeData__Target]
    mov r11, rcx
    mov rcx, rdx
    mov rdx, r11
    jmp METHODDESC_REGISTER
LEAF_END ThisPtrRetBufPrecodeWorker, _TEXT

;;
;; Prologue of all funclet calling helpers (CallXXXXFunclet)
;;
FUNCLET_CALL_PROLOGUE macro localsCount, alignStack
        PUSH_CALLEE_SAVED_REGISTERS

        arguments_scratch_area_size = 20h
        xmm_save_area_size = 10 * 10h ;; xmm6..xmm15 save area
        stack_alloc_size = arguments_scratch_area_size + localsCount * 8 + alignStack * 8 + xmm_save_area_size
        rsp_offsetof_arguments = stack_alloc_size + 8*8h + 8h
        rsp_offsetof_locals = arguments_scratch_area_size + xmm_save_area_size

        alloc_stack     stack_alloc_size

        save_xmm128_postrsp xmm6,  (arguments_scratch_area_size + 0 * 10h)
        save_xmm128_postrsp xmm7,  (arguments_scratch_area_size + 1 * 10h)
        save_xmm128_postrsp xmm8,  (arguments_scratch_area_size + 2 * 10h)
        save_xmm128_postrsp xmm9,  (arguments_scratch_area_size + 3 * 10h)
        save_xmm128_postrsp xmm10, (arguments_scratch_area_size + 4 * 10h)
        save_xmm128_postrsp xmm11, (arguments_scratch_area_size + 5 * 10h)
        save_xmm128_postrsp xmm12, (arguments_scratch_area_size + 6 * 10h)
        save_xmm128_postrsp xmm13, (arguments_scratch_area_size + 7 * 10h)
        save_xmm128_postrsp xmm14, (arguments_scratch_area_size + 8 * 10h)
        save_xmm128_postrsp xmm15, (arguments_scratch_area_size + 9 * 10h)

        END_PROLOGUE
endm

;;
;; Epilogue of all funclet calling helpers (CallXXXXFunclet)
;;
FUNCLET_CALL_EPILOGUE macro
        movdqa  xmm6,  [rsp + arguments_scratch_area_size + 0 * 10h]
        movdqa  xmm7,  [rsp + arguments_scratch_area_size + 1 * 10h]
        movdqa  xmm8,  [rsp + arguments_scratch_area_size + 2 * 10h]
        movdqa  xmm9,  [rsp + arguments_scratch_area_size + 3 * 10h]
        movdqa  xmm10, [rsp + arguments_scratch_area_size + 4 * 10h]
        movdqa  xmm11, [rsp + arguments_scratch_area_size + 5 * 10h]
        movdqa  xmm12, [rsp + arguments_scratch_area_size + 6 * 10h]
        movdqa  xmm13, [rsp + arguments_scratch_area_size + 7 * 10h]
        movdqa  xmm14, [rsp + arguments_scratch_area_size + 8 * 10h]
        movdqa  xmm15, [rsp + arguments_scratch_area_size + 9 * 10h]

        add     rsp, stack_alloc_size

        POP_CALLEE_SAVED_REGISTERS
endm

; This helper enables us to call into a funclet after restoring Fp register
NESTED_ENTRY CallEHFunclet, _TEXT
        ; On entry:
        ;
        ; RCX = throwable
        ; RDX = PC to invoke
        ; R8 = address of CONTEXT record; used to restore the non-volatile registers of CrawlFrame
        ; R9 = address of the location where the SP of funclet's caller (i.e. this helper) should be saved.
        ;

        FUNCLET_CALL_PROLOGUE 0, 1

        ;  Restore RBX, RBP, RSI, RDI, R12, R13, R14, R15 from CONTEXT
        mov     rbx, [r8 + OFFSETOF__CONTEXT__Rbx]
        mov     rbp, [r8 + OFFSETOF__CONTEXT__Rbp]
        mov     rsi, [r8 + OFFSETOF__CONTEXT__Rsi]
        mov     rdi, [r8 + OFFSETOF__CONTEXT__Rdi]
        mov     r12, [r8 + OFFSETOF__CONTEXT__R12]
        mov     r13, [r8 + OFFSETOF__CONTEXT__R13]
        mov     r14, [r8 + OFFSETOF__CONTEXT__R14]
        mov     r15, [r8 + OFFSETOF__CONTEXT__R15]

        ; Restore XMM registers from CONTEXT
        movdqa  xmm6, [r8 + OFFSETOF__CONTEXT__Xmm6]
        movdqa  xmm7, [r8 + OFFSETOF__CONTEXT__Xmm7]
        movdqa  xmm8, [r8 + OFFSETOF__CONTEXT__Xmm8]
        movdqa  xmm9, [r8 + OFFSETOF__CONTEXT__Xmm9]
        movdqa  xmm10, [r8 + OFFSETOF__CONTEXT__Xmm10]
        movdqa  xmm11, [r8 + OFFSETOF__CONTEXT__Xmm11]
        movdqa  xmm12, [r8 + OFFSETOF__CONTEXT__Xmm12]
        movdqa  xmm13, [r8 + OFFSETOF__CONTEXT__Xmm13]
        movdqa  xmm14, [r8 + OFFSETOF__CONTEXT__Xmm14]
        movdqa  xmm15, [r8 + OFFSETOF__CONTEXT__Xmm15]

         ; Save the SP of this function.
        mov     [r9], rsp
        ; Invoke the funclet
        call    rdx

        FUNCLET_CALL_EPILOGUE
        ret
NESTED_END CallEHFunclet, _TEXT

; This helper enables us to call into a filter funclet by passing it the CallerSP to lookup the
; frame pointer for accessing the locals in the parent method.
NESTED_ENTRY CallEHFilterFunclet, _TEXT
        ; On entry:
        ;
        ; RCX = throwable
        ; RDX = RBP of main function
        ; R8 = PC to invoke
        ; R9 = address of the location where the SP of funclet's caller (i.e. this helper) should be saved.
        ;

        FUNCLET_CALL_PROLOGUE 0, 1

        ; Save the SP of this function
        mov     [r9], rsp
        ; Restore RBP to match main function RBP
        mov     rbp, rdx
        ; Invoke the filter funclet
        call    r8

        FUNCLET_CALL_EPILOGUE
        ret
NESTED_END CallEHFilterFunclet, _TEXT

ifdef FEATURE_INTERPRETER

NESTED_ENTRY InterpreterStub, _TEXT

        PROLOG_WITH_TRANSITION_BLOCK

        __InterpreterStubArgumentRegistersOffset = __PWTB_ArgumentRegisters
        ; IR bytecode address
        mov             rbx, METHODDESC_REGISTER

        INLINE_GETTHREAD r10; thrashes rax and r11

        mov             rax, qword ptr [r10 + OFFSETOF__Thread__m_pInterpThreadContext]
        test            rax, rax
        jnz             HaveInterpThreadContext

        mov             rcx, r10
        call            Thread_GetInterpThreadContext
        RESTORE_ARGUMENT_REGISTERS __PWTB_ArgumentRegisters
        RESTORE_FLOAT_ARGUMENT_REGISTERS __PWTB_FloatArgumentRegisters

HaveInterpThreadContext:
        mov             r10, qword ptr [rax + OFFSETOF__InterpThreadContext__pStackPointer]
        ; Load the InterpMethod pointer from the IR bytecode
        mov             rax, qword ptr [rbx]
        mov             rax, qword ptr [rax + OFFSETOF__InterpMethod__pCallStub]
        lea             r11, qword ptr [rax + OFFSETOF__CallStubHeader__Routines]
        lea             rax, [rsp + __PWTB_TransitionBlock]
        ; Copy the arguments to the interpreter stack, invoke the InterpExecMethod and load the return value
        call            qword ptr [r11]

        EPILOG_WITH_TRANSITION_BLOCK_RETURN

NESTED_END InterpreterStub, _TEXT

NESTED_ENTRY InterpreterStubRetVoid, _TEXT
        alloc_stack 028h
END_PROLOGUE
        mov             rcx, rax ; pTransitionBlock*
        mov             rdx, rbx ; the IR bytecode pointer
        xor             r8, r8
        call            ExecuteInterpretedMethod
        add             rsp, 028h
        ret
NESTED_END InterpreterStubRetVoid, _TEXT

NESTED_ENTRY InterpreterStubRetI8, _TEXT
        alloc_stack 028h
END_PROLOGUE
        mov             rcx, rax ; pTransitionBlock*
        mov             rdx, rbx ; the IR bytecode pointer
        xor             r8, r8
        call            ExecuteInterpretedMethod
        mov             rax, qword ptr [rax]
        add             rsp, 028h
        ret
NESTED_END InterpreterStubRetI8, _TEXT

NESTED_ENTRY InterpreterStubRetDouble, _TEXT
        alloc_stack 028h
END_PROLOGUE
        mov             rcx, rax ; pTransitionBlock*
        mov             rdx, rbx ; the IR bytecode pointer
        xor             r8, r8
        call            ExecuteInterpretedMethod
        movsd           xmm0, real8 ptr [rax]
        add             rsp, 028h
        ret
NESTED_END InterpreterStubRetDouble, _TEXT

NESTED_ENTRY InterpreterStubRetBuffRCX, _TEXT
        alloc_stack 028h
END_PROLOGUE
        mov             rcx, rax ; pTransitionBlock*
        mov             rdx, rbx ; the IR bytecode pointer
        ; Load the return buffer address from the original rcx argument register
        mov             r8, qword ptr [rsp + 028h + 8 + __InterpreterStubArgumentRegistersOffset]
        call            ExecuteInterpretedMethod
        add             rsp, 028h
        ret
NESTED_END InterpreterStubRetBuffRCX, _TEXT

NESTED_ENTRY InterpreterStubRetBuffRDX, _TEXT
        alloc_stack 028h
END_PROLOGUE
        mov             rcx, rax ; pTransitionBlock*
        mov             rdx, rbx ; the IR bytecode pointer
        ; Load the return buffer address from the original rxx argument register
        mov             r8, qword ptr [rsp + 028h + 8 + __InterpreterStubArgumentRegistersOffset + 8];
        call            ExecuteInterpretedMethod
        add             rsp, 028h
        ret
NESTED_END InterpreterStubRetBuffRDX, _TEXT

; Copy arguments from the the processor stack to the interpreter stack.
; The CPU stack slots are aligned to pointer size.
LEAF_ENTRY Store_Stack, _TEXT
        mov esi, dword ptr [r11 + 8]  ; SP offset
        mov ecx, dword ptr [r11 + 12] ; number of stack slots
        ; load the caller Rsp as a based for the stack arguments
        ; The 8 represent the return address slot
        lea rsi, [rsp + rsi + 8 + __InterpreterStubArgumentRegistersOffset]
        mov rdi, r10
        shr rcx, 3
        rep movsq
        mov r10, rdi
        add r11, 16
        jmp qword ptr [r11]
LEAF_END Store_Stack, _TEXT

LEAF_ENTRY Load_Stack_Ref, _TEXT
        mov esi, dword ptr [r11 + 8]  ; SP offset
        mov edi, dword ptr [r11 + 12] ; size of the value type
        add rsi, 8; return address
        add rsi, rsp
        mov qword ptr [rsi], r10
        add r10, rdi
        lea r10, [r10 + 7]
        and r10, 0fffffffffffffff8h
        add r11, 16
        jmp qword ptr [r11]
LEAF_END Load_Stack_Ref, _TEXT

LEAF_ENTRY Store_Stack_Ref, _TEXT
        mov esi, dword ptr [r11 + 8]  ; SP offset
        mov ecx, dword ptr [r11 + 12] ; size of the value type
        mov rsi, [rsp + rsi + 8 + __InterpreterStubArgumentRegistersOffset]
        mov rdi, r10
        rep movsb
        ; align rdi up to the stack slot size
        lea rdi, [rdi + 7]
        and rdi, 0fffffffffffffff8h
        mov r10, rdi
        add r11, 16
        jmp qword ptr [r11]
LEAF_END Store_Stack_Ref, _TEXT

; Routines for passing value type arguments by reference in general purpose registers RCX, RDX, R8, R9
; from native code to the interpreter

Store_Ref macro argReg

LEAF_ENTRY Store_Ref_&argReg, _TEXT
        mov rsi, argReg
        mov rcx, [r11 + 8] ; size of the value type
        mov rdi, r10
        rep movsb
        ; align rdi up to the stack slot size
        lea rdi, [rdi + 7]
        and rdi, 0fffffffffffffff8h
        mov r10, rdi
        add r11, 16
        jmp qword ptr [r11]
LEAF_END Store_Ref_&argReg, _TEXT

        endm

Store_Ref RCX
Store_Ref RDX
Store_Ref R8
Store_Ref R9

; Routines for passing arguments by value in general purpose registers RCX, RDX, R8, R9
; from native code to the interpreter

LEAF_ENTRY Store_RCX, _TEXT
        mov [r10], rcx
        add r10, 8
        add r11, 8
        jmp qword ptr [r11]
LEAF_END Store_RCX, _TEXT

LEAF_ENTRY Store_RCX_RDX, _TEXT
        mov [r10], rcx
        mov [r10 + 8], rdx
        add r10, 16
        add r11, 8
        jmp qword ptr [r11]
LEAF_END Store_RCX_RDX, _TEXT

LEAF_ENTRY Store_RCX_RDX_R8, _TEXT
        mov [r10], rcx
        mov [r10 + 8], rdx
        mov [r10 + 16], r8
        add r10, 24
        add r11, 8
        jmp qword ptr [r11]
LEAF_END Store_RCX_RDX_R8, _TEXT

LEAF_ENTRY Store_RCX_RDX_R8_R9, _TEXT
        mov [r10], rcx
        mov [r10 + 8], rdx
        mov [r10 + 16], r8
        mov [r10 + 24], r9
        add r10, 32
        add r11, 8
        jmp qword ptr [r11]
LEAF_END Store_RCX_RDX_R8_R9, _TEXT

LEAF_ENTRY Store_RDX, _TEXT
        mov [r10], rdx
        add r10, 8
        add r11, 8
        jmp qword ptr [r11]
LEAF_END Store_RDX, _TEXT

LEAF_ENTRY Store_RDX_R8, _TEXT
        mov [r10], rdx
        mov [r10 + 8], r8
        add r10, 16
        add r11, 8
        jmp qword ptr [r11]
LEAF_END Store_RDX_R8, _TEXT

LEAF_ENTRY Store_RDX_R8_R9, _TEXT
        mov [r10], rdx
        mov [r10 + 8], r8
        mov [r10 + 16], r9
        add r10, 24
        add r11, 8
        jmp qword ptr [r11]
LEAF_END Store_RDX_R8_R9, _TEXT

LEAF_ENTRY Store_R8, _TEXT
        mov [r10], r8
        add r10, 8
        add r11, 8
        jmp qword ptr [r11]
LEAF_END Store_R8, _TEXT

LEAF_ENTRY Store_R8_R9, _TEXT
        mov [r10], r8
        mov [r10 + 8], r9
        add r10, 16
        add r11, 8
        jmp qword ptr [r11]
LEAF_END Store_R8_R9, _TEXT

LEAF_ENTRY Store_R9, _TEXT
        mov [r10], r9
        add r10, 8
        add r11, 8
        jmp qword ptr [r11]
LEAF_END Store_R9, _TEXT

LEAF_ENTRY Store_XMM0, _TEXT
        movsd real8 ptr [r10], xmm0
        add r10, 8
        add r11, 8
        jmp qword ptr [r11]
LEAF_END Store_XMM0, _TEXT

LEAF_ENTRY Store_XMM0_XMM1, _TEXT
        movsd real8 ptr [r10], xmm0
        movsd real8 ptr [r10 + 8], xmm1
        add r10, 16
        add r11, 8
        jmp qword ptr [r11]
LEAF_END Store_XMM0_XMM1, _TEXT

LEAF_ENTRY Store_XMM0_XMM1_XMM2, _TEXT
        movsd real8 ptr [r10], xmm0
        movsd real8 ptr [r10 + 8], xmm1
        movsd real8 ptr [r10 + 16], xmm2
        add r10, 24
        add r11, 8
        jmp qword ptr [r11]
LEAF_END Store_XMM0_XMM1_XMM2, _TEXT

LEAF_ENTRY Store_XMM0_XMM1_XMM2_XMM3, _TEXT
        movsd real8 ptr [r10], xmm0
        movsd real8 ptr [r10 + 8], xmm1
        movsd real8 ptr [r10 + 16], xmm2
        movsd real8 ptr [r10 + 24], xmm3
        add r10, 32
        add r11, 8
        jmp qword ptr [r11]
LEAF_END Store_XMM0_XMM1_XMM2_XMM3, _TEXT

LEAF_ENTRY Store_XMM1, _TEXT
        movsd real8 ptr [r10], xmm1
        add r10, 8
        add r11, 8
        jmp qword ptr [r11]
LEAF_END Store_XMM1, _TEXT

LEAF_ENTRY Store_XMM1_XMM2, _TEXT
        movsd real8 ptr [r10], xmm1
        movsd real8 ptr [r10 + 8], xmm2
        add r10, 16
        add r11, 8
        jmp qword ptr [r11]
LEAF_END Store_XMM1_XMM2, _TEXT

LEAF_ENTRY Store_XMM1_XMM2_XMM3, _TEXT
        movsd real8 ptr [r10], xmm1
        movsd real8 ptr [r10 + 8], xmm2
        movsd real8 ptr [r10 + 16], xmm3
        add r10, 24
        add r11, 8
        jmp qword ptr [r11]
LEAF_END Store_XMM1_XMM2_XMM3, _TEXT

LEAF_ENTRY Store_XMM2, _TEXT
        movsd real8 ptr [r10], xmm2
        add r10, 8
        add r11, 8
        jmp qword ptr [r11]
LEAF_END Store_XMM2, _TEXT

LEAF_ENTRY Store_XMM2_XMM3, _TEXT
        movsd real8 ptr [r10], xmm2
        movsd real8 ptr [r10 + 8], xmm3
        add r10, 16
        add r11, 8
        jmp qword ptr [r11]
LEAF_END Store_XMM2_XMM3, _TEXT

LEAF_ENTRY Store_XMM3, _TEXT
        movsd real8 ptr [r10], xmm3
        add r10, 8
        add r11, 8
        jmp qword ptr [r11]
LEAF_END Store_XMM3, _TEXT

; Copy arguments from the interpreter stack to the processor stack.
; The CPU stack slots are aligned to pointer size.
LEAF_ENTRY Load_Stack, _TEXT
        push rdi
        push rsi
        push rcx
        mov edi, dword ptr [r11 + 8]  ; SP offset
        mov ecx, dword ptr [r11 + 12] ; number of stack slots
        add edi, 20h ; the 3 pushes above plus return address
        add rdi, rsp
        mov rsi, r10
        shr rcx, 3
        rep movsq
        mov r10, rsi
        pop rcx
        pop rsi
        pop rdi
        add r11, 16
        jmp qword ptr [r11]
LEAF_END Load_Stack, _TEXT

; Routines for passing value type arguments by reference in general purpose registers RCX, RDX, R8, R9
; from the interpreter to native code

LEAF_ENTRY Load_Ref_RCX, _TEXT
        mov rcx, r10
        add r10, [r11 + 8] ; size of the value type
        add r11, 16
        jmp qword ptr [r11]
LEAF_END Load_Ref_RCX, _TEXT

LEAF_ENTRY Load_Ref_RDX, _TEXT
        mov rdx, r10
        add r10, [r11 + 8] ; size of the value type
        add r11, 16
        jmp qword ptr [r11]
LEAF_END Load_Ref_RDX, _TEXT

LEAF_ENTRY Load_Ref_R8, _TEXT
        mov r8, r10
        add r10, [r11 + 8] ; size of the value type
        add r11, 16
        jmp qword ptr [r11]
LEAF_END Load_Ref_R8, _TEXT

LEAF_ENTRY Load_Ref_R9, _TEXT
        mov r9, r10
        add r10, [r11 + 8] ; size of the value type
        add r11, 16
        jmp qword ptr [r11]
LEAF_END Load_Ref_R9, _TEXT

; Routines for passing arguments by value in general purpose registers RCX, RDX, R8, R9
; from the interpreter to native code

LEAF_ENTRY Load_RCX, _TEXT
        mov rcx, [r10]
        add r10, 8
        add r11, 8
        jmp qword ptr [r11]
LEAF_END Load_RCX, _TEXT

LEAF_ENTRY Load_RCX_RDX, _TEXT
        mov rcx, [r10]
        mov rdx, [r10 + 8]
        add r10, 16
        add r11, 8
        jmp qword ptr [r11]
LEAF_END Load_RCX_RDX, _TEXT

LEAF_ENTRY Load_RCX_RDX_R8, _TEXT
        mov rcx, [r10]
        mov rdx, [r10 + 8]
        mov r8, [r10 + 16]
        add r10, 24
        add r11, 8
        jmp qword ptr [r11]
LEAF_END Load_RCX_RDX_R8, _TEXT

LEAF_ENTRY Load_RCX_RDX_R8_R9, _TEXT
        mov rcx, [r10]
        mov rdx, [r10 + 8]
        mov r8, [r10 + 16]
        mov r9, [r10 + 24]
        add r10, 32
        add r11, 8
        jmp qword ptr [r11]
LEAF_END Load_RCX_RDX_R8_R9, _TEXT

LEAF_ENTRY Load_RDX, _TEXT
        mov rdx, [r10]
        add r10, 8
        add r11, 8
        jmp qword ptr [r11]
LEAF_END Load_RDX, _TEXT

LEAF_ENTRY Load_RDX_R8, _TEXT
        mov rdx, [r10]
        mov r8, [r10 + 8]
        add r10, 16
        add r11, 8
        jmp qword ptr [r11]
LEAF_END Load_RDX_R8, _TEXT

LEAF_ENTRY Load_RDX_R8_R9, _TEXT
        mov rdx, [r10]
        mov r8, [r10 + 8]
        mov r9, [r10 + 16]
        add r10, 24
        add r11, 8
        jmp qword ptr [r11]
LEAF_END Load_RDX_R8_R9, _TEXT

LEAF_ENTRY Load_R8, _TEXT
        mov r8, [r10]
        add r10, 8
        add r11, 8
        jmp qword ptr [r11]
LEAF_END Load_R8, _TEXT

LEAF_ENTRY Load_R8_R9, _TEXT
        mov r8, [r10]
        mov r9, [r10 + 8]
        add r10, 16
        add r11, 8
        jmp qword ptr [r11]
LEAF_END Load_R8_R9, _TEXT

LEAF_ENTRY Load_R9, _TEXT
        mov r9, [r10]
        add r10, 8
        add r11, 8
        jmp qword ptr [r11]
LEAF_END Load_R9, _TEXT

; Routines for passing arguments in floating point registers XMM0..XMM3

LEAF_ENTRY Load_XMM0, _TEXT
        movsd xmm0, real8 ptr [r10]
        add r10, 8
        add r11, 8
        jmp qword ptr [r11]
LEAF_END Load_XMM0, _TEXT

LEAF_ENTRY Load_XMM0_XMM1, _TEXT
        movsd xmm0, real8 ptr [r10]
        movsd xmm1, real8 ptr [r10 + 8]
        add r10, 10h
        add r11, 8
        jmp qword ptr [r11]
LEAF_END Load_XMM0_XMM1, _TEXT

LEAF_ENTRY Load_XMM0_XMM1_XMM2, _TEXT
        movsd xmm0, real8 ptr [r10]
        movsd xmm1, real8 ptr [r10 + 8]
        movsd xmm2, real8 ptr [r10 + 16]
        add r10, 24
        add r11, 8
        jmp qword ptr [r11]
LEAF_END Load_XMM0_XMM1_XMM2, _TEXT

LEAF_ENTRY Load_XMM0_XMM1_XMM2_XMM3, _TEXT
        movsd xmm0, real8 ptr [r10]
        movsd xmm1, real8 ptr [r10 + 8]
        movsd xmm2, real8 ptr [r10 + 16]
        movsd xmm3, real8 ptr [r10 + 24]
        add r10, 32
        add r11, 8
        jmp qword ptr [r11]
LEAF_END Load_XMM0_XMM1_XMM2_XMM3, _TEXT

LEAF_ENTRY Load_XMM1, _TEXT
        movsd xmm1, real8 ptr [r10]
        add r10, 8
        add r11, 8
        jmp qword ptr [r11]
LEAF_END Load_XMM1, _TEXT

LEAF_ENTRY Load_XMM1_XMM2, _TEXT
        movsd xmm1, real8 ptr [r10]
        movsd xmm2, real8 ptr [r10 + 8]
        add r10, 16
        add r11, 8
        jmp qword ptr [r11]
LEAF_END Load_XMM1_XMM2, _TEXT

LEAF_ENTRY Load_XMM1_XMM2_XMM3, _TEXT
        movsd xmm1, real8 ptr [r10]
        movsd xmm2, real8 ptr [r10 + 8]
        movsd xmm3, real8 ptr [r10 + 16]
        add r10, 24
        add r11, 8
        jmp qword ptr [r11]
LEAF_END Load_XMM1_XMM2_XMM3, _TEXT

LEAF_ENTRY Load_XMM2, _TEXT
        movsd xmm2, real8 ptr [r10]
        add r10, 8
        add r11, 8
        jmp qword ptr [r11]
LEAF_END Load_XMM2, _TEXT

LEAF_ENTRY Load_XMM2_XMM3, _TEXT
        movsd xmm2, real8 ptr [r10]
        movsd xmm3, real8 ptr [r10 + 8]
        add r10, 16
        add r11, 8
        jmp qword ptr [r11]
LEAF_END Load_XMM2_XMM3, _TEXT

LEAF_ENTRY Load_XMM3, _TEXT
        movsd xmm3, real8 ptr [r10]
        add r10, 8
        add r11, 8
        jmp qword ptr [r11]
LEAF_END Load_XMM3, _TEXT

NESTED_ENTRY CallJittedMethodRetVoid, _TEXT
        push_nonvol_reg rbp
        set_frame rbp, 0
END_PROLOGUE
        add r9, 20h ; argument save area + alignment
        sub rsp, r9 ; total stack space
        mov r11, rcx ; The routines list
        mov r10, rdx ; interpreter stack args
        call qword ptr [r11]
        mov rsp, rbp
        pop rbp
        ret
NESTED_END CallJittedMethodRetVoid, _TEXT

NESTED_ENTRY CallJittedMethodRetBuffRCX, _TEXT
        push_nonvol_reg rbp
        set_frame rbp, 0
END_PROLOGUE
        add r9, 20h ; argument save area + alignment
        sub rsp, r9 ; total stack space
        mov r11, rcx ; The routines list
        mov r10, rdx ; interpreter stack args
        mov rcx, r8  ; return buffer
        call qword ptr [r11]
        mov rsp, rbp
        pop rbp
        ret
NESTED_END CallJittedMethodRetBuffRCX, _TEXT

NESTED_ENTRY CallJittedMethodRetBuffRDX, _TEXT
        push_nonvol_reg rbp
        set_frame rbp, 0
END_PROLOGUE
        add r9, 20h ; argument save area + alignment
        sub rsp, r9 ; total stack space
        mov r11, rcx ; The routines list
        mov r10, rdx ; interpreter stack args
        mov rdx, r8  ; return buffer
        call qword ptr [r11]
        mov rsp, rbp
        pop rbp
        ret
NESTED_END CallJittedMethodRetBuffRDX, _TEXT


NESTED_ENTRY CallJittedMethodRetDouble, _TEXT
        push_nonvol_reg rbp
        set_frame rbp, 0
        push_vol_reg r8
        push_vol_reg rax ; align
END_PROLOGUE
        add r9, 20h ; argument save area + alignment
        sub rsp, r9 ; total stack space
        mov r11, rcx ; The routines list
        mov r10, rdx ; interpreter stack args
        call qword ptr [r11]
        mov r8, [rbp - 8]
        movsd real8 ptr [r8], xmm0
        mov rsp, rbp
        pop rbp
        ret
NESTED_END CallJittedMethodRetDouble, _TEXT

NESTED_ENTRY CallJittedMethodRetI8, _TEXT
        push_nonvol_reg rbp
        set_frame rbp, 0
        push_vol_reg r8
        push_vol_reg rax ; align
END_PROLOGUE
        add r9, 20h ; argument save area + alignment
        sub rsp, r9 ; total stack space
        mov r11, rcx ; The routines list
        mov r10, rdx ; interpreter stack args
        call qword ptr [r11]
        mov r8, [rbp - 8]
        mov qword ptr [r8], rax
        mov rsp, rbp
        pop rbp
        ret
NESTED_END CallJittedMethodRetI8, _TEXT

endif ; FEATURE_INTERPRETER

        end
