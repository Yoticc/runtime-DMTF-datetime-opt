// Copyright (c) .NET Foundation and contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

//
// MarkStep.cs
//
// Author:
//   Jb Evain (jbevain@gmail.com)
//
// (C) 2006 Jb Evain
// (C) 2007 Novell, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining
// a copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to
// the following conditions:
//
// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
// LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
// OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
// WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Diagnostics.CodeAnalysis;
using System.Linq;
using System.Reflection.Metadata.Ecma335;
using System.Text.RegularExpressions;
using ILLink.Shared;
using ILLink.Shared.TrimAnalysis;
using ILLink.Shared.TypeSystemProxy;
using Mono.Cecil;
using Mono.Cecil.Cil;
using Mono.Collections.Generic;
using Mono.Linker.Dataflow;

namespace Mono.Linker.Steps
{

    public partial class MarkStep : IStep
    {
        LinkContext? _context;
        protected LinkContext Context
        {
            get
            {
                Debug.Assert(_context != null);
                return _context;
            }
        }

        protected Queue<(MethodDefinition, DependencyInfo)> _methods;
        protected Dictionary<MethodDefinition, MessageOrigin> _interface_methods;
        protected Queue<AttributeProviderPair> _assemblyLevelAttributes;
        protected Queue<(AttributeProviderPair, DependencyInfo, MessageOrigin)> _lateMarkedAttributes;
        protected List<(TypeDefinition, MessageOrigin)> _typesWithInterfaces;
        protected HashSet<AssemblyDefinition> _dynamicInterfaceCastableImplementationTypesDiscovered;
        protected List<TypeDefinition> _dynamicInterfaceCastableImplementationTypes;
        protected List<(MethodBody, MessageOrigin)> _unreachableBodies;

        readonly List<(TypeDefinition Type, MethodBody Body, Instruction Instr)> _pending_isinst_instr;

        // Stores, for compiler-generated methods only, whether they require the reflection
        // method body scanner.
        readonly Dictionary<MethodBody, bool> _compilerGeneratedMethodRequiresScanner;

        TypeMapHandler _typeMapHandler;

        MarkStepContext? _markContext;
        MarkStepContext MarkContext
        {
            get
            {
                Debug.Assert(_markContext != null);
                return _markContext;
            }
        }
        readonly HashSet<TypeDefinition> _entireTypesMarked;
        DynamicallyAccessedMembersTypeHierarchy? _dynamicallyAccessedMembersTypeHierarchy;

        internal DynamicallyAccessedMembersTypeHierarchy DynamicallyAccessedMembersTypeHierarchy
        {
            get
            {
                Debug.Assert(_dynamicallyAccessedMembersTypeHierarchy != null);
                return _dynamicallyAccessedMembersTypeHierarchy;
            }
        }

        internal TypeMapHandler TypeMapHandler => _typeMapHandler;

#if DEBUG
        static readonly DependencyKind[] _entireTypeReasons = new DependencyKind[] {
            DependencyKind.AccessedViaReflection,
            DependencyKind.BaseType,
            DependencyKind.PreservedDependency,
            DependencyKind.NestedType,
            DependencyKind.TypeInAssembly,
            DependencyKind.Unspecified,
        };

        static readonly DependencyKind[] _fieldReasons = new DependencyKind[] {
            DependencyKind.Unspecified,
            DependencyKind.AccessedViaReflection,
            DependencyKind.AlreadyMarked,
            DependencyKind.Custom,
            DependencyKind.CustomAttributeField,
            DependencyKind.DynamicallyAccessedMember,
            DependencyKind.DynamicallyAccessedMemberOnType,
            DependencyKind.EventSourceProviderField,
            DependencyKind.FieldAccess,
            DependencyKind.FieldOnGenericInstance,
            DependencyKind.InteropMethodDependency,
            DependencyKind.Ldtoken,
            DependencyKind.MemberOfType,
            DependencyKind.DynamicDependency,
            DependencyKind.ReferencedBySpecialAttribute,
            DependencyKind.TypePreserve,
            DependencyKind.XmlDescriptor,
            DependencyKind.UnsafeAccessorTarget,
        };

        static readonly DependencyKind[] _typeReasons = new DependencyKind[] {
            DependencyKind.Unspecified,
            DependencyKind.AccessedViaReflection,
            DependencyKind.AlreadyMarked,
            DependencyKind.AttributeType,
            DependencyKind.BaseType,
            DependencyKind.CatchType,
            DependencyKind.Custom,
            DependencyKind.CustomAttributeArgumentType,
            DependencyKind.CustomAttributeArgumentValue,
            DependencyKind.DeclaringType,
            DependencyKind.DeclaringTypeOfCalledMethod,
            DependencyKind.DynamicallyAccessedMember,
            DependencyKind.DynamicallyAccessedMemberOnType,
            DependencyKind.DynamicDependency,
            DependencyKind.ElementType,
            DependencyKind.FieldType,
            DependencyKind.GenericArgumentType,
            DependencyKind.GenericParameterConstraintType,
            DependencyKind.InterfaceImplementationInterfaceType,
            DependencyKind.Ldtoken,
            DependencyKind.ModifierType,
            DependencyKind.NestedType,
            DependencyKind.InstructionTypeRef,
            DependencyKind.ParameterType,
            DependencyKind.ReferencedBySpecialAttribute,
            DependencyKind.ReturnType,
            DependencyKind.TypeInAssembly,
            DependencyKind.UnreachableBodyRequirement,
            DependencyKind.VariableType,
            DependencyKind.ParameterMarshalSpec,
            DependencyKind.FieldMarshalSpec,
            DependencyKind.ReturnTypeMarshalSpec,
            DependencyKind.DynamicInterfaceCastableImplementation,
            DependencyKind.XmlDescriptor,
        };

        static readonly DependencyKind[] _methodReasons = new DependencyKind[] {
            DependencyKind.Unspecified,
            DependencyKind.AccessedViaReflection,
            DependencyKind.AlreadyMarked,
            DependencyKind.AttributeConstructor,
            DependencyKind.AttributeProperty,
            DependencyKind.BaseDefaultCtorForStubbedMethod,
            DependencyKind.BaseMethod,
            DependencyKind.CctorForType,
            DependencyKind.CctorForField,
            DependencyKind.Custom,
            DependencyKind.DefaultCtorForNewConstrainedGenericArgument,
            DependencyKind.DirectCall,
            DependencyKind.DynamicallyAccessedMember,
            DependencyKind.DynamicallyAccessedMemberOnType,
            DependencyKind.DynamicDependency,
            DependencyKind.ElementMethod,
            DependencyKind.EventMethod,
            DependencyKind.EventOfEventMethod,
            DependencyKind.InteropMethodDependency,
            DependencyKind.KeptForSpecialAttribute,
            DependencyKind.Ldftn,
            DependencyKind.Ldtoken,
            DependencyKind.Ldvirtftn,
            DependencyKind.MemberOfType,
            DependencyKind.MethodForInstantiatedType,
            DependencyKind.MethodForSpecialType,
            DependencyKind.MethodImplOverride,
            DependencyKind.MethodOnGenericInstance,
            DependencyKind.Newobj,
            DependencyKind.Override,
            DependencyKind.OverrideOnInstantiatedType,
            DependencyKind.DynamicDependency,
            DependencyKind.PreservedMethod,
            DependencyKind.ReferencedBySpecialAttribute,
            DependencyKind.SerializationMethodForType,
            DependencyKind.TriggersCctorForCalledMethod,
            DependencyKind.TriggersCctorThroughFieldAccess,
            DependencyKind.TypePreserve,
            DependencyKind.UnreachableBodyRequirement,
            DependencyKind.VirtualCall,
            DependencyKind.VirtualNeededDueToPreservedScope,
            DependencyKind.ParameterMarshalSpec,
            DependencyKind.FieldMarshalSpec,
            DependencyKind.ReturnTypeMarshalSpec,
            DependencyKind.XmlDescriptor,
            DependencyKind.UnsafeAccessorTarget,
        };
#endif

        public MarkStep()
        {
            _methods = new Queue<(MethodDefinition, DependencyInfo)>();
            _interface_methods = new Dictionary<MethodDefinition, MessageOrigin>();
            _assemblyLevelAttributes = new Queue<AttributeProviderPair>();
            _lateMarkedAttributes = new Queue<(AttributeProviderPair, DependencyInfo, MessageOrigin)>();
            _typesWithInterfaces = new List<(TypeDefinition, MessageOrigin)>();
            _dynamicInterfaceCastableImplementationTypesDiscovered = new HashSet<AssemblyDefinition>();
            _dynamicInterfaceCastableImplementationTypes = new List<TypeDefinition>();
            _unreachableBodies = new List<(MethodBody, MessageOrigin)>();
            _pending_isinst_instr = new List<(TypeDefinition, MethodBody, Instruction)>();
            _entireTypesMarked = new HashSet<TypeDefinition>();
            _compilerGeneratedMethodRequiresScanner = new Dictionary<MethodBody, bool>();
            _typeMapHandler = new TypeMapHandler();
        }

        public AnnotationStore Annotations => Context.Annotations;
        public MarkingHelpers MarkingHelpers => Context.MarkingHelpers;
        public Tracer Tracer => Context.Tracer;
        public EmbeddedXmlInfo EmbeddedXmlInfo => Context.EmbeddedXmlInfo;

        public virtual void Process(LinkContext context)
        {
            _context = context;
            _markContext = new MarkStepContext();
            _dynamicallyAccessedMembersTypeHierarchy = new DynamicallyAccessedMembersTypeHierarchy(_context, this);

            Initialize();
            Process();
            Complete();
        }

        protected virtual void Initialize()
        {
            InitializeCorelibAttributeXml();
            Context.Pipeline.InitializeMarkHandlers(Context, MarkContext);

            if (Annotations.GetEntryPointAssembly() is AssemblyDefinition entryPoint)
            {
                _typeMapHandler = new TypeMapHandler(entryPoint);
            }

            _typeMapHandler.Initialize(Context, this);

            ProcessMarkedPending();
        }

        void InitializeCorelibAttributeXml()
        {
            // Pre-load corelib and process its attribute XML first. This is necessary because the
            // corelib attribute XML can contain modifications to other assemblies.
            // We could just mark it here, but the attribute processing isn't necessarily tied to marking,
            // so this would rely on implementation details of corelib.
            var coreLib = Context.TryResolve(PlatformAssemblies.CoreLib);
            if (coreLib == null)
                return;

            var xmlInfo = EmbeddedXmlInfo.ProcessAttributes(coreLib, Context);
            if (xmlInfo == null)
                return;

            // Because the attribute XML can reference other assemblies, they must go in the global store,
            // instead of the per-assembly stores.
            foreach (var (provider, annotations) in xmlInfo.CustomAttributes)
                Context.CustomAttributes.PrimaryAttributeInfo.AddCustomAttributes(provider, annotations);

            foreach (var (ca, origin) in xmlInfo.CustomAttributesOrigins)
                Context.CustomAttributes.PrimaryAttributeInfo.CustomAttributesOrigins.Add(ca, origin);
        }

        protected virtual void Complete()
        {
            foreach ((var body, var _) in _unreachableBodies)
            {
                Annotations.SetAction(body.Method, MethodAction.ConvertToThrow);
            }
        }

        static bool TypeIsDynamicInterfaceCastableImplementation(TypeDefinition type)
        {
            if (!type.IsInterface || !type.HasInterfaces || !type.HasCustomAttributes)
                return false;

            foreach (var ca in type.CustomAttributes)
            {
                if (ca.AttributeType.IsTypeOf("System.Runtime.InteropServices", "DynamicInterfaceCastableImplementationAttribute"))
                    return true;
            }
            return false;
        }

        protected bool IsFullyPreserved(TypeDefinition type)
        {
            if (Annotations.TryGetPreserve(type, out TypePreserve preserve) && preserve == TypePreserve.All)
                return true;

            switch (Annotations.GetAction(type.Module.Assembly))
            {
                case AssemblyAction.Save:
                case AssemblyAction.Copy:
                case AssemblyAction.CopyUsed:
                case AssemblyAction.AddBypassNGen:
                case AssemblyAction.AddBypassNGenUsed:
                    return true;
            }

            return false;
        }

        internal void MarkEntireType(TypeDefinition type, in DependencyInfo reason, MessageOrigin origin)
        {
#if DEBUG
            if (!_entireTypeReasons.Contains(reason.Kind))
                throw new InternalErrorException($"Unsupported type dependency '{reason.Kind}'.");
#endif

            // Prevent cases where there's nothing on the stack (can happen when marking entire assemblies)
            // In which case we would generate warnings with no source (hard to debug)
            origin = origin.Provider is not null ? origin : new MessageOrigin(type);

            if (!_entireTypesMarked.Add(type))
                return;

            if (type.HasNestedTypes)
            {
                foreach (TypeDefinition nested in type.NestedTypes)
                    MarkEntireType(nested, new DependencyInfo(DependencyKind.NestedType, type), origin);
            }

            MarkTypeVisibleToReflection(type, reason, origin);

            if (type.HasInterfaces)
            {
                foreach (InterfaceImplementation iface in type.Interfaces)
                    MarkInterfaceImplementation(iface, new MessageOrigin(type));
            }

            MarkGenericParameterProvider(type, origin);

            MarkFieldsVisibleToReflection(type, new DependencyInfo(DependencyKind.MemberOfType, type), origin);
            MarkMethodsVisibleToReflection(type, new DependencyInfo(DependencyKind.MemberOfType, type), origin);

            if (type.HasProperties)
            {
                foreach (var property in type.Properties)
                {
                    MarkPropertyVisibleToReflection(property, new DependencyInfo(DependencyKind.MemberOfType, type), origin);
                }
            }

            if (type.HasEvents)
            {
                foreach (var ev in type.Events)
                {
                    MarkEventVisibleToReflection(ev, new DependencyInfo(DependencyKind.MemberOfType, type), origin);
                }
            }
        }

        void Process()
        {
            while (ProcessPrimaryQueue() ||
                ProcessMarkedPending() ||
                ProcessLazyAttributes() ||
                ProcessLateMarkedAttributes() ||
                MarkFullyPreservedAssemblies()) ;

            ProcessPendingTypeChecks();
        }

        static bool IsFullyPreservedAction(AssemblyAction action) => action == AssemblyAction.Copy || action == AssemblyAction.Save;

        bool MarkFullyPreservedAssemblies()
        {
            // Fully mark any assemblies with copy/save action.

            // Unresolved references could get the copy/save action if this is the default action.
            bool scanReferences = IsFullyPreservedAction(Context.TrimAction) || IsFullyPreservedAction(Context.DefaultAction);

            if (!scanReferences)
            {
                // Unresolved references could get the copy/save action if it was set explicitly
                // for some referenced assembly that has not been resolved yet
                foreach (var (assemblyName, action) in Context.Actions)
                {
                    if (!IsFullyPreservedAction(action))
                        continue;

                    var assembly = Context.GetLoadedAssembly(assemblyName);
                    if (assembly == null)
                    {
                        scanReferences = true;
                        break;
                    }

                    // The action should not change from the explicit command-line action
                    Debug.Assert(Annotations.GetAction(assembly) == action);
                }
            }

            // Setup empty origin - there has to be some origin setup since we're doing marking below
            // but there's no "origin" right now (command line is the origin really)
            var emptyOrigin = new MessageOrigin(null as ICustomAttributeProvider);

            // Beware: this works on loaded assemblies, not marked assemblies, so it should not be tied to marking.
            // We could further optimize this to only iterate through assemblies if the last mark iteration loaded
            // a new assembly, since this is the only way that the set we need to consider could have changed.
            var assembliesToCheck = scanReferences ? Context.GetReferencedAssemblies().ToArray() : Context.GetAssemblies();
            bool markedNewAssembly = false;
            foreach (var assembly in assembliesToCheck)
            {
                var action = Annotations.GetAction(assembly);
                if (!IsFullyPreservedAction(action))
                    continue;
                if (!Annotations.IsProcessed(assembly))
                    markedNewAssembly = true;
                MarkAssembly(assembly, new DependencyInfo(DependencyKind.AssemblyAction, null), emptyOrigin);
            }
            return markedNewAssembly;
        }

        bool ProcessPrimaryQueue()
        {
            if (QueueIsEmpty())
                return false;

            while (!QueueIsEmpty())
            {
                ProcessQueue();
                ProcessMarkedTypesWithInterfaces();
                ProcessInterfaceMethods();
                ProcessDynamicCastableImplementationInterfaces();
                ProcessPendingBodies();
                DoAdditionalProcessing();
            }

            return true;
        }

        bool ProcessMarkedPending()
        {
            bool marked = false;
            foreach (var pending in Annotations.GetMarkedPending())
            {
                marked = true;

                // Some pending items might be processed by the time we get to them.
                if (Annotations.IsProcessed(pending.Key))
                    continue;

                switch (pending.Key)
                {
                    case TypeDefinition type:
                        MarkType(type, DependencyInfo.AlreadyMarked, pending.Value);
                        break;
                    case MethodDefinition method:
                        MarkMethod(method, DependencyInfo.AlreadyMarked, pending.Value);
                        // Methods will not actually be processed until we drain the method queue.
                        break;
                    case FieldDefinition field:
                        MarkField(field, DependencyInfo.AlreadyMarked, pending.Value);
                        break;
                    case ModuleDefinition module:
                        MarkModule(module, DependencyInfo.AlreadyMarked, pending.Value);
                        break;
                    case ExportedType exportedType:
                        Annotations.SetProcessed(exportedType);
                        // No additional processing is done for exported types.
                        break;
                    default:
                        throw new NotImplementedException(pending.GetType().ToString());
                }
            }

            foreach (var type in Annotations.GetPendingPreserve())
            {
                marked = true;
                ApplyPreserveInfo(type);
            }

            return marked;
        }

        void ProcessPendingTypeChecks()
        {
            for (int i = 0; i < _pending_isinst_instr.Count; ++i)
            {
                var item = _pending_isinst_instr[i];
                TypeDefinition type = item.Type;
                if (Annotations.IsInstantiated(type))
                    continue;

                Instruction instr = item.Instr;
                LinkerILProcessor ilProcessor = item.Body.GetLinkerILProcessor();

                ilProcessor.InsertAfter(instr, Instruction.Create(OpCodes.Ldnull));
                Instruction new_instr = Instruction.Create(OpCodes.Pop);
                ilProcessor.Replace(instr, new_instr);

                Context.LogMessage($"Removing typecheck of '{type.FullName}' inside '{item.Body.Method.GetDisplayName()}' method.");
            }
        }

        void ProcessQueue()
        {
            while (!QueueIsEmpty())
            {
                (MethodDefinition method, DependencyInfo reason) = _methods.Dequeue();
                ProcessMethod(method, reason);
            }
        }

        bool QueueIsEmpty()
        {
            return _methods.Count == 0;
        }

        protected virtual void EnqueueMethod(MethodDefinition method, in DependencyInfo reason)
        {
            _methods.Enqueue((method, reason));
        }

        void ProcessInterfaceMethods()
        {
            foreach ((var method, var origin) in _interface_methods)
            {
                ProcessInterfaceMethod(method, origin);
            }
        }

        /// <summary>
        /// Handles marking of interface implementations, and the marking of methods that implement interfaces
        /// once ILLink knows whether a type is instantiated or relevant to variant casting,
        /// and after interfaces and interface methods have been marked.
        /// </summary>
        void ProcessMarkedTypesWithInterfaces()
        {
            // We may mark an interface type later on.  Which means we need to reprocess any time with one or more interface implementations that have not been marked
            // and if an interface type is found to be marked and implementation is not marked, then we need to mark that implementation

            for (int i = 0; i < _typesWithInterfaces.Count; i++)
            {
                (var type, var origin) = _typesWithInterfaces[i];
                // Exception, types that have not been flagged as instantiated yet.  These types may not need their interfaces even if the
                // interface type is marked
                // UnusedInterfaces optimization is turned off mark all interface implementations
                bool unusedInterfacesOptimizationEnabled = Context.IsOptimizationEnabled(CodeOptimizations.UnusedInterfaces, type);
                if (Annotations.IsInstantiated(type) || Annotations.IsRelevantToVariantCasting(type) ||
                    !unusedInterfacesOptimizationEnabled)
                {
                    MarkInterfaceImplementations(type);
                }
                // Interfaces in PreservedScope should have their methods added to _virtual_methods so that they are properly processed
                foreach (var method in type.Methods)
                {
                    var baseMethods = Annotations.GetBaseMethods(method);
                    if (baseMethods is null)
                        continue;
                    foreach (var ov in baseMethods)
                    {
                        if (ov.Base.DeclaringType is not null && ov.Base.DeclaringType.IsInterface && IgnoreScope(ov.Base.DeclaringType.Scope))
                        {
                            MarkMethodAsVirtual(ov.Base, origin);
                        }
                    }
                }
            }
        }

        void DiscoverDynamicCastableImplementationInterfaces()
        {
            // We could potentially avoid loading all references here: https://github.com/dotnet/linker/issues/1788
            foreach (var assembly in Context.GetReferencedAssemblies().ToArray())
            {
                switch (Annotations.GetAction(assembly))
                {
                    // We only need to search assemblies where we don't mark everything
                    // Assemblies that are fully marked already mark these types.
                    case AssemblyAction.Link:
                    case AssemblyAction.AddBypassNGen:
                    case AssemblyAction.AddBypassNGenUsed:
                        if (!_dynamicInterfaceCastableImplementationTypesDiscovered.Add(assembly))
                            continue;

                        foreach (TypeDefinition type in assembly.MainModule.Types)
                            CheckIfTypeOrNestedTypesIsDynamicCastableImplementation(type);

                        break;
                }
            }

            void CheckIfTypeOrNestedTypesIsDynamicCastableImplementation(TypeDefinition type)
            {
                if (!Annotations.IsMarked(type) && TypeIsDynamicInterfaceCastableImplementation(type))
                    _dynamicInterfaceCastableImplementationTypes.Add(type);

                if (type.HasNestedTypes)
                {
                    foreach (var nestedType in type.NestedTypes)
                        CheckIfTypeOrNestedTypesIsDynamicCastableImplementation(nestedType);
                }
            }
        }

        void ProcessDynamicCastableImplementationInterfaces()
        {
            DiscoverDynamicCastableImplementationInterfaces();

            // We may mark an interface type later on.  Which means we need to reprocess any time with one or more interface implementations that have not been marked
            // and if an interface type is found to be marked and implementation is not marked, then we need to mark that implementation

            for (int i = 0; i < _dynamicInterfaceCastableImplementationTypes.Count; i++)
            {
                var type = _dynamicInterfaceCastableImplementationTypes[i];

                Debug.Assert(TypeIsDynamicInterfaceCastableImplementation(type));

                // If the type has already been marked, we can remove it from this list.
                if (Annotations.IsMarked(type))
                {
                    _dynamicInterfaceCastableImplementationTypes.RemoveAt(i--);
                    continue;
                }

                foreach (var iface in type.Interfaces)
                {
                    if (Annotations.IsMarked(iface.InterfaceType))
                    {
                        // We only need to mark the type definition because ILLink will ensure that all marked implemented interfaces and used method implementations
                        // will be marked on this type as well.
                        MarkType(type, new DependencyInfo(DependencyKind.DynamicInterfaceCastableImplementation, iface.InterfaceType), new MessageOrigin(Context.TryResolve(iface.InterfaceType)));

                        _dynamicInterfaceCastableImplementationTypes.RemoveAt(i--);
                        break;
                    }
                }
            }
        }

        void ProcessPendingBodies()
        {
            for (int i = 0; i < _unreachableBodies.Count; i++)
            {
                (var body, var origin) = _unreachableBodies[i];
                if (Annotations.IsInstantiated(body.Method.DeclaringType))
                {
                    MarkMethodBody(body, origin);

                    _unreachableBodies.RemoveAt(i--);
                }
            }
        }

        void MarkMethodAsVirtual(MethodDefinition method, MessageOrigin origin)
        {
            Annotations.EnqueueVirtualMethod(method);
            if (method.DeclaringType.IsInterface)
            {
                _interface_methods.TryAdd(method, origin);
            }
        }

        void ProcessInterfaceMethod(MethodDefinition method, MessageOrigin origin)
        {
            Debug.Assert(method.DeclaringType.IsInterface);
            var defaultImplementations = Annotations.GetDefaultInterfaceImplementations(method);
            if (defaultImplementations is not null)
            {
                foreach (var dimInfo in defaultImplementations)
                {
                    ProcessDefaultImplementation(dimInfo, origin);

                    if (IsInterfaceImplementationMethodNeededByTypeDueToInterface(dimInfo))
                        MarkMethod(dimInfo.Override, new DependencyInfo(DependencyKind.Override, dimInfo.Base), origin);
                }
            }
            List<OverrideInformation>? overridingMethods = (List<OverrideInformation>?)Annotations.GetOverrides(method);
            if (overridingMethods is not null)
            {
                for (int i = 0; i < overridingMethods.Count; i++)
                {
                    OverrideInformation ov = overridingMethods[i];
                    if (IsInterfaceImplementationMethodNeededByTypeDueToInterface(ov))
                        MarkMethod(ov.Override, new DependencyInfo(DependencyKind.Override, ov.Base), origin);
                }
            }
        }

        /// <summary>
        /// Returns true if the Override in <paramref name="overrideInformation"/> should be marked because it is needed by the base method.
        /// Does not take into account if the base method is in a preserved scope.
        /// Assumes the base method is marked or comes from a preserved scope.
        /// </summary>
        // TODO: Move interface method marking logic here https://github.com/dotnet/linker/issues/3090
        bool ShouldMarkOverrideForBase(OverrideInformation overrideInformation)
        {
            Debug.Assert(Annotations.IsMarked(overrideInformation.Base) || IgnoreScope(overrideInformation.Base.DeclaringType.Scope));
            if (!Annotations.IsMarked(overrideInformation.Override.DeclaringType))
                return false;
            if (overrideInformation.IsOverrideOfInterfaceMember)
                return false;

            if (!Context.IsOptimizationEnabled(CodeOptimizations.OverrideRemoval, overrideInformation.Override))
                return true;

            // In this context, an override needs to be kept if
            // a) it's an override on an instantiated type (of a marked base) or
            // b) it's an override of an abstract base (required for valid IL)
            if (Annotations.IsInstantiated(overrideInformation.Override.DeclaringType))
                return true;

            // Direct overrides of marked abstract ov.Overrides must be marked or we get invalid IL.
            // Overrides further in the hierarchy will override the direct override (which will be implemented by the above rule), so we don't need to worry about invalid IL.
            if (overrideInformation.Base.IsAbstract)
                return true;

            return false;
        }

        /// <summary>
        /// Marks the Override of <paramref name="overrideInformation"/> with the correct reason. Should be called when <see cref="ShouldMarkOverrideForBase(OverrideInformation, bool)"/> returns true.
        /// </summary>
        // TODO: Take into account a base method in preserved scope
        void MarkOverrideForBaseMethod(OverrideInformation overrideInformation, MessageOrigin origin)
        {
            Debug.Assert(ShouldMarkOverrideForBase(overrideInformation));
            if (Context.IsOptimizationEnabled(CodeOptimizations.OverrideRemoval, overrideInformation.Override) && Annotations.IsInstantiated(overrideInformation.Override.DeclaringType))
            {
                MarkMethod(overrideInformation.Override, new DependencyInfo(DependencyKind.OverrideOnInstantiatedType, overrideInformation.Override.DeclaringType), origin);
            }
            else
            {
                // If the optimization is disabled or it's an abstract type, we just mark it as a normal override.
                Debug.Assert(!Context.IsOptimizationEnabled(CodeOptimizations.OverrideRemoval, overrideInformation.Override) || overrideInformation.Base.IsAbstract);
                MarkMethod(overrideInformation.Override, new DependencyInfo(DependencyKind.Override, overrideInformation.Base), origin);
            }
        }

        void MarkMethodIfNeededByBaseMethod(MethodDefinition method, MessageOrigin origin)
        {
            Debug.Assert(Annotations.IsMarked(method.DeclaringType));

            var bases = Annotations.GetBaseMethods(method);
            if (bases is null)
                return;

            var markedBaseMethods = bases.Where(ov => Annotations.IsMarked(ov.Base) || IgnoreScope(ov.Base.DeclaringType.Scope));
            foreach (var ov in markedBaseMethods)
            {
                if (ShouldMarkOverrideForBase(ov))
                    MarkOverrideForBaseMethod(ov, origin);
            }
        }

        /// <summary>
        /// Returns true if <paramref name="type"/> implements <paramref name="interfaceType"/> and the interface implementation is marked,
        /// or if any marked interface implementations on <paramref name="type"/> are interfaces that implement <paramref name="interfaceType"/> and that interface implementation is marked
        /// </summary>
        bool IsInterfaceImplementationMarkedRecursively(TypeDefinition type, TypeDefinition interfaceType)
            => IsInterfaceImplementationMarkedRecursively(type, interfaceType, Context);

        /// <summary>
        /// Returns true if <paramref name="type"/> implements <paramref name="interfaceType"/> and the interface implementation is marked,
        /// or if any marked interface implementations on <paramref name="type"/> are interfaces that implement <paramref name="interfaceType"/> and that interface implementation is marked
        /// </summary>
        internal static bool IsInterfaceImplementationMarkedRecursively(TypeDefinition type, TypeDefinition interfaceType, LinkContext context)
        {
            if (type.HasInterfaces)
            {
                foreach (var intf in type.Interfaces)
                {
                    TypeDefinition? resolvedInterface = context.Resolve(intf.InterfaceType);
                    if (resolvedInterface == null)
                        continue;
                    if (!context.Annotations.IsMarked(intf))
                        continue;

                    if (resolvedInterface == interfaceType)
                        return true;
                    if (IsInterfaceImplementationMarkedRecursively(resolvedInterface, interfaceType, context))
                        return true;
                }
            }

            return false;
        }

        void ProcessDefaultImplementation(OverrideInformation ov, MessageOrigin origin)
        {
            Debug.Assert(ov.IsOverrideOfInterfaceMember);
            if ((!ov.Override.IsStatic && !Annotations.IsInstantiated(ov.InterfaceImplementor.Implementor))
                || ov.Override.IsStatic && !Annotations.IsRelevantToVariantCasting(ov.InterfaceImplementor.Implementor))
                return;

            MarkInterfaceImplementation(ov.InterfaceImplementor.InterfaceImplementation, origin);
        }

        void MarkMarshalSpec(IMarshalInfoProvider spec, in DependencyInfo reason, MessageOrigin origin)
        {
            if (!spec.HasMarshalInfo)
                return;

            if (spec.MarshalInfo is CustomMarshalInfo marshaler)
            {
                MarkType(marshaler.ManagedType, reason, origin);
                TypeDefinition? type = Context.Resolve(marshaler.ManagedType);
                if (type != null)
                {
                    MarkICustomMarshalerMethods(type, in reason, origin);
                    MarkCustomMarshalerGetInstance(type, in reason, origin);
                }
            }
        }

        void MarkCustomAttributes(ICustomAttributeProvider provider, in DependencyInfo reason, MessageOrigin origin)
        {
            if (provider.HasCustomAttributes)
            {
                bool providerInLinkedAssembly = Annotations.GetAction(CustomAttributeSource.GetAssemblyFromCustomAttributeProvider(provider)) == AssemblyAction.Link;
                bool markOnUse = Context.KeepUsedAttributeTypesOnly && providerInLinkedAssembly;

                foreach (CustomAttribute ca in provider.CustomAttributes)
                {
                    if (ProcessLinkerSpecialAttribute(ca, provider, reason, origin))
                        continue;

                    if (markOnUse)
                    {
                        _lateMarkedAttributes.Enqueue((new AttributeProviderPair(ca, provider), reason, origin));
                        continue;
                    }

                    var resolvedAttributeType = Context.Resolve(ca.AttributeType);
                    if (resolvedAttributeType == null)
                    {
                        continue;
                    }

                    if (providerInLinkedAssembly && IsAttributeRemoved(ca, resolvedAttributeType))
                        continue;

                    MarkCustomAttribute(ca, reason, origin);
                }
            }

            if (!(provider is MethodDefinition || provider is FieldDefinition))
                return;

            IMemberDefinition providerMember = (IMemberDefinition)provider; ;
            MessageOrigin providerOrigin = new MessageOrigin(providerMember);
            foreach (var dynamicDependency in Annotations.GetLinkerAttributes<DynamicDependency>(providerMember))
                MarkDynamicDependency(dynamicDependency, providerMember, providerOrigin);
        }

        bool IsAttributeRemoved(CustomAttribute ca, TypeDefinition attributeType)
        {
            foreach (var attr in Annotations.GetLinkerAttributes<RemoveAttributeInstancesAttribute>(attributeType))
            {
                var args = attr.Arguments;
                if (args.Length == 0)
                    return true;

                if (args.Length > ca.ConstructorArguments.Count)
                    continue;

                if (HasMatchingArguments(args, ca.ConstructorArguments))
                    return true;
            }

            return false;

            static bool HasMatchingArguments(CustomAttributeArgument[] removeAttrInstancesArgs, Collection<CustomAttributeArgument> attributeInstanceArgs)
            {
                for (int i = 0; i < removeAttrInstancesArgs.Length; ++i)
                {
                    if (!removeAttrInstancesArgs[i].IsEqualTo(attributeInstanceArgs[i]))
                        return false;
                }
                return true;
            }
        }

        protected virtual bool ProcessLinkerSpecialAttribute(CustomAttribute ca, ICustomAttributeProvider provider, in DependencyInfo reason, MessageOrigin origin)
        {
            var isPreserveDependency = IsUserDependencyMarker(ca.AttributeType);
            var isDynamicDependency = ca.AttributeType.IsTypeOf<DynamicDependencyAttribute>();

            if (!((isPreserveDependency || isDynamicDependency) && provider is IMemberDefinition member))
                return false;

            if (isPreserveDependency)
                MarkUserDependency(member, ca, origin);

            if (Context.CanApplyOptimization(CodeOptimizations.RemoveDynamicDependencyAttribute, member.DeclaringType.Module.Assembly))
            {
                // Record the custom attribute so that it has a reason, without actually marking it.
                Tracer.AddDirectDependency(ca, reason, marked: false);
            }
            else
            {
                MarkCustomAttribute(ca, reason, origin);
            }

            return true;
        }

        void MarkDynamicDependency(DynamicDependency dynamicDependency, IMemberDefinition context, MessageOrigin origin)
        {
            Debug.Assert(context is MethodDefinition || context is FieldDefinition);
            AssemblyDefinition? assembly;
            if (dynamicDependency.AssemblyName != null)
            {
                assembly = Context.TryResolve(dynamicDependency.AssemblyName);
                if (assembly == null)
                {
                    Context.LogWarning(origin, DiagnosticId.UnresolvedAssemblyInDynamicDependencyAttribute, dynamicDependency.AssemblyName);
                    return;
                }
            }
            else
            {
                assembly = context.DeclaringType.Module.Assembly;
                Debug.Assert(assembly != null);
            }

            TypeDefinition? type;
            if (dynamicDependency.TypeName is string typeName)
            {
                type = DocumentationSignatureParser.GetTypeByDocumentationSignature(assembly, typeName, Context);
                if (type == null)
                {
                    Context.LogWarning(origin, DiagnosticId.UnresolvedTypeInDynamicDependencyAttribute, typeName);
                    return;
                }

                MarkingHelpers.MarkMatchingExportedType(type, assembly, new DependencyInfo(DependencyKind.DynamicDependency, type), origin);
            }
            else if (dynamicDependency.Type is TypeReference typeReference)
            {
                type = Context.TryResolve(typeReference);
                if (type == null)
                {
                    Context.LogWarning(origin, DiagnosticId.UnresolvedTypeInDynamicDependencyAttribute, typeReference.GetDisplayName());
                    return;
                }
            }
            else
            {
                type = Context.TryResolve(context.DeclaringType);
                if (type == null)
                {
                    Context.LogWarning(context, DiagnosticId.UnresolvedTypeInDynamicDependencyAttribute, context.DeclaringType.GetDisplayName());
                    return;
                }
            }

            IEnumerable<IMetadataTokenProvider> members;
            if (dynamicDependency.MemberSignature is string memberSignature)
            {
                members = DocumentationSignatureParser.GetMembersByDocumentationSignature(type, memberSignature, Context, acceptName: true);
                if (!members.Any())
                {
                    Context.LogWarning(origin, DiagnosticId.NoMembersResolvedForMemberSignatureOrType, memberSignature, type.GetDisplayName());
                    return;
                }
            }
            else
            {
                var memberTypes = dynamicDependency.MemberTypes;
                members = type.GetDynamicallyAccessedMembers(Context, memberTypes);
                if (!members.Any())
                {
                    Context.LogWarning(origin, DiagnosticId.NoMembersResolvedForMemberSignatureOrType, memberTypes.ToString(), type.GetDisplayName());
                    return;
                }
            }

            MarkMembersVisibleToReflection(members, new DependencyInfo(DependencyKind.DynamicDependency, context), origin);
        }

        void MarkMembersVisibleToReflection(IEnumerable<IMetadataTokenProvider> members, in DependencyInfo reason, MessageOrigin origin)
        {
            foreach (var member in members)
            {
                switch (member)
                {
                    case TypeDefinition type:
                        MarkTypeVisibleToReflection(type, reason, origin);
                        break;
                    case MethodDefinition method:
                        MarkMethodVisibleToReflection(method, reason, origin);
                        break;
                    case FieldDefinition field:
                        MarkFieldVisibleToReflection(field, reason, origin);
                        break;
                    case PropertyDefinition property:
                        MarkPropertyVisibleToReflection(property, reason, origin);
                        break;
                    case EventDefinition @event:
                        MarkEventVisibleToReflection(@event, reason, origin);
                        break;
                    case InterfaceImplementation interfaceType:
                        MarkInterfaceImplementation(interfaceType, origin, reason);
                        break;
                }
            }
        }

        protected virtual bool IsUserDependencyMarker(TypeReference type)
        {
            return type.Name == "PreserveDependencyAttribute" && type.Namespace == "System.Runtime.CompilerServices";
        }

        protected virtual void MarkUserDependency(IMemberDefinition context, CustomAttribute ca, MessageOrigin origin)
        {
            Context.LogWarning(context, DiagnosticId.DeprecatedPreserveDependencyAttribute);

            if (!DynamicDependency.ShouldProcess(Context, ca))
                return;

            AssemblyDefinition? assembly;
            var args = ca.ConstructorArguments;
            if (args.Count >= 3 && args[2].Value is string assemblyName)
            {
                assembly = Context.TryResolve(assemblyName);
                if (assembly == null)
                {
                    Context.LogWarning(context, DiagnosticId.CouldNotResolveDependencyAssembly, assemblyName);
                    return;
                }
            }
            else
            {
                assembly = null;
            }

            TypeDefinition? td;
            if (args.Count >= 2 && args[1].Value is string typeName)
            {
                AssemblyDefinition assemblyDef = assembly ?? ((MemberReference)context).Module.Assembly;
                td = Context.TryResolve(assemblyDef, typeName);

                if (td == null)
                {
                    Context.LogWarning(context, DiagnosticId.CouldNotResolveDependencyType, typeName);
                    return;
                }

                MarkingHelpers.MarkMatchingExportedType(td, assemblyDef, new DependencyInfo(DependencyKind.PreservedDependency, ca), origin);
            }
            else
            {
                td = context.DeclaringType;
            }

            string? member = null;
            string[]? signature = null;
            if (args.Count >= 1 && args[0].Value is string memberSignature)
            {
                memberSignature = memberSignature.Replace(" ", "");
                var sign_start = memberSignature.IndexOf('(');
                var sign_end = memberSignature.LastIndexOf(')');
                if (sign_start > 0 && sign_end > sign_start)
                {
                    var parameters = memberSignature.Substring(sign_start + 1, sign_end - sign_start - 1);
                    signature = string.IsNullOrEmpty(parameters) ? Array.Empty<string>() : parameters.Split(',');
                    member = memberSignature.Substring(0, sign_start);
                }
                else
                {
                    member = memberSignature;
                }
            }

            if (member == "*")
            {
                MarkEntireType(td, new DependencyInfo(DependencyKind.PreservedDependency, ca), origin);
                return;
            }

            if (member != null)
            {
                if (MarkDependencyMethod(td, member, signature, new DependencyInfo(DependencyKind.PreservedDependency, ca), origin))
                    return;

                if (MarkNamedField(td, member, new DependencyInfo(DependencyKind.PreservedDependency, ca), origin))
                    return;
            }

            Context.LogWarning(context, DiagnosticId.CouldNotResolveDependencyMember, member ?? "", td.GetDisplayName());
        }

        bool MarkDependencyMethod(TypeDefinition type, string name, string[]? signature, in DependencyInfo reason, MessageOrigin origin)
        {
            bool marked = false;

            int arity_marker = name.IndexOf('`');
            if (arity_marker < 1 || !int.TryParse(name.AsSpan(arity_marker + 1), out int arity))
            {
                arity = 0;
            }
            else
            {
                name = name.Substring(0, arity_marker);
            }

            foreach (var m in type.Methods)
            {
                if (m.Name != name)
                    continue;

                if (m.GenericParameters.Count != arity)
                    continue;

                if (signature == null)
                {
                    MarkMethodVisibleToReflection(m, reason, origin);
                    marked = true;
                    continue;
                }

                if (m.GetMetadataParametersCount() != signature.Length)
                    continue;

                bool matched = true;
                foreach (var p in m.GetMetadataParameters())
                {
                    if (p.ParameterType.FullName != signature[p.MetadataIndex].Trim().ToCecilName())
                    {
                        matched = false;
                        break;
                    }
                }

                if (!matched)
                    continue;

                MarkMethodVisibleToReflection(m, reason, origin);
                marked = true;
            }

            return marked;
        }

        void LazyMarkCustomAttributes(ICustomAttributeProvider provider)
        {
            Debug.Assert(provider is ModuleDefinition or AssemblyDefinition);
            if (!provider.HasCustomAttributes)
                return;

            foreach (CustomAttribute ca in provider.CustomAttributes)
            {
                _assemblyLevelAttributes.Enqueue(new AttributeProviderPair(ca, provider));
            }
        }

        protected internal virtual void MarkCustomAttribute(CustomAttribute ca, in DependencyInfo reason, MessageOrigin origin)
        {
            Annotations.Mark(ca, reason);
            MarkMethod(ca.Constructor, new DependencyInfo(DependencyKind.AttributeConstructor, ca), origin);

            MarkCustomAttributeArguments(ca, origin);

            TypeReference constructor_type = ca.Constructor.DeclaringType;
            TypeDefinition? type = Context.Resolve(constructor_type);

            if (type == null)
            {
                return;
            }

            MarkCustomAttributeProperties(ca, type, origin);
            MarkCustomAttributeFields(ca, type, origin);
        }

        protected virtual bool ShouldMarkCustomAttribute(CustomAttribute ca, ICustomAttributeProvider provider)
        {
            var attr_type = ca.AttributeType;

            if (Context.KeepUsedAttributeTypesOnly)
            {
                switch (attr_type.FullName)
                {
                    // These are required by the runtime
                    case "System.ThreadStaticAttribute":
                    case "System.ContextStaticAttribute":
                    case "System.Runtime.CompilerServices.IsByRefLikeAttribute":
                        return true;
                    // Attributes related to `fixed` keyword used to declare fixed length arrays
                    case "System.Runtime.CompilerServices.FixedBufferAttribute":
                        return true;
                    case "System.Runtime.InteropServices.InterfaceTypeAttribute":
                    case "System.Runtime.InteropServices.GuidAttribute":
                        return true;
                    // May be implicitly used by the runtime
                    case "System.Runtime.CompilerServices.InternalsVisibleToAttribute":
                        return true;
                }

                TypeDefinition? type = Context.Resolve(attr_type);
                if (type is null || !Annotations.IsMarked(type))
                    return false;
            }

            return true;
        }

        protected virtual bool ShouldMarkTypeStaticConstructor(TypeDefinition type)
        {
            if (Annotations.HasPreservedStaticCtor(type))
                return false;

            if (type.IsBeforeFieldInit && Context.IsOptimizationEnabled(CodeOptimizations.BeforeFieldInit, type))
                return false;

            return true;
        }

        protected internal void MarkStaticConstructor(TypeDefinition type, in DependencyInfo reason, in MessageOrigin origin)
        {
            if (MarkMethodIf(type.Methods, IsNonEmptyStaticConstructor, reason, origin) != null)
                Annotations.SetPreservedStaticCtor(type);
        }

        protected virtual bool ShouldMarkTopLevelCustomAttribute(AttributeProviderPair app, MethodDefinition resolvedConstructor)
        {
            var ca = app.Attribute;

            if (!ShouldMarkCustomAttribute(app.Attribute, app.Provider))
                return false;

            // If an attribute's module has not been marked after processing all types in all assemblies and the attribute itself has not been marked,
            // then surely nothing is using this attribute and there is no need to mark it
            if (!Annotations.IsMarked(resolvedConstructor.Module) &&
                !Annotations.IsMarked(ca.AttributeType) &&
                Annotations.GetAction(resolvedConstructor.Module.Assembly) == AssemblyAction.Link)
                return false;

            if (ca.Constructor.DeclaringType.Namespace == "System.Diagnostics")
            {
                string attributeName = ca.Constructor.DeclaringType.Name;
                if (attributeName == "DebuggerDisplayAttribute" || attributeName == "DebuggerTypeProxyAttribute")
                {
                    var displayTargetType = GetDebuggerAttributeTargetType(app.Attribute, (AssemblyDefinition)app.Provider);
                    if (displayTargetType == null || !Annotations.IsMarked(displayTargetType))
                        return false;
                }
            }

            return true;
        }

        protected void MarkSecurityDeclarations(ISecurityDeclarationProvider provider, in DependencyInfo reason, MessageOrigin origin)
        {
            // most security declarations are removed (if linked) but user code might still have some
            // and if the attributes references types then they need to be marked too
            if ((provider == null) || !provider.HasSecurityDeclarations)
                return;

            foreach (var sd in provider.SecurityDeclarations)
                MarkSecurityDeclaration(sd, reason, origin);
        }

        protected virtual void MarkSecurityDeclaration(SecurityDeclaration sd, in DependencyInfo reason, MessageOrigin origin)
        {
            if (!sd.HasSecurityAttributes)
                return;

            foreach (var sa in sd.SecurityAttributes)
                MarkSecurityAttribute(sa, reason, origin);
        }

        protected virtual void MarkSecurityAttribute(SecurityAttribute sa, in DependencyInfo reason, MessageOrigin origin)
        {
            TypeReference security_type = sa.AttributeType;
            TypeDefinition? type = Context.Resolve(security_type);
            if (type == null)
            {
                return;
            }

            // Security attributes participate in inference logic without being marked.
            Tracer.AddDirectDependency(sa, reason, marked: false);
            MarkType(security_type, new DependencyInfo(DependencyKind.AttributeType, sa), origin);
            MarkCustomAttributeProperties(sa, type, origin);
            MarkCustomAttributeFields(sa, type, origin);
        }

        protected void MarkCustomAttributeProperties(ICustomAttribute ca, TypeDefinition attribute, MessageOrigin origin)
        {
            if (!ca.HasProperties)
                return;

            foreach (var named_argument in ca.Properties)
                MarkCustomAttributeProperty(named_argument, attribute, ca, new DependencyInfo(DependencyKind.AttributeProperty, ca), origin);
        }

        protected void MarkCustomAttributeProperty(CustomAttributeNamedArgument namedArgument, TypeDefinition attribute, ICustomAttribute ca, in DependencyInfo reason, MessageOrigin origin)
        {
            PropertyDefinition? property = GetProperty(attribute, namedArgument.Name);
            if (property != null)
                MarkMethod(property.SetMethod, reason, origin);

            MarkCustomAttributeArgument(namedArgument.Argument, ca, origin);

            if (property != null && Annotations.FlowAnnotations.RequiresDataFlowAnalysis(property.SetMethod))
            {
                var scanner = new AttributeDataFlow(Context, this, origin);
                scanner.ProcessAttributeDataflow(property.SetMethod, new List<CustomAttributeArgument> { namedArgument.Argument });
            }
        }

        PropertyDefinition? GetProperty(TypeDefinition inputType, string propertyname)
        {
            TypeDefinition? type = inputType;
            while (type != null)
            {
                PropertyDefinition? property = type.Properties.FirstOrDefault(p => p.Name == propertyname);
                if (property != null)
                    return property;

                type = Context.TryResolve(type.BaseType);
            }

            return null;
        }

        protected void MarkCustomAttributeFields(ICustomAttribute ca, TypeDefinition attribute, MessageOrigin origin)
        {
            if (!ca.HasFields)
                return;

            foreach (var named_argument in ca.Fields)
                MarkCustomAttributeField(named_argument, attribute, ca, origin);
        }

        protected void MarkCustomAttributeField(CustomAttributeNamedArgument namedArgument, TypeDefinition attribute, ICustomAttribute ca, MessageOrigin origin)
        {
            FieldDefinition? field = GetField(attribute, namedArgument.Name);
            if (field != null)
                MarkField(field, new DependencyInfo(DependencyKind.CustomAttributeField, ca), origin);

            MarkCustomAttributeArgument(namedArgument.Argument, ca, origin);

            if (field != null && Annotations.FlowAnnotations.RequiresDataFlowAnalysis(field))
            {
                var scanner = new AttributeDataFlow(Context, this, origin);
                scanner.ProcessAttributeDataflow(field, namedArgument.Argument);
            }
        }

        FieldDefinition? GetField(TypeDefinition inputType, string fieldname)
        {
            TypeDefinition? type = inputType;
            while (type != null)
            {
                FieldDefinition? field = type.Fields.FirstOrDefault(f => f.Name == fieldname);
                if (field != null)
                    return field;

                type = Context.TryResolve(type.BaseType);
            }

            return null;
        }

        MethodDefinition? GetMethodWithNoParameters(TypeDefinition inputType, string methodname)
        {
            TypeDefinition? type = inputType;
            while (type != null)
            {
                MethodDefinition? method = type.Methods.FirstOrDefault(m => m.Name == methodname && !m.HasMetadataParameters());
                if (method != null)
                    return method;

                type = Context.TryResolve(type.BaseType);
            }

            return null;
        }

        void MarkCustomAttributeArguments(CustomAttribute ca, MessageOrigin origin)
        {
            if (!ca.HasConstructorArguments)
                return;

            foreach (var argument in ca.ConstructorArguments)
                MarkCustomAttributeArgument(argument, ca, origin);

            var resolvedConstructor = Context.TryResolve(ca.Constructor);
            if (resolvedConstructor != null && Annotations.FlowAnnotations.RequiresDataFlowAnalysis(resolvedConstructor))
            {
                var scanner = new AttributeDataFlow(Context, this, origin);
                scanner.ProcessAttributeDataflow(resolvedConstructor, ca.ConstructorArguments);
            }
        }

        void MarkCustomAttributeArgument(CustomAttributeArgument argument, ICustomAttribute ca, MessageOrigin origin)
        {
            var at = argument.Type;

            if (at.IsArray)
            {
                var et = at.GetElementType();

                MarkType(et, new DependencyInfo(DependencyKind.CustomAttributeArgumentType, ca), origin);
                if (argument.Value == null)
                    return;

                // Array arguments are modeled as a CustomAttributeArgument [], and will mark the
                // Type once for each element in the array.
                foreach (var caa in (CustomAttributeArgument[])argument.Value)
                    MarkCustomAttributeArgument(caa, ca, origin);

                return;
            }

            if (at.Namespace == "System")
            {
                switch (at.Name)
                {
                    case "Type":
                        MarkType(argument.Type, new DependencyInfo(DependencyKind.CustomAttributeArgumentType, ca), origin);
                        MarkType((TypeReference)argument.Value, new DependencyInfo(DependencyKind.CustomAttributeArgumentValue, ca), origin);
                        return;

                    case "Object":
                        var boxed_value = (CustomAttributeArgument)argument.Value;
                        MarkType(boxed_value.Type, new DependencyInfo(DependencyKind.CustomAttributeArgumentType, ca), origin);
                        MarkCustomAttributeArgument(boxed_value, ca, origin);
                        return;
                }
            }
        }

        protected bool CheckProcessed(IMetadataTokenProvider provider)
        {
            return !Annotations.SetProcessed(provider);
        }

        protected virtual void MarkAssembly(AssemblyDefinition assembly, DependencyInfo reason, MessageOrigin origin)
        {
            Annotations.Mark(assembly, reason, origin);
            if (CheckProcessed(assembly))
                return;

            var assemblyOrigin = new MessageOrigin(assembly);

            EmbeddedXmlInfo.ProcessDescriptors(assembly, Context);

            foreach (Action<AssemblyDefinition> handleMarkAssembly in MarkContext.MarkAssemblyActions)
                handleMarkAssembly(assembly);

            // Security attributes do not respect the attributes XML
            if (Context.StripSecurity)
                RemoveSecurity.ProcessAssembly(assembly, Context);

            MarkExportedTypesTarget.ProcessAssembly(assembly, Context);

            var action = Annotations.GetAction(assembly);
            if (ProcessReferencesStep.IsFullyPreservedAction(action))
            {
                if (action != AssemblyAction.Copy ||
                    !Context.TryGetCustomData("DisableMarkingOfCopyAssemblies", out string? disableMarkingOfCopyAssembliesValue) ||
                    disableMarkingOfCopyAssembliesValue != "true")
                {
                    MarkEntireAssembly(assembly, assemblyOrigin);
                }
                return;
            }
            else if (Annotations.IsRootAssembly(assembly))
            {
                MarkEntireAssembly(assembly, assemblyOrigin);
            }

            ProcessModuleType(assembly, origin);

            LazyMarkCustomAttributes(assembly);

            MarkSecurityDeclarations(assembly, new DependencyInfo(DependencyKind.AssemblyOrModuleAttribute, assembly), assemblyOrigin);

            foreach (ModuleDefinition module in assembly.Modules)
                LazyMarkCustomAttributes(module);
        }

        void MarkEntireAssembly(AssemblyDefinition assembly, MessageOrigin origin)
        {
            Debug.Assert(Annotations.IsProcessed(assembly));

            ModuleDefinition module = assembly.MainModule;

            MarkCustomAttributes(assembly, new DependencyInfo(DependencyKind.AssemblyOrModuleAttribute, assembly), origin);
            MarkCustomAttributes(module, new DependencyInfo(DependencyKind.AssemblyOrModuleAttribute, module), origin);

            foreach (TypeDefinition type in module.Types)
                MarkEntireType(type, new DependencyInfo(DependencyKind.TypeInAssembly, assembly), origin);

            // Mark scopes of type references and exported types.
            TypeReferenceMarker.MarkTypeReferences(assembly, MarkingHelpers);
        }

        sealed class TypeReferenceMarker : TypeReferenceWalker
        {

            readonly MarkingHelpers markingHelpers;

            TypeReferenceMarker(AssemblyDefinition assembly, MarkingHelpers markingHelpers)
                : base(assembly, walkSymbols: false)
            {
                this.markingHelpers = markingHelpers;
            }

            public static void MarkTypeReferences(AssemblyDefinition assembly, MarkingHelpers markingHelpers)
            {
                new TypeReferenceMarker(assembly, markingHelpers).Process();
            }

            protected override void ProcessTypeReference(TypeReference type)
            {
                markingHelpers.MarkForwardedScope(type, new MessageOrigin(assembly));
            }

            protected override void ProcessExportedType(ExportedType exportedType)
            {
                markingHelpers.MarkExportedType(exportedType, assembly.MainModule, new DependencyInfo(DependencyKind.ExportedType, assembly), new MessageOrigin(assembly));
                markingHelpers.MarkForwardedScope(CreateTypeReferenceForExportedTypeTarget(exportedType), new MessageOrigin(assembly));
            }

            protected override void ProcessExtra()
            {
                // Also mark the scopes of metadata typeref rows to cover any not discovered by the traversal.
                // This can happen when the compiler emits typerefs into IL which aren't strictly necessary per ECMA 335.
                foreach (TypeReference typeReference in assembly.MainModule.GetTypeReferences())
                {
                    if (!Visited!.Add(typeReference))
                        continue;
                    markingHelpers.MarkForwardedScope(typeReference, new MessageOrigin(assembly));
                }
            }

            TypeReference CreateTypeReferenceForExportedTypeTarget(ExportedType exportedType)
            {
                TypeReference? declaringTypeReference = null;
                if (exportedType.DeclaringType != null)
                {
                    declaringTypeReference = CreateTypeReferenceForExportedTypeTarget(exportedType.DeclaringType);
                }

                return new TypeReference(exportedType.Namespace, exportedType.Name, assembly.MainModule, exportedType.Scope)
                {
                    DeclaringType = declaringTypeReference
                };
            }
        }

        void ProcessModuleType(AssemblyDefinition assembly, MessageOrigin origin)
        {
            // The <Module> type may have an initializer, in which case we want to keep it.
            TypeDefinition? moduleType = assembly.MainModule.Types.FirstOrDefault(t => t.MetadataToken.RID == 1);
            if (moduleType != null && moduleType.HasMethods)
                MarkType(moduleType, new DependencyInfo(DependencyKind.TypeInAssembly, assembly), origin);
        }

        bool ProcessLazyAttributes()
        {
            if (Annotations.HasMarkedAnyIndirectlyCalledMethods() && MarkDisablePrivateReflectionAttribute())
                return true;

            var startingQueueCount = _assemblyLevelAttributes.Count;
            if (startingQueueCount == 0)
                return false;

            var skippedItems = new List<AttributeProviderPair>();
            var markOccurred = false;

            while (_assemblyLevelAttributes.Count != 0)
            {
                var assemblyLevelAttribute = _assemblyLevelAttributes.Dequeue();
                var customAttribute = assemblyLevelAttribute.Attribute;

                var provider = assemblyLevelAttribute.Provider;
                Debug.Assert(provider is ModuleDefinition or AssemblyDefinition);
                var assembly = (provider is ModuleDefinition module) ? module.Assembly : provider as AssemblyDefinition;

                MessageOrigin assemblyOrigin = new MessageOrigin(assembly);

                var resolved = Context.Resolve(customAttribute.Constructor);
                if (resolved == null)
                {
                    continue;
                }

                if (IsAttributeRemoved(customAttribute, resolved.DeclaringType) && Annotations.GetAction(CustomAttributeSource.GetAssemblyFromCustomAttributeProvider(assemblyLevelAttribute.Provider)) == AssemblyAction.Link)
                    continue;

                if (!ShouldMarkTopLevelCustomAttribute(assemblyLevelAttribute, resolved))
                {
                    skippedItems.Add(assemblyLevelAttribute);
                    continue;
                }

                markOccurred = true;
                MarkCustomAttribute(customAttribute, new DependencyInfo(DependencyKind.AssemblyOrModuleAttribute, assemblyLevelAttribute.Provider), assemblyOrigin);

                string attributeFullName = customAttribute.Constructor.DeclaringType.FullName;
                switch (attributeFullName)
                {
                    case "System.Diagnostics.DebuggerDisplayAttribute":
                    {
                        TypeDefinition? targetType = GetDebuggerAttributeTargetType(assemblyLevelAttribute.Attribute, (AssemblyDefinition)assemblyLevelAttribute.Provider);
                        if (targetType != null)
                            MarkTypeWithDebuggerDisplayAttribute(targetType, customAttribute, assemblyOrigin);
                        break;
                    }
                    case "System.Diagnostics.DebuggerTypeProxyAttribute":
                    {
                        TypeDefinition? targetType = GetDebuggerAttributeTargetType(assemblyLevelAttribute.Attribute, (AssemblyDefinition)assemblyLevelAttribute.Provider);
                        if (targetType != null)
                            MarkTypeWithDebuggerTypeProxyAttribute(targetType, customAttribute, assemblyOrigin);
                        break;
                    }
                }
            }

            // requeue the items we skipped in case we need to make another pass
            foreach (var item in skippedItems)
                _assemblyLevelAttributes.Enqueue(item);

            return markOccurred;
        }

        bool ProcessLateMarkedAttributes()
        {
            var startingQueueCount = _lateMarkedAttributes.Count;
            if (startingQueueCount == 0)
                return false;

            var skippedItems = new List<(AttributeProviderPair, DependencyInfo, MessageOrigin)>();
            var markOccurred = false;

            while (_lateMarkedAttributes.Count != 0)
            {
                var (attributeProviderPair, reason, origin) = _lateMarkedAttributes.Dequeue();
                var customAttribute = attributeProviderPair.Attribute;
                var provider = attributeProviderPair.Provider;

                var resolved = Context.Resolve(customAttribute.Constructor);
                if (resolved == null)
                {
                    continue;
                }

                if (!ShouldMarkCustomAttribute(customAttribute, provider))
                {
                    skippedItems.Add((attributeProviderPair, reason, origin));
                    continue;
                }

                markOccurred = true;
                MarkCustomAttribute(customAttribute, reason, origin);
            }

            // requeue the items we skipped in case we need to make another pass
            foreach (var item in skippedItems)
                _lateMarkedAttributes.Enqueue(item);

            return markOccurred;
        }

        protected void MarkField(FieldReference reference, DependencyInfo reason, in MessageOrigin origin)
        {
            if (reference.DeclaringType is GenericInstanceType)
            {
                Debug.Assert(reason.Kind == DependencyKind.FieldAccess || reason.Kind == DependencyKind.Ldtoken);
                // Blame the field reference (without actually marking) on the original reason.
                Tracer.AddDirectDependency(reference, reason, marked: false);
                MarkType(reference.DeclaringType, new DependencyInfo(DependencyKind.DeclaringType, reference), origin);

                // Blame the field definition that we will resolve on the field reference.
                reason = new DependencyInfo(DependencyKind.FieldOnGenericInstance, reference);
            }

            FieldDefinition? field = Context.Resolve(reference);

            if (field == null)
            {
                return;
            }

            MarkField(field, reason, origin);
        }

        void ReportWarningsForReflectionAccess(in MessageOrigin origin, MethodDefinition method, DependencyKind dependencyKind)
        {
            if (Annotations.ShouldSuppressAnalysisWarningsForRequiresUnreferencedCode(origin.Provider, out _))
                return;

            bool isReflectionAccessCoveredByRUC;
            bool isCompilerGenerated = CompilerGeneratedState.IsNestedFunctionOrStateMachineMember(method);
            bool forceRUCCheck = false;
            RequiresUnreferencedCodeAttribute? requiresUnreferencedCode;
            switch (dependencyKind)
            {
                case DependencyKind.AttributeProperty:
                    // Property assignment in an attribute instance.
                    // This case is more like a direct method call than reflection, and should
                    // be logically similar to what is done in ReflectionMethodBodyScanner for method calls.
                    isReflectionAccessCoveredByRUC = Annotations.DoesMethodRequireUnreferencedCode(method, out requiresUnreferencedCode);
                    break;

                case DependencyKind.Ldftn:
                case DependencyKind.Ldvirtftn:
                case DependencyKind.Ldtoken:
                    // Compiler generated code accessed via a token is considered a "hard" reference
                    // even though we also have to treat it as reflection access.
                    // So we need to enforce RUC check/warn in this case.
                    forceRUCCheck = true;
                    isReflectionAccessCoveredByRUC = Annotations.ShouldSuppressAnalysisWarningsForRequiresUnreferencedCode(method, out requiresUnreferencedCode);
                    break;

                case DependencyKind.AttributeConstructor:
                    // Attribute constructors for the System.Runtime.InteropServices.TypeMap*Attribute types should not
                    // enforce RUC as directly accessing the attributes themselves is not valid.
                    // They should only be accessed via the type-map APIs.
                    // Additionally, there's no way to suppress the warnings for the linker.
                    // By suppressing them here, they become analyzer-only, which is the expected behavior.
                    isReflectionAccessCoveredByRUC = Annotations.ShouldSuppressAnalysisWarningsForRequiresUnreferencedCode(method, out requiresUnreferencedCode);
                    if (origin.Provider is AssemblyDefinition && TypeMapHandler.IsTypeMapAttributeType(method.DeclaringType))
                    {
                        isReflectionAccessCoveredByRUC = false;
                    }
                    break;
                default:
                    // If the method being accessed has warnings suppressed due to Requires attributes,
                    // we need to issue a warning for the reflection access. This is true even for instance
                    // methods, which can be reflection-invoked without ever calling a constructor of the
                    // accessed type.
                    isReflectionAccessCoveredByRUC = Annotations.ShouldSuppressAnalysisWarningsForRequiresUnreferencedCode(method, out requiresUnreferencedCode);
                    break;
            }

            if (isReflectionAccessCoveredByRUC && (!isCompilerGenerated || forceRUCCheck))
                ReportRequiresUnreferencedCode(method.GetDisplayName(), requiresUnreferencedCode!, new DiagnosticContext(origin, diagnosticsEnabled: true, Context));

            bool isReflectionAccessCoveredByDAM = Annotations.FlowAnnotations.ShouldWarnWhenAccessedForReflection(method);
            if (isReflectionAccessCoveredByDAM && (!isCompilerGenerated || forceRUCCheck))
            {
                // ReflectionMethodBodyScanner handles more cases for data flow annotations
                // so don't warn for those.
                switch (dependencyKind)
                {
                    case DependencyKind.AttributeConstructor:
                    case DependencyKind.AttributeProperty:
                        break;
                    default:
                        Context.LogWarning(origin, DiagnosticId.DynamicallyAccessedMembersMethodAccessedViaReflection, method.GetDisplayName());
                        break;
                }
            }
        }

        void ReportWarningsForTypeHierarchyReflectionAccess(IMemberDefinition member, MessageOrigin origin)
        {
            Debug.Assert(member is MethodDefinition or FieldDefinition);

            // Don't check whether the current scope is a RUC type or RUC method because these warnings
            // are not suppressed in RUC scopes. Here the scope represents the DynamicallyAccessedMembers
            // annotation on a type, not a callsite which uses the annotation. We always want to warn about
            // possible reflection access indicated by these annotations.

            var type = origin.Provider as TypeDefinition;
            Debug.Assert(type != null);

            static bool IsDeclaredWithinType(IMemberDefinition member, TypeDefinition type)
            {
                while ((member = member.DeclaringType) != null)
                {
                    if (member == type)
                        return true;
                }
                return false;
            }

            var reportOnMember = IsDeclaredWithinType(member, type);
            if (reportOnMember)
                origin = new MessageOrigin(member);

            bool isReflectionAccessCoveredByRUC = Annotations.ShouldSuppressAnalysisWarningsForRequiresUnreferencedCode(member, out RequiresUnreferencedCodeAttribute? requiresUnreferencedCodeAttribute);
            bool isCompilerGenerated = CompilerGeneratedState.IsNestedFunctionOrStateMachineMember(member);
            if (isReflectionAccessCoveredByRUC && !isCompilerGenerated)
            {
                var id = reportOnMember ? DiagnosticId.DynamicallyAccessedMembersOnTypeReferencesMemberWithRequiresUnreferencedCode : DiagnosticId.DynamicallyAccessedMembersOnTypeReferencesMemberOnBaseWithRequiresUnreferencedCode;
                Context.LogWarning(origin, id, type.GetDisplayName(),
                    ((MemberReference)member).GetDisplayName(), // The cast is valid since it has to be a method or field
                    MessageFormat.FormatRequiresAttributeMessageArg(requiresUnreferencedCodeAttribute!.Message),
                    MessageFormat.FormatRequiresAttributeMessageArg(requiresUnreferencedCodeAttribute!.Url));
            }

            bool isReflectionAccessCoveredByDAM = Annotations.FlowAnnotations.ShouldWarnWhenAccessedForReflection(member);
            if (isReflectionAccessCoveredByDAM && !isCompilerGenerated)
            {
                var id = reportOnMember ? DiagnosticId.DynamicallyAccessedMembersOnTypeReferencesMemberWithDynamicallyAccessedMembers : DiagnosticId.DynamicallyAccessedMembersOnTypeReferencesMemberOnBaseWithDynamicallyAccessedMembers;
                Context.LogWarning(origin, id, type.GetDisplayName(), ((MemberReference)member).GetDisplayName());
            }
        }

        void MarkField(FieldDefinition field, in DependencyInfo reason, in MessageOrigin origin)
        {
#if DEBUG
            if (!_fieldReasons.Contains(reason.Kind))
                throw new ArgumentOutOfRangeException($"Internal error: unsupported field dependency {reason.Kind}");
#endif

            if (reason.Kind == DependencyKind.AlreadyMarked)
            {
                Debug.Assert(Annotations.IsMarked(field));
            }
            else
            {
                Annotations.Mark(field, reason, origin);
            }

            ProcessAnalysisAnnotationsForField(field, reason.Kind, in origin);

            if (CheckProcessed(field))
                return;

            // Use the original origin for marking the declaring type - it provides better warning message location
            MarkType(field.DeclaringType, new DependencyInfo(DependencyKind.DeclaringType, field), origin);

            var fieldOrigin = new MessageOrigin(field);
            MarkType(field.FieldType, new DependencyInfo(DependencyKind.FieldType, field), fieldOrigin);
            MarkCustomAttributes(field, new DependencyInfo(DependencyKind.CustomAttribute, field), fieldOrigin);
            MarkMarshalSpec(field, new DependencyInfo(DependencyKind.FieldMarshalSpec, field), fieldOrigin);
            DoAdditionalFieldProcessing(field);

            // If we accessed a field on a type and the type has explicit/sequential layout, make sure to keep
            // all the other fields.
            //
            // We normally do this when the type is seen as instantiated, but one can get into a situation
            // where the type is not seen as instantiated and the offsets still matter (usually when type safety
            // is violated with Unsafe.As).
            //
            // This won't do too much work because classes are rarely tagged for explicit/sequential layout.
            if (!field.DeclaringType.IsValueType && !field.DeclaringType.IsAutoLayout)
            {
                // We also need to walk the base hierarchy because the offset of the field depends on the
                // layout of the base.
                TypeDefinition? typeWithFields = field.DeclaringType;
                while (typeWithFields != null)
                {
                    MarkImplicitlyUsedFields(typeWithFields, fieldOrigin);
                    typeWithFields = Context.TryResolve(typeWithFields.BaseType);
                }
            }

            var parent = field.DeclaringType;
            if (field.IsStatic && !Annotations.HasPreservedStaticCtor(parent))
            {
                var cctorReason = reason.Kind switch
                {
                    // Report an edge directly from the method accessing the field to the static ctor it triggers
                    DependencyKind.FieldAccess => new DependencyInfo(DependencyKind.TriggersCctorThroughFieldAccess, reason.Source),
                    _ => new DependencyInfo(DependencyKind.CctorForField, field)
                };
                MarkStaticConstructor(parent, cctorReason, fieldOrigin);
            }
        }

        void ProcessAnalysisAnnotationsForField(FieldDefinition field, DependencyKind dependencyKind, in MessageOrigin origin)
        {
            switch (dependencyKind)
            {
                // Marked through things like descriptor - don't want to warn as it's intentional choice
                case DependencyKind.AlreadyMarked:
                case DependencyKind.TypePreserve:
                case DependencyKind.PreservedMethod:
                case DependencyKind.MemberOfType:
                    return;

                case DependencyKind.DynamicallyAccessedMemberOnType:
                    ReportWarningsForTypeHierarchyReflectionAccess(field, origin);
                    return;

                default:
                    break;
            }

            if (Annotations.ShouldSuppressAnalysisWarningsForRequiresUnreferencedCode(origin.Provider, out _))
                return;

            if (Annotations.ShouldSuppressAnalysisWarningsForRequiresUnreferencedCode(field, out RequiresUnreferencedCodeAttribute? requiresUnreferencedCodeAttribute))
                ReportRequiresUnreferencedCode(field.GetDisplayName(), requiresUnreferencedCodeAttribute!, new DiagnosticContext(origin, diagnosticsEnabled: true, Context));

            switch (dependencyKind)
            {
                case DependencyKind.AccessedViaReflection:
                case DependencyKind.DynamicDependency:
                case DependencyKind.DynamicallyAccessedMember:
                case DependencyKind.InteropMethodDependency:
                case DependencyKind.Ldtoken:
                case DependencyKind.UnsafeAccessorTarget:
                    if (Annotations.FlowAnnotations.ShouldWarnWhenAccessedForReflection(field))
                        Context.LogWarning(origin, DiagnosticId.DynamicallyAccessedMembersFieldAccessedViaReflection, field.GetDisplayName());

                    break;
            }
        }

        /// <summary>
        /// Returns true if the assembly of the <paramref name="scope"></paramref> is not set to link (i.e. action=copy is set for that assembly)
        /// </summary>
        protected virtual bool IgnoreScope(IMetadataScope scope)
        {
            AssemblyDefinition? assembly = Context.Resolve(scope);
            return assembly != null && Annotations.GetAction(assembly) != AssemblyAction.Link;
        }

        void MarkModule(ModuleDefinition module, DependencyInfo reason, MessageOrigin origin)
        {
            if (reason.Kind == DependencyKind.AlreadyMarked)
            {
                Debug.Assert(Annotations.IsMarked(module));
            }
            else
            {
                Annotations.Mark(module, reason, origin);
            }
            if (CheckProcessed(module))
                return;
            MarkAssembly(module.Assembly, new DependencyInfo(DependencyKind.AssemblyOfModule, module), origin);
        }

        protected virtual void MarkSerializable(TypeDefinition type, MessageOrigin origin)
        {
            if (!type.HasMethods)
                return;

            if (Context.GetTargetRuntimeVersion() > TargetRuntimeVersion.NET5)
                return;

            if (type.IsSerializable())
            {
                MarkDefaultConstructor(type, new DependencyInfo(DependencyKind.SerializationMethodForType, type), origin);
                MarkMethodsIf(type.Methods, IsSpecialSerializationConstructor, new DependencyInfo(DependencyKind.SerializationMethodForType, type), origin);
            }

            MarkMethodsIf(type.Methods, HasOnSerializeOrDeserializeAttribute, new DependencyInfo(DependencyKind.SerializationMethodForType, type), origin);
        }

        protected internal virtual void MarkTypeVisibleToReflection(TypeReference type, in DependencyInfo reason, in MessageOrigin origin)
        {
            TypeDefinition? definition = MarkType(type, reason, origin);
            if (definition is not null)
            {
                // If a type is visible to reflection, we need to stop doing optimization that could cause observable difference
                // in reflection APIs. This includes APIs like MakeGenericType (where variant castability of the produced type
                // could be incorrect) or IsAssignableFrom (where assignability of unconstructed types might change).
                MarkRelevantToVariantCasting(definition);
                Annotations.MarkReflectionUsed(definition);
                MarkImplicitlyUsedFields(definition, origin);
            }
        }

        internal void MarkRelevantToVariantCasting(TypeDefinition type)
        {
            _typeMapHandler.ProcessType(type);
            Annotations.MarkRelevantToVariantCasting(type);
        }

        internal void MarkMethodVisibleToReflection(MethodReference method, in DependencyInfo reason, in MessageOrigin origin)
        {
            MarkMethod(method, reason, origin);
            if (Context.Resolve(method) is MethodDefinition methodDefinition)
            {
                Annotations.MarkReflectionUsed(methodDefinition);
                Annotations.MarkIndirectlyCalledMethod(methodDefinition);
            }
        }

        bool MarkMethodsVisibleToReflection(TypeDefinition type, in DependencyInfo reason, MessageOrigin origin)
        {
            if (!type.HasMethods)
                return false;

            foreach (MethodDefinition method in type.Methods)
            {
                if (IsFullyPreservedAction(Annotations.GetAction(type.Module.Assembly)))
                    Annotations.SetAction(method, MethodAction.ForceParse);
                MarkMethodVisibleToReflection(method, reason, origin);
            }
            return true;
        }

        internal void MarkFieldVisibleToReflection(FieldReference field, in DependencyInfo reason, in MessageOrigin origin)
        {
            MarkField(field, reason, origin);
        }

        bool MarkFieldsVisibleToReflection(TypeDefinition type, in DependencyInfo reason, MessageOrigin origin, bool markBackingFieldsOnlyIfPropertyMarked = false)
        {
            if (!type.HasFields)
                return false;

            foreach (FieldDefinition field in type.Fields)
            {
                if (markBackingFieldsOnlyIfPropertyMarked && field.Name.EndsWith(">k__BackingField", StringComparison.Ordinal))
                {
                    // We can't reliably construct the expected property name from the backing field name for all compilers
                    // because csc shortens the name of the backing field in some cases
                    // For example:
                    // Field Name = <IFoo<int>.Bar>k__BackingField
                    // Property Name = IFoo<System.Int32>.Bar
                    //
                    // instead we will search the properties and find the one that makes use of the current backing field
                    var propertyDefinition = SearchPropertiesForMatchingFieldDefinition(field);
                    if (propertyDefinition != null && !Annotations.IsMarked(propertyDefinition))
                        continue;
                }
                MarkFieldVisibleToReflection(field, reason, origin);
            }
            return true;
        }

        internal void MarkPropertyVisibleToReflection(PropertyDefinition property, in DependencyInfo reason, in MessageOrigin origin)
        {
            // Marking the property itself actually doesn't keep it (it only marks its attributes and records the dependency), we have to mark the methods on it
            MarkProperty(property, reason);
            // We don't track PropertyInfo, so we can't tell if any accessor is needed by the app, so include them both.
            // With better tracking it might be possible to be more precise here: dotnet/linker/issues/1948
            MarkMethodIfNotNull(property.GetMethod, reason, origin);
            MarkMethodIfNotNull(property.SetMethod, reason, origin);
            MarkMethodsIf(property.OtherMethods, m => true, reason, origin);
        }

        internal void MarkEventVisibleToReflection(EventDefinition @event, in DependencyInfo reason, in MessageOrigin origin)
        {
            MarkEvent(@event, reason, origin);
            // MarkEvent already marks the add/remove/invoke methods, but we need to mark them with the
            // DependencyInfo used to access the event from reflection, to produce warnings for annotated
            // event methods.
            MarkMethodIfNotNull(@event.AddMethod, reason, origin);
            MarkMethodIfNotNull(@event.RemoveMethod, reason, origin);
            MarkMethodIfNotNull(@event.InvokeMethod, reason, origin);
            MarkMethodsIf(@event.OtherMethods, m => true, reason, origin);
        }

        internal void MarkStaticConstructorVisibleToReflection(TypeDefinition type, in DependencyInfo reason, in MessageOrigin origin)
        {
            MarkStaticConstructor(type, reason, origin);
        }

        /// <summary>
        /// Marks the specified <paramref name="reference"/> as referenced.
        /// </summary>
        /// <param name="reference">The type reference to mark.</param>
        /// <param name="reason">The reason why the marking is occuring</param>
        /// <returns>The resolved type definition if the reference can be resolved</returns>
        protected internal virtual TypeDefinition? MarkType(TypeReference reference, DependencyInfo reason, MessageOrigin origin)
        {
#if DEBUG
            if (!_typeReasons.Contains(reason.Kind))
                throw new ArgumentOutOfRangeException($"Internal error: unsupported type dependency {reason.Kind}");
#endif
            if (reference == null)
                return null;

            (reference, reason) = GetOriginalType(reference, reason, origin);

            if (reference is FunctionPointerType)
                return null;

            if (reference is GenericParameter)
                return null;

            TypeDefinition? type = Context.Resolve(reference);

            if (type == null)
                return null;

            // Track a mark reason for each call to MarkType.
            switch (reason.Kind)
            {
                case DependencyKind.AlreadyMarked:
                    Debug.Assert(Annotations.IsMarked(type));
                    break;
                default:
                    Annotations.Mark(type, reason, origin);
                    break;
            }

            // Treat cctors triggered by a called method specially and mark this case up-front.
            if (type.HasMethods && ShouldMarkTypeStaticConstructor(type) && reason.Kind == DependencyKind.DeclaringTypeOfCalledMethod)
                MarkStaticConstructor(type, new DependencyInfo(DependencyKind.TriggersCctorForCalledMethod, reason.Source), origin);

            if (Annotations.HasLinkerAttribute<RemoveAttributeInstancesAttribute>(type))
            {
                // Don't warn about references from the removed attribute itself (for example the .ctor on the attribute
                // will call MarkType on the attribute type itself).
                // If for some reason we do keep the attribute type (could be because of previous reference which would cause IL2045
                // or because of a copy assembly with a reference and so on) then we should not spam the warnings due to the type itself.
                // Also don't warn when the type is marked due to an assembly being rooted.
                if (!(reason.Source is IMemberDefinition sourceMemberDefinition && sourceMemberDefinition.DeclaringType == type) &&
                    reason.Kind is not DependencyKind.TypeInAssembly)
                {
                    // Don't warn for type map attribute types. They're marked as "remove attributes" but we explicitly keep the ones needed.
                    if (!TypeMapHandler.IsTypeMapAttributeType(type))
                        Context.LogWarning(origin, DiagnosticId.AttributeIsReferencedButTrimmerRemoveAllInstances, type.GetDisplayName());
                }
            }

            if (CheckProcessed(type))
                return type;

            if (type.Scope is ModuleDefinition module)
                MarkModule(module, new DependencyInfo(DependencyKind.ScopeOfType, type), origin);

            var typeOrigin = new MessageOrigin(type);

            foreach (Action<TypeDefinition> handleMarkType in MarkContext.MarkTypeActions)
                handleMarkType(type);

            MarkType(type.BaseType, new DependencyInfo(DependencyKind.BaseType, type), typeOrigin);

            // The DynamicallyAccessedMembers hierarchy processing must be done after the base type was marked
            // (to avoid inconsistencies in the cache), but before anything else as work done below
            // might need the results of the processing here.
            DynamicallyAccessedMembersTypeHierarchy.ProcessMarkedTypeForDynamicallyAccessedMembersHierarchy(type);

            if (type.DeclaringType != null)
                MarkType(type.DeclaringType, new DependencyInfo(DependencyKind.DeclaringType, type), typeOrigin);
            MarkCustomAttributes(type, new DependencyInfo(DependencyKind.CustomAttribute, type), typeOrigin);
            MarkSecurityDeclarations(type, new DependencyInfo(DependencyKind.CustomAttribute, type), typeOrigin);

            if (type.IsMulticastDelegate())
            {
                MarkMulticastDelegate(type, typeOrigin);
            }

            if (type.IsClass && type.BaseType == null && type.Name == "Object" && ShouldMarkSystemObjectFinalize)
                MarkMethodIf(type.Methods, static m => m.Name == "Finalize", new DependencyInfo(DependencyKind.MethodForSpecialType, type), typeOrigin);

            MarkSerializable(type, typeOrigin);

            // This marks static fields of KeyWords/OpCodes/Tasks subclasses of an EventSource type.
            // The special handling of EventSource is still needed in .NET6 in library mode
            if ((!Context.DisableEventSourceSpecialHandling || Context.GetTargetRuntimeVersion() < TargetRuntimeVersion.NET6) && BCL.EventTracingForWindows.IsEventSourceImplementation(type, Context))
            {
                MarkEventSourceProviders(type, typeOrigin);
            }

            // This marks properties for [EventData] types as well as other attribute dependencies.
            MarkTypeSpecialCustomAttributes(type, typeOrigin);

            MarkGenericParameterProvider(type, typeOrigin);

            // There are a number of markings we can defer until later when we know it's possible a reference type could be instantiated
            // For example, if no instance of a type exist, then we don't need to mark the interfaces on that type -- Note this is not true for static interfaces
            // However, for some other types there is no benefit to deferring
            if (type.IsInterface)
            {
                // There's no benefit to deferring processing of an interface type until we know a type implementing that interface is marked
                MarkRequirementsForInstantiatedTypes(type);
            }
            else if (type.IsValueType)
            {
                // Note : Technically interfaces could be removed from value types in some of the same cases as reference types, however, it's harder to know when
                // a value type instance could exist.  You'd have to track initobj and maybe locals types.  Going to punt for now.
                MarkRequirementsForInstantiatedTypes(type);
            }
            else if (IsFullyPreserved(type))
            {
                // Here for a couple reasons:
                // * Edge case to cover a scenario where a type has preserve all, implements interfaces, but does not have any instance ctors.
                //    Normally TypePreserve.All would cause an instance ctor to be marked and that would in turn lead to MarkInterfaceImplementations being called
                //    Without an instance ctor, MarkInterfaceImplementations is not called and then TypePreserve.All isn't truly respected.
                // * If an assembly has the action Copy and had ResolveFromAssemblyStep ran for the assembly, then InitializeType will have led us here
                //    When the entire assembly is preserved, then all interfaces, base, etc will be preserved on the type, so we need to make sure
                //    all of these types are marked.  For example, if an interface implementation is of a type in another assembly that is linked,
                //    and there are no other usages of that interface type, then we need to make sure the interface type is still marked because
                //    this type is going to retain the interface implementation
                MarkRequirementsForInstantiatedTypes(type);
            }
            else if (AlwaysMarkTypeAsInstantiated(type))
            {
                MarkRequirementsForInstantiatedTypes(type);
            }

            // Save for later once we know which interfaces are marked and then determine which interface implementations and methods to keep
            if (type.HasInterfaces)
                _typesWithInterfaces.Add((type, typeOrigin));

            if (type.HasMethods)
            {
                // TODO: MarkMethodIfNeededByBaseMethod should include logic for IsMethodNeededByTypeDueToPreservedScope: https://github.com/dotnet/linker/issues/3090
                foreach (var method in type.Methods)
                {
                    MarkMethodIfNeededByBaseMethod(method, typeOrigin);
                    if (IsMethodNeededByTypeDueToPreservedScope(method))
                    {
                        // For methods that must be preserved, blame the declaring type.
                        MarkMethod(method, new DependencyInfo(DependencyKind.VirtualNeededDueToPreservedScope, type), typeOrigin);
                    }
                }
                if (ShouldMarkTypeStaticConstructor(type) && reason.Kind != DependencyKind.TriggersCctorForCalledMethod)
                {
                    MarkStaticConstructor(type, new DependencyInfo(DependencyKind.CctorForType, type), typeOrigin);
                }
            }

            DoAdditionalTypeProcessing(type);

            ApplyPreserveInfo(type);
            ApplyPreserveMethods(type, typeOrigin);

            return type;
        }

        /// <summary>
        /// Allow subclasses to disable marking of System.Object.Finalize()
        /// </summary>
        protected virtual bool ShouldMarkSystemObjectFinalize => true;

        // Allow subclassers to mark additional things in the main processing loop
        protected virtual void DoAdditionalProcessing()
        {
        }

        // Allow subclassers to mark additional things
        protected virtual void DoAdditionalTypeProcessing(TypeDefinition type)
        {
        }

        // Allow subclassers to mark additional things
        protected virtual void DoAdditionalFieldProcessing(FieldDefinition field)
        {
        }

        // Allow subclassers to mark additional things
        protected virtual void DoAdditionalPropertyProcessing(PropertyDefinition property)
        {
        }

        // Allow subclassers to mark additional things
        protected virtual void DoAdditionalEventProcessing(EventDefinition evt)
        {
        }

        // Allow subclassers to mark additional things
        protected virtual void DoAdditionalInstantiatedTypeProcessing(TypeDefinition type)
        {
        }

        TypeDefinition? GetDebuggerAttributeTargetType(CustomAttribute ca, AssemblyDefinition asm)
        {
            foreach (var property in ca.Properties)
            {
                if (property.Name == "Target")
                    return Context.TryResolve((TypeReference)property.Argument.Value);

                if (property.Name == "TargetTypeName")
                {
                    string targetTypeName = (string)property.Argument.Value;
                    return Context.TryResolve(asm, targetTypeName);
                }
            }

            return null;
        }

        void MarkTypeSpecialCustomAttributes(TypeDefinition type, MessageOrigin origin)
        {
            if (!type.HasCustomAttributes)
                return;

            foreach (CustomAttribute attribute in type.CustomAttributes)
            {
                var attrType = attribute.Constructor.DeclaringType;
                var resolvedAttributeType = Context.Resolve(attrType);
                if (resolvedAttributeType == null)
                {
                    continue;
                }

                if (Annotations.HasLinkerAttribute<RemoveAttributeInstancesAttribute>(resolvedAttributeType) && Annotations.GetAction(type.Module.Assembly) == AssemblyAction.Link)
                    continue;

                switch (attrType.Name)
                {
                    case "XmlSchemaProviderAttribute" when attrType.Namespace == "System.Xml.Serialization":
                        MarkXmlSchemaProvider(type, attribute, origin);
                        break;
                    case "DebuggerDisplayAttribute" when attrType.Namespace == "System.Diagnostics":
                        MarkTypeWithDebuggerDisplayAttribute(type, attribute, origin);
                        break;
                    case "DebuggerTypeProxyAttribute" when attrType.Namespace == "System.Diagnostics":
                        MarkTypeWithDebuggerTypeProxyAttribute(type, attribute, origin);
                        break;
                    // The special handling of EventSource is still needed in .NET6 in library mode
                    case "EventDataAttribute" when attrType.Namespace == "System.Diagnostics.Tracing" && (!Context.DisableEventSourceSpecialHandling || Context.GetTargetRuntimeVersion() < TargetRuntimeVersion.NET6):
                        if (MarkMethodsIf(type.Methods, MethodDefinitionExtensions.IsPublicInstancePropertyMethod, new DependencyInfo(DependencyKind.ReferencedBySpecialAttribute, type), origin))
                            Tracer.AddDirectDependency(attribute, new DependencyInfo(DependencyKind.CustomAttribute, type), marked: false);
                        break;
                }
            }
        }

        void MarkMethodSpecialCustomAttributes(MethodDefinition method, MessageOrigin origin)
        {
            if (!method.HasCustomAttributes)
                return;

            foreach (CustomAttribute attribute in method.CustomAttributes)
            {
                switch (attribute.Constructor.DeclaringType.FullName)
                {
                    case "System.Web.Services.Protocols.SoapHeaderAttribute":
                        MarkSoapHeader(method, attribute, origin);
                        break;
                }
            }
        }

        void MarkXmlSchemaProvider(TypeDefinition type, CustomAttribute attribute, MessageOrigin origin)
        {
            if (TryGetStringArgument(attribute, out string? name))
            {
                Tracer.AddDirectDependency(attribute, new DependencyInfo(DependencyKind.CustomAttribute, type), marked: false);
                MarkNamedMethod(type, name, new DependencyInfo(DependencyKind.ReferencedBySpecialAttribute, attribute), origin);
            }
        }

        [GeneratedRegex("{[^{}]+}")]
        private static partial Regex DebuggerDisplayAttributeValueRegex();

        [GeneratedRegex(@".+,\s*nq")]
        private static partial Regex ContainsNqSuffixRegex();

        void MarkTypeWithDebuggerDisplayAttribute(TypeDefinition type, CustomAttribute attribute, MessageOrigin origin)
        {
            if (Context.KeepMembersForDebugger)
            {
                // Members referenced by the DebuggerDisplayAttribute are kept even if the attribute may not be.
                // Record a logical dependency on the attribute so that we can blame it for the kept members below.
                Tracer.AddDirectDependency(attribute, new DependencyInfo(DependencyKind.CustomAttribute, type), marked: false);

                MarkTypeWithDebuggerDisplayAttributeValue(type, attribute, (string)attribute.ConstructorArguments[0].Value, origin);
                if (attribute.HasProperties)
                {
                    foreach (var property in attribute.Properties)
                    {
                        if (property.Name is "Name" or "Type")
                        {
                            MarkTypeWithDebuggerDisplayAttributeValue(type, attribute, (string)property.Argument.Value, origin);
                        }
                    }
                }
            }
        }

        void MarkTypeWithDebuggerDisplayAttributeValue(TypeDefinition type, CustomAttribute attribute, string? displayString, MessageOrigin origin)
        {
            if (string.IsNullOrEmpty(displayString))
                return;

            foreach (Match match in DebuggerDisplayAttributeValueRegex().Matches(displayString))
            {
                // Remove '{' and '}'
                string realMatch = match.Value.Substring(1, match.Value.Length - 2);

                // Remove ",nq" suffix if present
                // (it asks the expression evaluator to remove the quotes when displaying the final value)
                if (ContainsNqSuffixRegex().IsMatch(realMatch))
                {
                    realMatch = realMatch.Substring(0, realMatch.LastIndexOf(','));
                }

                if (realMatch.EndsWith("()"))
                {
                    string methodName = realMatch.Substring(0, realMatch.Length - 2);

                    // It's a call to a method on some member.  Handling this scenario robustly would be complicated and a decent bit of work.
                    //
                    // We could implement support for this at some point, but for now it's important to make sure at least we don't crash trying to find some
                    // method on the current type when it exists on some other type
                    if (methodName.Contains('.'))
                        continue;

                    MethodDefinition? method = GetMethodWithNoParameters(type, methodName);
                    if (method != null)
                    {
                        MarkMethodVisibleToReflection(method, new DependencyInfo(DependencyKind.ReferencedBySpecialAttribute, attribute), origin);
                        continue;
                    }
                }
                else
                {
                    FieldDefinition? field = GetField(type, realMatch);
                    if (field != null)
                    {
                        MarkFieldVisibleToReflection(field, new DependencyInfo(DependencyKind.ReferencedBySpecialAttribute, attribute), origin);
                        continue;
                    }

                    PropertyDefinition? property = GetProperty(type, realMatch);
                    if (property != null)
                    {
                        MarkPropertyVisibleToReflection(property, new DependencyInfo(DependencyKind.ReferencedBySpecialAttribute, attribute), origin);
                        continue;
                    }
                }

                while (true)
                {
                    // Currently if we don't understand the DebuggerDisplayAttribute we mark everything on the type
                    // This can be improved: dotnet/linker/issues/1873
                    MarkMethodsVisibleToReflection(type, new DependencyInfo(DependencyKind.KeptForSpecialAttribute, attribute), origin);
                    MarkFieldsVisibleToReflection(type, new DependencyInfo(DependencyKind.ReferencedBySpecialAttribute, attribute), origin);
                    if (Context.TryResolve(type.BaseType) is not TypeDefinition baseType)
                        break;
                    type = baseType;
                }
                return;
            }
        }

        void MarkTypeWithDebuggerTypeProxyAttribute(TypeDefinition type, CustomAttribute attribute, MessageOrigin origin)
        {
            if (Context.KeepMembersForDebugger)
            {
                object constructorArgument = attribute.ConstructorArguments[0].Value;
                TypeReference? proxyTypeReference = constructorArgument as TypeReference;
                if (proxyTypeReference == null)
                {
                    if (constructorArgument is string proxyTypeReferenceString)
                    {
                        proxyTypeReference = type.Module.GetType(proxyTypeReferenceString, runtimeName: true);
                    }
                }

                if (proxyTypeReference == null)
                {
                    return;
                }

                Tracer.AddDirectDependency(attribute, new DependencyInfo(DependencyKind.CustomAttribute, type), marked: false);
                MarkType(proxyTypeReference, new DependencyInfo(DependencyKind.ReferencedBySpecialAttribute, attribute), origin);

                if (Context.TryResolve(proxyTypeReference) is TypeDefinition proxyType)
                {
                    MarkMethodsVisibleToReflection(proxyType, new DependencyInfo(DependencyKind.ReferencedBySpecialAttribute, attribute), origin);
                    MarkFieldsVisibleToReflection(proxyType, new DependencyInfo(DependencyKind.ReferencedBySpecialAttribute, attribute), origin);
                }
            }
        }

        static bool TryGetStringArgument(CustomAttribute attribute, [NotNullWhen(true)] out string? argument)
        {
            argument = null;

            if (attribute.ConstructorArguments.Count < 1)
                return false;

            argument = attribute.ConstructorArguments[0].Value as string;

            return argument != null;
        }

        protected int MarkNamedMethod(TypeDefinition type, string method_name, in DependencyInfo reason, MessageOrigin origin)
        {
            if (!type.HasMethods)
                return 0;

            int count = 0;
            foreach (MethodDefinition method in type.Methods)
            {
                if (method.Name != method_name)
                    continue;

                MarkMethod(method, reason, origin);
                count++;
            }

            return count;
        }

        void MarkSoapHeader(MethodDefinition method, CustomAttribute attribute, MessageOrigin origin)
        {
            if (!TryGetStringArgument(attribute, out string? member_name))
                return;

            MarkNamedField(method.DeclaringType, member_name, new DependencyInfo(DependencyKind.ReferencedBySpecialAttribute, attribute), origin);
            MarkNamedProperty(method.DeclaringType, member_name, new DependencyInfo(DependencyKind.ReferencedBySpecialAttribute, attribute), origin);
        }

        bool MarkNamedField(TypeDefinition type, string field_name, in DependencyInfo reason, MessageOrigin origin)
        {
            if (!type.HasFields)
                return false;

            foreach (FieldDefinition field in type.Fields)
            {
                if (field.Name != field_name)
                    continue;

                MarkField(field, reason, origin);
                return true;
            }

            return false;
        }

        void MarkNamedProperty(TypeDefinition type, string property_name, in DependencyInfo reason, MessageOrigin origin)
        {
            if (!type.HasProperties)
                return;

            foreach (PropertyDefinition property in type.Properties)
            {
                if (property.Name != property_name)
                    continue;

                // This marks methods directly without reporting the property.
                MarkMethod(property.GetMethod, reason, origin);
                MarkMethod(property.SetMethod, reason, origin);
            }
        }

        void MarkInterfaceImplementations(TypeDefinition type)
        {
            var ifaces = Annotations.GetRecursiveInterfaces(type);
            if (ifaces is null)
                return;
            foreach (var (ifaceType, impls) in ifaces)
            {
                // Only mark interface implementations of interface types that have been marked.
                // This enables stripping of interfaces that are never used
                if (ShouldMarkInterfaceImplementationList(type, impls, ifaceType))
                    MarkInterfaceImplementationList(impls, new MessageOrigin(type));
            }
        }


        protected virtual bool ShouldMarkInterfaceImplementationList(TypeDefinition type, List<InterfaceImplementation> ifaces, TypeReference ifaceType)
        {
            if (ifaces.All(Annotations.IsMarked))
                return false;

            if (!Context.IsOptimizationEnabled(CodeOptimizations.UnusedInterfaces, type))
                return true;

            if (Context.Resolve(ifaceType) is not TypeDefinition resolvedInterfaceType)
                return false;

            if (Annotations.IsMarked(resolvedInterfaceType))
                return true;

            // It's hard to know if a com or windows runtime interface will be needed from managed code alone,
            // so as a precaution we will mark these interfaces once the type is instantiated
            if (Context.KeepComInterfaces && (resolvedInterfaceType.IsImport || resolvedInterfaceType.IsWindowsRuntime))
                return true;

            return IsFullyPreserved(type);
        }

        void MarkGenericParameterProvider(IGenericParameterProvider provider, MessageOrigin origin)
        {
            if (!provider.HasGenericParameters)
                return;

            foreach (GenericParameter parameter in provider.GenericParameters)
                MarkGenericParameter(parameter, origin);
        }

        void MarkGenericParameter(GenericParameter parameter, MessageOrigin origin)
        {
            MarkCustomAttributes(parameter, new DependencyInfo(DependencyKind.GenericParameterCustomAttribute, parameter.Owner), origin);
            if (!parameter.HasConstraints)
                return;

            foreach (var constraint in parameter.Constraints)
            {
                MarkCustomAttributes(constraint, new DependencyInfo(DependencyKind.GenericParameterConstraintCustomAttribute, parameter.Owner), origin);
                MarkType(constraint.ConstraintType, new DependencyInfo(DependencyKind.GenericParameterConstraintType, parameter.Owner), origin);
            }
        }

        /// <summary>
        /// Returns true if any of the base methods of the <paramref name="method"/> passed is in an assembly that is not trimmed (i.e. action != trim).
        /// Meant to be used to determine whether methods should be marked regardless of whether it is instantiated or not.
        /// </summary>
        /// <remarks>
        /// When the unusedinterfaces optimization is on, this is used to mark methods that override an abstract method from a non-link assembly and must be kept.
        /// When the unusedinterfaces optimization is off, this will do the same as when on but will also mark interface methods from interfaces defined in a non-link assembly.
        /// If the containing type is instantiated, the caller should also use <see cref="IsMethodNeededByInstantiatedTypeDueToPreservedScope (MethodDefinition)" />
        /// </remarks>
        bool IsMethodNeededByTypeDueToPreservedScope(MethodDefinition method)
        {
            if (Annotations.IsMarked(method))
                return false;
            // All methods we care about here will be virtual
            if (!method.IsVirtual)
                return false;

            var base_list = Annotations.GetBaseMethods(method);
            if (base_list == null)
                return false;

            foreach (OverrideInformation ov in base_list)
            {
                // Skip interface methods, they will be captured later by IsInterfaceImplementationMethodNeededByTypeDueToInterface
                if (ov.Base.DeclaringType.IsInterface)
                    continue;

                if (!IgnoreScope(ov.Base.DeclaringType.Scope) && !IsMethodNeededByTypeDueToPreservedScope(ov.Base))
                    continue;

                // If the type is marked, we need to keep overrides of abstract members defined in assemblies
                // that are copied to keep the IL valid.
                // However, if the base method is a non-abstract virtual (has an implementation on the base type), then we don't need to keep the override
                // until the type could be instantiated
                if (!ov.Base.IsAbstract)
                    continue;

                return true;
            }

            return false;
        }

        /// <summary>
        /// Returns true if the override method is required due to the interface that the base method is declared on. See doc at <see href="docs/methods-kept-by-interface.md"/> for explanation of logic.
        /// </summary>
        bool IsInterfaceImplementationMethodNeededByTypeDueToInterface(OverrideInformation overrideInformation)
        {
            var @base = overrideInformation.Base;
            var method = overrideInformation.Override;
            Debug.Assert(overrideInformation.IsOverrideOfInterfaceMember);
            if (@base is null || method is null || @base.DeclaringType is null)
                return false;

            if (Annotations.IsMarked(method))
                return false;

            // If the interface implementation is not marked, do not mark the implementation method
            // A type that doesn't implement the interface isn't required to have methods that implement the interface.
            InterfaceImplementation? iface = overrideInformation.InterfaceImplementor.InterfaceImplementation;
            if (!((iface is not null && Annotations.IsMarked(iface))
                || IsInterfaceImplementationMarkedRecursively(method.DeclaringType, @base.DeclaringType)))
                return false;

            // If the interface method is not marked and the interface doesn't come from a preserved scope, do not mark the implementation method
            // Unmarked interface methods from link assemblies will be removed so the implementing method does not need to be kept.
            if (!Annotations.IsMarked(@base) && !IgnoreScope(@base.DeclaringType.Scope))
                return false;

            // If the interface method is abstract, mark the implementation method
            // The method is needed for valid IL.
            if (@base.IsAbstract)
                return true;

            // If the method is static and the implementing type is relevant to variant casting, mark the implementation method.
            // A static method may only be called through a constrained call if the type is relevant to variant casting.
            if (@base.IsStatic)
                return Annotations.IsRelevantToVariantCasting(overrideInformation.InterfaceImplementor.Implementor)
                    || IgnoreScope(@base.DeclaringType.Scope);

            // If the implementing type is marked as instantiated, mark the implementation method.
            // If the type is not instantiated, do not mark the implementation method
            return Annotations.IsInstantiated(overrideInformation.InterfaceImplementor.Implementor);
        }

        static bool IsSpecialSerializationConstructor(MethodDefinition method)
        {
            if (!method.IsInstanceConstructor())
                return false;

            if (method.GetMetadataParametersCount() != 2)
                return false;

            return method.TryGetParameter((ParameterIndex)1)?.ParameterType.Name == "SerializationInfo" &&
                method.TryGetParameter((ParameterIndex)2)?.ParameterType.Name == "StreamingContext";
        }

        protected internal bool MarkMethodsIf(Collection<MethodDefinition> methods, Func<MethodDefinition, bool> predicate, in DependencyInfo reason, in MessageOrigin origin)
        {
            bool marked = false;
            foreach (MethodDefinition method in methods)
            {
                if (predicate(method))
                {
                    MarkMethod(method, reason, origin);
                    marked = true;
                }
            }
            return marked;
        }

        protected MethodDefinition? MarkMethodIf(Collection<MethodDefinition> methods, Func<MethodDefinition, bool> predicate, in DependencyInfo reason, in MessageOrigin origin)
        {
            foreach (MethodDefinition method in methods)
            {
                if (predicate(method))
                {
                    return MarkMethod(method, reason, origin);
                }
            }

            return null;
        }

        protected bool MarkDefaultConstructor(TypeDefinition type, in DependencyInfo reason, MessageOrigin origin)
        {
            if (type?.HasMethods != true)
                return false;

            return MarkMethodIf(type.Methods, MethodDefinitionExtensions.IsDefaultConstructor, reason, origin) != null;
        }

        void MarkCustomMarshalerGetInstance(TypeDefinition type, in DependencyInfo reason, MessageOrigin origin)
        {
            if (!type.HasMethods)
                return;

            MarkMethodIf(type.Methods,
                m =>
                    m.Name == "GetInstance"
                    && m.IsStatic
                    && m.GetMetadataParametersCount() == 1
                    && m.GetParameter((ParameterIndex)0).ParameterType.MetadataType == MetadataType.String,
                reason,
                origin);
        }

        void MarkICustomMarshalerMethods(TypeDefinition inputType, in DependencyInfo reason, MessageOrigin origin)
        {
            TypeDefinition? type = inputType;
            do
            {
                if (!type.HasInterfaces)
                    continue;

                foreach (var iface in type.Interfaces)
                {
                    var iface_type = iface.InterfaceType;
                    if (!iface_type.IsTypeOf("System.Runtime.InteropServices", "ICustomMarshaler"))
                        continue;

                    //
                    // Instead of trying to guess where to find the interface declaration ILLink walks
                    // the list of implemented interfaces and resolve the declaration from there
                    //
                    var tdef = Context.Resolve(iface_type);
                    if (tdef == null)
                    {
                        return;
                    }

                    MarkMethodsIf(tdef.Methods, m => !m.IsStatic, reason, origin);

                    MarkInterfaceImplementation(iface, new MessageOrigin(type));
                    return;
                }
            } while ((type = Context.TryResolve(type.BaseType)) != null);
        }

        bool IsNonEmptyStaticConstructor(MethodDefinition method)
        {
            if (!method.IsStaticConstructor())
                return false;

            if (!method.HasBody || !method.IsIL)
                return true;

            var body = Context.GetMethodIL(method);

            if (body.Body.CodeSize != 1)
                return true;

            return body.Instructions[0].OpCode.Code != Code.Ret;
        }

        static bool HasOnSerializeOrDeserializeAttribute(MethodDefinition method)
        {
            if (!method.HasCustomAttributes)
                return false;
            foreach (var ca in method.CustomAttributes)
            {
                var cat = ca.AttributeType;
                if (cat.Namespace != "System.Runtime.Serialization")
                    continue;
                switch (cat.Name)
                {
                    case "OnDeserializedAttribute":
                    case "OnDeserializingAttribute":
                    case "OnSerializedAttribute":
                    case "OnSerializingAttribute":
                        return true;
                }
            }
            return false;
        }

        protected virtual bool AlwaysMarkTypeAsInstantiated(TypeDefinition td)
        {
            switch (td.Name)
            {
                // These types are created from native code which means we are unable to track when they are instantiated
                // Since these are such foundational types, let's take the easy route and just always assume an instance of one of these
                // could exist
                case "Delegate":
                case "MulticastDelegate":
                case "ValueType":
                case "Enum":
                case "Array":
                case "RuntimeType": // works around https://github.com/dotnet/runtime/issues/110605
                    return td.Namespace == "System";
            }

            return false;
        }

        void MarkEventSourceProviders(TypeDefinition td, MessageOrigin origin)
        {
            Debug.Assert(Context.GetTargetRuntimeVersion() < TargetRuntimeVersion.NET6 || !Context.DisableEventSourceSpecialHandling);
            foreach (var nestedType in td.NestedTypes)
            {
                if (BCL.EventTracingForWindows.IsProviderName(nestedType.Name))
                    MarkStaticFields(nestedType, new DependencyInfo(DependencyKind.EventSourceProviderField, td), origin);
            }
        }

        protected virtual void MarkMulticastDelegate(TypeDefinition type, MessageOrigin origin)
        {
            MarkMethodsIf(type.Methods, m => m.Name == ".ctor" || m.Name == "Invoke", new DependencyInfo(DependencyKind.MethodForSpecialType, type), origin);
        }

        protected (TypeReference, DependencyInfo) GetOriginalType(TypeReference type, DependencyInfo reason, MessageOrigin origin)
        {
            while (type is TypeSpecification specification)
            {
                if (type is GenericInstanceType git)
                {
                    MarkGenericArguments(git, origin);
                    Debug.Assert(!(specification.ElementType is TypeSpecification));
                }

                if (type is IModifierType mod)
                    MarkModifierType(mod, origin);

                if (type is FunctionPointerType fnptr)
                {
                    MarkParameters(fnptr, origin);
                    MarkType(fnptr.ReturnType, new DependencyInfo(DependencyKind.ReturnType, fnptr), origin);
                    break; // FunctionPointerType is the original type
                }

                // Blame the type reference (which isn't marked) on the original reason.
                Tracer.AddDirectDependency(specification, reason, marked: false);
                // Blame the outgoing element type on the specification.
                (type, reason) = (specification.ElementType, new DependencyInfo(DependencyKind.ElementType, specification));
            }

            return (type, reason);
        }

        void MarkParameters(FunctionPointerType fnptr, MessageOrigin origin)
        {
            if (!fnptr.HasParameters)
                return;

            for (int i = 0; i < fnptr.Parameters.Count; i++)
            {
                MarkType(fnptr.Parameters[i].ParameterType, new DependencyInfo(DependencyKind.ParameterType, fnptr), origin);
            }
        }

        void MarkModifierType(IModifierType mod, MessageOrigin origin)
        {
            MarkType(mod.ModifierType, new DependencyInfo(DependencyKind.ModifierType, mod), origin);
        }

        void MarkGenericArguments(IGenericInstance instance, MessageOrigin origin)
        {
            var arguments = instance.GenericArguments;

            IGenericParameterProvider? generic_element = GetGenericProviderFromInstance(instance);
            Collection<GenericParameter>? parameters = generic_element?.GenericParameters;

            for (int i = 0; i < arguments.Count; i++)
            {
                var argument = arguments[i];
                var parameter = parameters?[i];

                var argumentTypeDef = MarkType(argument, new DependencyInfo(DependencyKind.GenericArgumentType, instance), origin);

                if (parameter is not null && Annotations.FlowAnnotations.RequiresGenericArgumentDataFlowAnalysis(parameter))
                {
                    // The only two implementations of IGenericInstance both derive from MemberReference
                    Debug.Assert(instance is MemberReference);

                    var instanceMemberOrigin = origin.Provider is not null ? origin : new MessageOrigin(((MemberReference)instance).Resolve());
                    var scanner = new GenericArgumentDataFlow(Context, this, instanceMemberOrigin);
                    scanner.ProcessGenericArgumentDataFlow(parameter, argument);
                }

                if (argumentTypeDef == null)
                    continue;

                MarkRelevantToVariantCasting(argumentTypeDef);

                if (parameter?.HasDefaultConstructorConstraint == true)
                    MarkDefaultConstructor(argumentTypeDef, new DependencyInfo(DependencyKind.DefaultCtorForNewConstrainedGenericArgument, instance), origin);
            }
        }

        IGenericParameterProvider? GetGenericProviderFromInstance(IGenericInstance instance)
        {
            if (instance is GenericInstanceMethod method)
                return Context.TryResolve(method.ElementMethod);

            if (instance is GenericInstanceType type)
                return Context.TryResolve(type.ElementType);

            return null;
        }

        void ApplyPreserveInfo(TypeDefinition type)
        {
            var typeOrigin = new MessageOrigin(type);

            if (Annotations.TryGetPreserve(type, out TypePreserve preserve))
            {
                if (!Annotations.SetAppliedPreserve(type, preserve))
                    return;

                var di = new DependencyInfo(DependencyKind.TypePreserve, type);

                switch (preserve)
                {
                    case TypePreserve.All:
                        MarkFieldsVisibleToReflection(type, di, typeOrigin);
                        MarkMethodsVisibleToReflection(type, in di, typeOrigin);
                        return;

                    case TypePreserve.Fields:
                        if (!MarkFieldsVisibleToReflection(type, di, typeOrigin, markBackingFieldsOnlyIfPropertyMarked: true))
                            Context.LogWarning(type, DiagnosticId.TypeHasNoFieldsToPreserve, type.GetDisplayName());
                        break;
                    case TypePreserve.Methods:
                        if (!MarkMethodsVisibleToReflection(type, in di, typeOrigin))
                            Context.LogWarning(type, DiagnosticId.TypeHasNoMethodsToPreserve, type.GetDisplayName());
                        break;
                }
            }

            if (Annotations.TryGetPreservedMembers(type, out TypePreserveMembers members))
            {
                var di = new DependencyInfo(DependencyKind.TypePreserve, type);

                if (type.HasMethods)
                {
                    foreach (var m in type.Methods)
                    {
                        if ((members & TypePreserveMembers.Visible) != 0 && IsMethodVisible(m))
                        {
                            MarkMethodVisibleToReflection(m, di, typeOrigin);
                            continue;
                        }

                        if ((members & TypePreserveMembers.Internal) != 0 && IsMethodInternal(m))
                        {
                            MarkMethodVisibleToReflection(m, di, typeOrigin);
                            continue;
                        }

                        if ((members & TypePreserveMembers.Library) != 0)
                        {
                            if (IsSpecialSerializationConstructor(m) || HasOnSerializeOrDeserializeAttribute(m))
                            {
                                MarkMethodVisibleToReflection(m, di, typeOrigin);
                                continue;
                            }
                        }
                    }
                }

                if (type.HasFields)
                {
                    foreach (var f in type.Fields)
                    {
                        if ((members & TypePreserveMembers.Visible) != 0 && IsFieldVisible(f))
                        {
                            MarkFieldVisibleToReflection(f, di, typeOrigin);
                            continue;
                        }

                        if ((members & TypePreserveMembers.Internal) != 0 && IsFieldInternal(f))
                        {
                            MarkFieldVisibleToReflection(f, di, typeOrigin);
                            continue;
                        }
                    }
                }
            }
        }

        static bool IsMethodVisible(MethodDefinition method)
        {
            return method.IsPublic || method.IsFamily || method.IsFamilyOrAssembly;
        }

        static bool IsMethodInternal(MethodDefinition method)
        {
            return method.IsAssembly || method.IsFamilyAndAssembly;
        }

        static bool IsFieldVisible(FieldDefinition field)
        {
            return field.IsPublic || field.IsFamily || field.IsFamilyOrAssembly;
        }

        static bool IsFieldInternal(FieldDefinition field)
        {
            return field.IsAssembly || field.IsFamilyAndAssembly;
        }

        void ApplyPreserveMethods(TypeDefinition type, MessageOrigin origin)
        {
            var list = Annotations.GetPreservedMethods(type);
            if (list == null)
                return;

            Annotations.ClearPreservedMethods(type);
            MarkMethodCollection(list, new DependencyInfo(DependencyKind.PreservedMethod, type), origin);
        }

        void ApplyPreserveMethods(MethodDefinition method, MessageOrigin origin)
        {
            var list = Annotations.GetPreservedMethods(method);
            if (list == null)
                return;

            Annotations.ClearPreservedMethods(method);
            MarkMethodCollection(list, new DependencyInfo(DependencyKind.PreservedMethod, method), origin);
        }

        protected bool MarkFields(TypeDefinition type, bool includeStatic, in DependencyInfo reason, MessageOrigin origin)
        {
            if (!type.HasFields)
                return false;

            foreach (FieldDefinition field in type.Fields)
            {
                if (!includeStatic && field.IsStatic)
                    continue;
                MarkField(field, reason, origin);
            }

            return true;
        }

        PropertyDefinition? SearchPropertiesForMatchingFieldDefinition(FieldDefinition field)
        {
            foreach (var property in field.DeclaringType.Properties)
            {
                var body = property.GetMethod?.Body;
                if (body == null)
                    continue;

                foreach (var ins in Context.GetMethodIL(body).Instructions)
                {
                    if (ins?.Operand == field)
                        return property;
                }
            }

            return null;
        }

        protected void MarkStaticFields(TypeDefinition type, in DependencyInfo reason, MessageOrigin origin)
        {
            if (!type.HasFields)
                return;

            foreach (FieldDefinition field in type.Fields)
            {
                if (field.IsStatic)
                    MarkField(field, reason, origin);
            }
        }

        protected virtual bool MarkMethods(TypeDefinition type, in DependencyInfo reason, MessageOrigin origin)
        {
            if (!type.HasMethods)
                return false;

            MarkMethodCollection(type.Methods, reason, origin);
            return true;
        }

        void MarkMethodCollection(IList<MethodDefinition> methods, in DependencyInfo reason, MessageOrigin origin)
        {
            foreach (MethodDefinition method in methods)
                MarkMethod(method, reason, origin);
        }

        protected virtual MethodDefinition? MarkMethod(MethodReference reference, DependencyInfo reason, in MessageOrigin origin)
        {
            DependencyInfo originalReason = reason;
            (reference, reason) = GetOriginalMethod(reference, reason, origin);

            if (reference.DeclaringType is ArrayType arrayType)
            {
                MarkType(reference.DeclaringType, new DependencyInfo(DependencyKind.DeclaringType, reference), origin);

                if (reference.Name == ".ctor" && Context.TryResolve(arrayType) is TypeDefinition typeDefinition)
                {
                    MarkRelevantToVariantCasting(typeDefinition);
                }
                return null;
            }

            if (reference.DeclaringType is GenericInstanceType)
            {
                // Blame the method reference on the original reason without marking it.
                Tracer.AddDirectDependency(reference, reason, marked: false);
                MarkType(reference.DeclaringType, new DependencyInfo(DependencyKind.DeclaringType, reference), origin);
                // Mark the resolved method definition as a dependency of the reference.
                reason = new DependencyInfo(DependencyKind.MethodOnGenericInstance, reference);
            }

            MethodDefinition? method = Context.Resolve(reference);
            if (method == null)
                return null;

            var methodAction = Annotations.GetAction(method);
            if (methodAction is MethodAction.ConvertToStub)
            {
                // CodeRewriterStep runs after sweeping, and may request the stubbed value for any preserved method
                // with the action ConvertToStub. Ensure we have precomputed any stub value that may be needed by
                // CodeRewriterStep. This ensures sweeping doesn't change the stub value (which can be determined by
                // FeatureGuardAttribute or FeatureSwitchDefinitionAttribute that might have been removed).
                Annotations.TryGetMethodStubValue(method, out _);
            }

            if (methodAction == MethodAction.Nothing)
                Annotations.SetAction(method, MethodAction.Parse);


            // Use the original reason as it's important to correctly generate warnings
            // the updated reason is only useful for better tracking of dependencies.
            ProcessAnalysisAnnotationsForMethod(method, originalReason.Kind, origin);

            // Record the reason for marking a method on each call.
            switch (reason.Kind)
            {
                case DependencyKind.AlreadyMarked:
                    Debug.Assert(Annotations.IsMarked(method));
                    break;
                default:
                    Annotations.Mark(method, reason, origin);
                    break;
            }

            bool markedForCall =
                reason.Kind == DependencyKind.DirectCall ||
                reason.Kind == DependencyKind.VirtualCall ||
                reason.Kind == DependencyKind.Newobj;
            if (markedForCall)
            {
                // Record declaring type of a called method up-front as a special case so that we may
                // track at least some method calls that trigger a cctor.
                // Temporarily switch to the original source for marking this method
                // this is for the same reason as for tracking, but this time so that we report potential
                // warnings from a better place.
                MarkType(method.DeclaringType, new DependencyInfo(DependencyKind.DeclaringTypeOfCalledMethod, method), new MessageOrigin(reason.Source as IMemberDefinition ?? method));
            }

            if (method.TryGetEvent(out EventDefinition? @event)
                && reason.Kind != DependencyKind.EventMethod)
            {
                var kind = PropagateDependencyKindToAccessors(reason.Kind, DependencyKind.EventOfEventMethod);
                MarkEvent(@event, new DependencyInfo(kind, originalReason.Source), origin);
            }

            // We will only enqueue a method to be processed if it hasn't been processed yet.
            if (!CheckProcessed(method))
                EnqueueMethod(method, reason);

            return method;
        }

        bool ShouldWarnForReflectionAccessToCompilerGeneratedCode(MethodDefinition method, bool isCoveredByAnnotations)
        {
            // No need to warn if it's already covered by the Requires attribute or explicit annotations on the method.
            if (isCoveredByAnnotations)
                return false;

            if (!CompilerGeneratedState.IsNestedFunctionOrStateMachineMember(method) || method.Body == null)
                return false;

            // Warn only if it has potential dataflow issues, as approximated by our check to see if it requires
            // the reflection scanner. Checking this will also mark direct dependencies of the method body, if it
            // hasn't been marked already. A cache ensures this only happens once for the method, whether or not
            // it is accessed via reflection.
            return CheckRequiresReflectionMethodBodyScanner(Context.GetMethodIL(method));
        }

        void ProcessAnalysisAnnotationsForMethod(MethodDefinition method, DependencyKind dependencyKind, in MessageOrigin origin)
        {
            switch (dependencyKind)
            {
                // DirectCall, VirtualCall and NewObj are handled by ReflectionMethodBodyScanner
                // This is necessary since the ReflectionMethodBodyScanner has intrinsic handling for some
                // of the annotated methods (for example Type.GetType)
                // and it knows when it's OK and when it needs a warning. In this place we don't know
                // and would have to warn every time.
                case DependencyKind.DirectCall:
                case DependencyKind.VirtualCall:
                case DependencyKind.Newobj:

                // Special case (like object.Equals or similar) - avoid checking anything
                case DependencyKind.MethodForSpecialType:

                // Marked through things like descriptor - don't want to warn as it's intentional choice
                case DependencyKind.AlreadyMarked:
                case DependencyKind.TypePreserve:
                case DependencyKind.PreservedMethod:

                // Marking the base method only because it's a base method should not produce a warning
                // we should produce warning only if there's some other reference. This is because all methods
                // in the hierarchy should have the RUC (if base as it), and so something must have
                // started it.
                // Similarly for overrides.
                case DependencyKind.BaseMethod:
                case DependencyKind.MethodImplOverride:
                case DependencyKind.Override:
                case DependencyKind.OverrideOnInstantiatedType:

                // These are used for virtual methods which are kept because the base method is in an assembly
                // which is "copy" (or "skip"). We don't want to report warnings for methods which were kept
                // only because of "copy" action (or similar), so ignore it here. If the method is referenced
                // directly somewhere else (either the derived or base) the warning would be reported.
                case DependencyKind.MethodForInstantiatedType:
                case DependencyKind.VirtualNeededDueToPreservedScope:

                // Used when marked because the member must be kept for the type to function (for example explicit layout,
                // or because the type is included as a whole for some other reasons). This alone should not act as a base
                // for raising a warning.
                // Note that "include whole type" due to dynamic access is handled specifically in MarkEntireType
                // and the DependencyKind in that case will be one of the dynamic acccess kinds and not MemberOfType
                // since in those cases the warnings are desirable (potential access through reflection).
                case DependencyKind.MemberOfType:
                case DependencyKind.EventMethod:

                // Used when marking a cctor because a type or field is kept. This should not warn because we already warn
                // on access to members of the type which could trigger the cctor.
                case DependencyKind.CctorForType:
                case DependencyKind.CctorForField:
                case DependencyKind.TriggersCctorThroughFieldAccess:
                case DependencyKind.TriggersCctorForCalledMethod:

                // We should not be generating code which would produce warnings
                case DependencyKind.UnreachableBodyRequirement:

                case DependencyKind.Custom:
                case DependencyKind.Unspecified:

                // Don't warn for methods kept due to non-understood DebuggerDisplayAttribute
                // until https://github.com/dotnet/linker/issues/1873 is fixed.
                case DependencyKind.KeptForSpecialAttribute:
                    break;

                case DependencyKind.DynamicallyAccessedMemberOnType:
                    // DynamicallyAccessedMembers on type gets special treatment so that the warning origin
                    // is the type or the annotated member.
                    ReportWarningsForTypeHierarchyReflectionAccess(method, origin);
                    break;

                default:
                    // All other cases have the potential of us missing a warning if we don't report it
                    // It is possible that in some cases we may report the same warning twice, but that's better than not reporting it.
                    ReportWarningsForReflectionAccess(origin, method, dependencyKind);
                    break;
            }
            ;
        }

        internal static void ReportRequiresUnreferencedCode(string displayName, RequiresUnreferencedCodeAttribute requiresUnreferencedCode, in DiagnosticContext diagnosticContext)
        {
            string arg1 = MessageFormat.FormatRequiresAttributeMessageArg(requiresUnreferencedCode.Message);
            string arg2 = MessageFormat.FormatRequiresAttributeUrlArg(requiresUnreferencedCode.Url);
            diagnosticContext.AddDiagnostic(DiagnosticId.RequiresUnreferencedCode, displayName, arg1, arg2);
        }

        protected (MethodReference, DependencyInfo) GetOriginalMethod(MethodReference method, DependencyInfo reason, MessageOrigin origin)
        {
            while (method is MethodSpecification specification)
            {
                // Blame the method reference (which isn't marked) on the original reason.
                Tracer.AddDirectDependency(specification, reason, marked: false);
                // Blame the outgoing element method on the specification.
                if (method is GenericInstanceMethod gim)
                    MarkGenericArguments(gim, origin);

                (method, reason) = (specification.ElementMethod, new DependencyInfo(DependencyKind.ElementMethod, specification));
                Debug.Assert(!(method is MethodSpecification));
            }

            return (method, reason);
        }

        protected virtual void ProcessMethod(MethodDefinition method, in DependencyInfo reason)
        {
#if DEBUG
            if (!_methodReasons.Contains(reason.Kind))
                throw new InternalErrorException($"Unsupported method dependency {reason.Kind}");
#endif
            var methodOrigin = new MessageOrigin(method);

            bool markedForCall =
                reason.Kind == DependencyKind.DirectCall ||
                reason.Kind == DependencyKind.VirtualCall ||
                reason.Kind == DependencyKind.Newobj;

            foreach (Action<MethodDefinition> handleMarkMethod in MarkContext.MarkMethodActions)
                handleMarkMethod(method);

            if (!markedForCall)
                MarkType(method.DeclaringType, new DependencyInfo(DependencyKind.DeclaringType, method), methodOrigin);
            MarkCustomAttributes(method, new DependencyInfo(DependencyKind.CustomAttribute, method), methodOrigin);
            MarkSecurityDeclarations(method, new DependencyInfo(DependencyKind.CustomAttribute, method), methodOrigin);

            MarkGenericParameterProvider(method, methodOrigin);

            if (method.IsInstanceConstructor())
            {
                MarkRequirementsForInstantiatedTypes(method.DeclaringType);
                Tracer.AddDirectDependency(method.DeclaringType, new DependencyInfo(DependencyKind.InstantiatedByCtor, method), marked: false);
            }
            else if (method.IsStaticConstructor() && Annotations.HasLinkerAttribute<RequiresUnreferencedCodeAttribute>(method))
                Context.LogWarning(methodOrigin, DiagnosticId.RequiresUnreferencedCodeOnStaticConstructor, method.GetDisplayName());
            else if (method == method.Module.EntryPoint && Annotations.HasLinkerAttribute<RequiresUnreferencedCodeAttribute>(method))
                Context.LogWarning(methodOrigin, DiagnosticId.RequiresUnreferencedCodeOnEntryPoint, method.GetDisplayName());

            if (method.IsConstructor)
            {
                if (!Annotations.ProcessSatelliteAssemblies && KnownMembers.IsSatelliteAssemblyMarker(method))
                    Annotations.ProcessSatelliteAssemblies = true;
            }
            else if (method.TryGetProperty(out PropertyDefinition? property))
                MarkProperty(property, new DependencyInfo(PropagateDependencyKindToAccessors(reason.Kind, DependencyKind.PropertyOfPropertyMethod), method));

            if (method.HasMetadataParameters())
            {
#pragma warning disable RS0030 // MethodReference.Parameters is banned. It's easiest to leave the code as is for now
                foreach (ParameterDefinition pd in method.Parameters)
                {
                    MarkType(pd.ParameterType, new DependencyInfo(DependencyKind.ParameterType, method), methodOrigin);
                    MarkCustomAttributes(pd, new DependencyInfo(DependencyKind.ParameterAttribute, method), methodOrigin);
                    MarkMarshalSpec(pd, new DependencyInfo(DependencyKind.ParameterMarshalSpec, method), methodOrigin);
                }
#pragma warning restore RS0030
            }

            if (method.HasOverrides)
            {
                var assembly = Context.Resolve(method.DeclaringType.Scope);
                // If this method is in a Copy, CopyUsed, or Save assembly, .overrides won't get swept and we need to keep all of them
                bool markAllOverrides = assembly != null && Annotations.GetAction(assembly) is AssemblyAction.Copy or AssemblyAction.CopyUsed or AssemblyAction.Save;
                foreach (MethodReference @base in method.Overrides)
                {
                    // Method implementing a static interface method will have an override to it - note instance methods usually don't unless they're explicit.
                    // Calling the implementation method directly has no impact on the interface, and as such it should not mark the interface or its method.
                    // Only if the interface method is referenced, then all the methods which implemented must be kept, but not the other way round.
                    if (!markAllOverrides &&
                        Context.Resolve(@base) is MethodDefinition baseDefinition
                        && baseDefinition.DeclaringType.IsInterface && baseDefinition.IsStatic && method.IsStatic)
                        continue;
                    // Instance methods can have overrides on public implementation methods in IL, but C# will usually only have them for private explicit interface implementations.
                    // It is valid IL for a public method to override an interface method and only be called directly. In this case it would be safe to skip marking the .override method.
                    // However, in most cases, the C# compiler will only generate .override for instance methods when it's a private explicit interface implementations which can only be called through the interface.
                    // We can just take a short cut and mark all the overrides on instance methods. We shouldn't miss out on size savings for code generated by Roslyn.
                    MarkMethod(@base, new DependencyInfo(DependencyKind.MethodImplOverride, method), methodOrigin);
                    MarkRuntimeInterfaceImplementation(method, @base);
                }
            }

            MarkMethodSpecialCustomAttributes(method, methodOrigin);

            if (method.IsVirtual)
                MarkMethodAsVirtual(method, methodOrigin);

            MarkNewCodeDependencies(method, methodOrigin);

            MarkBaseMethods(method, methodOrigin);

            if (Annotations.GetOverrides(method) is IEnumerable<OverrideInformation> overrides)
            {
                foreach (var @override in overrides.Where(ov => Annotations.IsMarked(ov.Base) || IgnoreScope(ov.Base.DeclaringType.Scope)))
                {
                    if (ShouldMarkOverrideForBase(@override))
                        MarkOverrideForBaseMethod(@override, methodOrigin);
                }
            }

            MarkType(method.ReturnType, new DependencyInfo(DependencyKind.ReturnType, method), methodOrigin);
            MarkCustomAttributes(method.MethodReturnType, new DependencyInfo(DependencyKind.ReturnTypeAttribute, method), methodOrigin);
            MarkMarshalSpec(method.MethodReturnType, new DependencyInfo(DependencyKind.ReturnTypeMarshalSpec, method), methodOrigin);

            if (method.IsPInvokeImpl || method.IsInternalCall)
            {
                ProcessInteropMethod(method, methodOrigin);
            }

            if (!method.HasBody || method.Body.CodeSize == 0)
            {
                ProcessUnsafeAccessorMethod(method);
            }

            if (ShouldParseMethodBody(method))
                MarkMethodBody(method.Body, methodOrigin);

            if (method.DeclaringType.IsMulticastDelegate())
            {
                string? methodPair = null;
                if (method.Name == "BeginInvoke")
                    methodPair = "EndInvoke";
                else if (method.Name == "EndInvoke")
                    methodPair = "BeginInvoke";

                if (methodPair != null)
                {
                    TypeDefinition declaringType = method.DeclaringType;
                    MarkMethodIf(declaringType.Methods, m => m.Name == methodPair, new DependencyInfo(DependencyKind.MethodForSpecialType, declaringType), methodOrigin);
                }
            }

            DoAdditionalMethodProcessing(method);

            ApplyPreserveMethods(method, methodOrigin);
        }

        // Allow subclassers to mark additional things when marking a method
        protected virtual void DoAdditionalMethodProcessing(MethodDefinition method)
        {
        }

        static DependencyKind PropagateDependencyKindToAccessors(DependencyKind parentDependencyKind, DependencyKind kind)
        {
            switch (parentDependencyKind)
            {
                // If the member is marked due to descriptor or similar, propagate the original reason to suppress some warnings correctly
                case DependencyKind.AlreadyMarked:
                case DependencyKind.TypePreserve:
                case DependencyKind.PreservedMethod:
                case DependencyKind.DynamicallyAccessedMemberOnType:
                    return parentDependencyKind;

                default:
                    return kind;
            }
        }

        void MarkImplicitlyUsedFields(TypeDefinition type, MessageOrigin origin)
        {
            if (type?.HasFields != true)
                return;

            // keep fields for types with explicit layout, for enums and for InlineArray types
            if (!type.IsAutoLayout || type.IsEnum || TypeIsInlineArrayType(type))
                MarkFields(type, includeStatic: type.IsEnum, reason: new DependencyInfo(DependencyKind.MemberOfType, type), origin);
        }

        static bool TypeIsInlineArrayType(TypeDefinition type)
        {
            if (!type.IsValueType)
                return false;

            foreach (var customAttribute in type.CustomAttributes)
                if (customAttribute.AttributeType.IsTypeOf("System.Runtime.CompilerServices", "InlineArrayAttribute"))
                    return true;

            return false;
        }

        protected virtual void MarkRequirementsForInstantiatedTypes(TypeDefinition type)
        {
            if (Annotations.IsInstantiated(type))
                return;

            Annotations.MarkInstantiated(type);

            _typeMapHandler.ProcessType(type);

            var typeOrigin = new MessageOrigin(type);

            MarkInterfaceImplementations(type);

            // Requires interface implementations to be marked first
            foreach (var method in type.Methods)
            {
                MarkMethodIfNeededByBaseMethod(method, typeOrigin);
            }

            MarkImplicitlyUsedFields(type, typeOrigin);

            _typeMapHandler.ProcessInstantiated(type);

            DoAdditionalInstantiatedTypeProcessing(type);
        }

        void MarkRuntimeInterfaceImplementation(MethodDefinition method, MethodReference ov)
        {
            if (Context.Resolve(ov) is not MethodDefinition resolvedOverride)
                return;
            if (!resolvedOverride.DeclaringType.IsInterface)
                return;
            var interfaceToBeImplemented = ov.DeclaringType;

            var ifaces = Annotations.GetRecursiveInterfaces(method.DeclaringType);
            if (ifaces is null)
                return;
            foreach (var iface in ifaces)
            {
                if (TypeReferenceEqualityComparer.AreEqual(iface.InterfaceType, interfaceToBeImplemented, Context))
                {
                    MarkInterfaceImplementationList(iface.ImplementationChain, new MessageOrigin(method.DeclaringType));
                    return;
                }
            }
        }

        void MarkNewCodeDependencies(MethodDefinition method, MessageOrigin origin)
        {
            switch (Annotations.GetAction(method))
            {
                case MethodAction.ConvertToStub:
                    if (!method.IsInstanceConstructor())
                        return;

                    var baseType = Context.Resolve(method.DeclaringType.BaseType);
                    if (baseType == null)
                        break;
                    if (!MarkDefaultConstructor(baseType, new DependencyInfo(DependencyKind.BaseDefaultCtorForStubbedMethod, method), origin))
                        throw new LinkerFatalErrorException(MessageContainer.CreateErrorMessage(origin, DiagnosticId.CannotStubConstructorWhenBaseTypeDoesNotHaveConstructor, method.DeclaringType.GetDisplayName()));

                    break;

                case MethodAction.ConvertToThrow:
                    MarkAndCacheConvertToThrowExceptionCtor(new DependencyInfo(DependencyKind.UnreachableBodyRequirement, method), origin);
                    break;
            }
        }

        protected virtual void MarkAndCacheConvertToThrowExceptionCtor(DependencyInfo reason, MessageOrigin origin)
        {
            if (Context.MarkedKnownMembers.NotSupportedExceptionCtorString != null)
                return;

            var nse = BCL.FindPredefinedType(WellKnownType.System_NotSupportedException, Context);
            if (nse == null)
                throw new LinkerFatalErrorException(MessageContainer.CreateErrorMessage(null, DiagnosticId.CouldNotFindType, "System.NotSupportedException"));

            MarkType(nse, reason, origin);

            var nseCtor = MarkMethodIf(nse.Methods, KnownMembers.IsNotSupportedExceptionCtorString, reason, origin);
            Context.MarkedKnownMembers.NotSupportedExceptionCtorString = nseCtor ??
                throw new LinkerFatalErrorException(MessageContainer.CreateErrorMessage(null, DiagnosticId.CouldNotFindConstructor, nse.GetDisplayName()));

            var objectType = BCL.FindPredefinedType(WellKnownType.System_Object, Context);
            if (objectType == null)
                throw new NotSupportedException("Missing predefined 'System.Object' type");

            MarkType(objectType, reason, origin);

            var objectCtor = MarkMethodIf(objectType.Methods, MethodDefinitionExtensions.IsDefaultConstructor, reason, origin);
            Context.MarkedKnownMembers.ObjectCtor = objectCtor ??
                    throw new LinkerFatalErrorException(MessageContainer.CreateErrorMessage(null, DiagnosticId.CouldNotFindConstructor, objectType.GetDisplayName()));
        }

        bool MarkDisablePrivateReflectionAttribute()
        {
            if (Context.MarkedKnownMembers.DisablePrivateReflectionAttributeCtor != null)
                return false;

            var disablePrivateReflection = BCL.FindPredefinedType(WellKnownType.System_Runtime_CompilerServices_DisablePrivateReflectionAttribute, Context);
            if (disablePrivateReflection == null)
                throw new LinkerFatalErrorException(MessageContainer.CreateErrorMessage(null, DiagnosticId.CouldNotFindType, "System.Runtime.CompilerServices.DisablePrivateReflectionAttribute"));

            var emptyOrigin = new MessageOrigin(null as ICustomAttributeProvider);
            MarkType(disablePrivateReflection, DependencyInfo.DisablePrivateReflectionRequirement, emptyOrigin);

            var ctor = MarkMethodIf(disablePrivateReflection.Methods, MethodDefinitionExtensions.IsDefaultConstructor, new DependencyInfo(DependencyKind.DisablePrivateReflectionRequirement, disablePrivateReflection), emptyOrigin);
            Context.MarkedKnownMembers.DisablePrivateReflectionAttributeCtor = ctor ??
                throw new LinkerFatalErrorException(MessageContainer.CreateErrorMessage(null, DiagnosticId.CouldNotFindConstructor, disablePrivateReflection.GetDisplayName()));

            return true;
        }

        void MarkBaseMethods(MethodDefinition method, MessageOrigin origin)
        {
            var base_methods = Annotations.GetBaseMethods(method);
            if (base_methods == null)
                return;

            foreach (OverrideInformation ov in base_methods)
            {
                // We should add all interface base methods to _virtual_methods for virtual override annotation validation
                // Interfaces from preserved scope will be missed if we don't add them here
                // This will produce warnings for all interface methods and virtual methods regardless of whether the interface, interface implementation, or interface method is kept or not.
                if (ov.Base.DeclaringType.IsInterface && !method.DeclaringType.IsInterface)
                {
                    // These are all virtual, no need to check IsVirtual before adding to list
                    MarkMethodAsVirtual(ov.Base, origin);
                    continue;
                }

                MarkMethod(ov.Base, new DependencyInfo(DependencyKind.BaseMethod, method), origin);
                MarkBaseMethods(ov.Base, origin);
            }
        }

        void ProcessInteropMethod(MethodDefinition method, MessageOrigin origin)
        {
            if (method.IsPInvokeImpl && method.PInvokeInfo != null)
            {
                var pii = method.PInvokeInfo;
                Annotations.MarkProcessed(pii.Module, new DependencyInfo(DependencyKind.InteropMethodDependency, method));
                if (!string.IsNullOrEmpty(Context.PInvokesListFile))
                {
                    Context.PInvokes.Add(new PInvokeInfo(
                        assemblyName: method.DeclaringType.Module.Name,
                        entryPoint: pii.EntryPoint,
                        fullName: method.FullName,
                        moduleName: pii.Module.Name
                    ));
                }
            }

            TypeDefinition? returnTypeDefinition = Context.TryResolve(method.ReturnType);

            const bool includeStaticFields = false;
            if (returnTypeDefinition != null)
            {
                if (!returnTypeDefinition.IsImport)
                {
                    // What we keep here is correct most of the time, but not every time. Fine for now.
                    MarkDefaultConstructor(returnTypeDefinition, new DependencyInfo(DependencyKind.InteropMethodDependency, method), origin);
                    MarkFields(returnTypeDefinition, includeStaticFields, new DependencyInfo(DependencyKind.InteropMethodDependency, method), origin);
                }
            }

            if (method.HasThis && !method.DeclaringType.IsImport)
            {
                // This is probably Mono-specific. One can't have InternalCall or P/invoke instance methods in CoreCLR or .NET.
                MarkFields(method.DeclaringType, includeStaticFields, new DependencyInfo(DependencyKind.InteropMethodDependency, method), origin);
            }

#pragma warning disable RS0030 // MethodReference.Parameters is banned. It's easiest to leave this code as is for now
            foreach (ParameterDefinition pd in method.Parameters)
            {
                TypeReference paramTypeReference = pd.ParameterType;
                if (paramTypeReference is TypeSpecification paramTypeSpecification)
                {
                    paramTypeReference = paramTypeSpecification.ElementType;
                }
                TypeDefinition? paramTypeDefinition = Context.TryResolve(paramTypeReference);
                if (paramTypeDefinition != null)
                {
                    if (!paramTypeDefinition.IsImport)
                    {
                        // What we keep here is correct most of the time, but not every time. Fine for now.
                        MarkFields(paramTypeDefinition, includeStaticFields, new DependencyInfo(DependencyKind.InteropMethodDependency, method), origin);
                        if (pd.ParameterType.IsByReference)
                        {
                            MarkDefaultConstructor(paramTypeDefinition, new DependencyInfo(DependencyKind.InteropMethodDependency, method), origin);
                        }
                    }
                }
            }
#pragma warning restore RS0030
        }

        void ProcessUnsafeAccessorMethod(MethodDefinition method)
        {
            (new UnsafeAccessorMarker(Context, this)).ProcessUnsafeAccessorMethod(method);
        }

        protected virtual bool ShouldParseMethodBody(MethodDefinition method)
        {
            if (!method.HasBody)
                return false;

            switch (Annotations.GetAction(method))
            {
                case MethodAction.ForceParse:
                    return true;
                case MethodAction.Parse:
                    AssemblyDefinition? assembly = Context.Resolve(method.DeclaringType.Scope);
                    if (assembly == null)
                        return false;
                    switch (Annotations.GetAction(assembly))
                    {
                        case AssemblyAction.Link:
                        case AssemblyAction.Copy:
                        case AssemblyAction.CopyUsed:
                        case AssemblyAction.AddBypassNGen:
                        case AssemblyAction.AddBypassNGenUsed:
                            return true;
                        default:
                            return false;
                    }
                default:
                    return false;
            }
        }

        protected internal void MarkProperty(PropertyDefinition prop, in DependencyInfo reason)
        {
            if (!Annotations.MarkProcessed(prop, reason))
                return;

            var propertyOrigin = new MessageOrigin(prop);

            // Consider making this more similar to MarkEvent method?
            MarkCustomAttributes(prop, new DependencyInfo(DependencyKind.CustomAttribute, prop), propertyOrigin);
            DoAdditionalPropertyProcessing(prop);
        }

        protected internal virtual void MarkEvent(EventDefinition evt, in DependencyInfo reason, MessageOrigin origin)
        {
            origin = reason.Source is IMemberDefinition member ? new MessageOrigin(member) : origin;
            DependencyKind dependencyKind = DependencyKind.EventMethod;

            MarkMethodIfNotNull(evt.AddMethod, new DependencyInfo(dependencyKind, evt), origin);
            MarkMethodIfNotNull(evt.InvokeMethod, new DependencyInfo(dependencyKind, evt), origin);
            MarkMethodIfNotNull(evt.RemoveMethod, new DependencyInfo(dependencyKind, evt), origin);

            if (!Annotations.MarkProcessed(evt, reason))
                return;

            var eventOrigin = new MessageOrigin(evt);
            MarkCustomAttributes(evt, new DependencyInfo(DependencyKind.CustomAttribute, evt), eventOrigin);
            DoAdditionalEventProcessing(evt);
        }

        internal void MarkMethodIfNotNull(MethodReference method, in DependencyInfo reason, in MessageOrigin origin)
        {
            if (method == null)
                return;

            MarkMethod(method, reason, origin);
        }

        protected virtual void MarkMethodBody(MethodBody body, MessageOrigin origin)
        {
            var processedMethodBody = Context.GetMethodIL(body);

            if (Context.IsOptimizationEnabled(CodeOptimizations.UnreachableBodies, body.Method) && IsUnreachableBody(processedMethodBody))
            {
                MarkAndCacheConvertToThrowExceptionCtor(new DependencyInfo(DependencyKind.UnreachableBodyRequirement, body.Method), origin);
                _unreachableBodies.Add((body, origin));
                return;
            }

            // Note: we mark the method body of every method here including compiler-generated methods,
            // whether they are accessed from the user method or via reflection.
            // But for compiler-generated methods we only do dataflow analysis if they're used through their
            // corresponding user method, so we will skip dataflow for compiler-generated methods which
            // are only accessed via reflection.
            bool requiresReflectionMethodBodyScanner = MarkAndCheckRequiresReflectionMethodBodyScanner(processedMethodBody, origin);

            // Data-flow (reflection scanning) for compiler-generated methods will happen as part of the
            // data-flow scan of the user-defined method which uses this compiler-generated method.
            if (CompilerGeneratedState.IsNestedFunctionOrStateMachineMember(body.Method))
                return;

            MarkReflectionLikeDependencies(processedMethodBody, requiresReflectionMethodBodyScanner, origin);
        }

        bool CheckRequiresReflectionMethodBodyScanner(MethodIL methodIL)
        {
            // This method is only called on reflection access to compiler-generated methods.
            // This should be uncommon, so don't cache the result.
            if (ReflectionMethodBodyScanner.RequiresReflectionMethodBodyScannerForMethodBody(Context, methodIL.Method))
                return true;

            foreach (Instruction instruction in methodIL.Instructions)
            {
                switch (instruction.OpCode.OperandType)
                {
                    case OperandType.InlineField:
                        if (InstructionRequiresReflectionMethodBodyScannerForFieldAccess(instruction))
                            return true;
                        break;

                    case OperandType.InlineMethod:
                        if (ReflectionMethodBodyScanner.RequiresReflectionMethodBodyScannerForCallSite(Context, (MethodReference)instruction.Operand))
                            return true;
                        break;
                }
            }
            return false;
        }

        // Keep the return value of this method in sync with that of CheckRequiresReflectionMethodBodyScanner.
        // It computes the same value, while also marking as it goes, as an optimization.
        // This should only be called behind a check to IsProcessed for the method or corresponding user method,
        // to avoid recursion.
        bool MarkAndCheckRequiresReflectionMethodBodyScanner(MethodIL methodIL, MessageOrigin origin)
        {
#if DEBUG
            if (!Annotations.IsProcessed(methodIL.Method))
            {
                Debug.Assert(CompilerGeneratedState.IsNestedFunctionOrStateMachineMember(methodIL.Method));
                MethodDefinition owningMethod = methodIL.Method;
                while (Context.CompilerGeneratedState.TryGetOwningMethodForCompilerGeneratedMember(owningMethod, out var owner))
                    owningMethod = owner;
                Debug.Assert(owningMethod != methodIL.Method);
                Debug.Assert(Annotations.IsProcessed(owningMethod));
            }
#endif
            // This may get called multiple times for compiler-generated code: once for
            // reflection access, and once as part of the interprocedural scan of the user method.
            // This check ensures that we only do the work and produce warnings once.
            if (_compilerGeneratedMethodRequiresScanner.TryGetValue(methodIL.Body, out bool requiresReflectionMethodBodyScanner))
                return requiresReflectionMethodBodyScanner;

            foreach (VariableDefinition var in methodIL.Variables)
                MarkType(var.VariableType, new DependencyInfo(DependencyKind.VariableType, methodIL.Method), origin);

            foreach (ExceptionHandler eh in methodIL.ExceptionHandlers)
                if (eh.HandlerType == ExceptionHandlerType.Catch)
                    MarkType(eh.CatchType, new DependencyInfo(DependencyKind.CatchType, methodIL.Method), origin);

            requiresReflectionMethodBodyScanner =
                ReflectionMethodBodyScanner.RequiresReflectionMethodBodyScannerForMethodBody(Context, methodIL.Method);
            MessageOrigin ilOrigin = new MessageOrigin(methodIL.Method);
            foreach (Instruction instruction in methodIL.Instructions)
                MarkInstruction(instruction, methodIL.Method, ref requiresReflectionMethodBodyScanner, ref ilOrigin);

            MarkInterfacesNeededByBodyStack(methodIL);

            if (CompilerGeneratedState.IsNestedFunctionOrStateMachineMember(methodIL.Method))
                _compilerGeneratedMethodRequiresScanner.Add(methodIL.Body, requiresReflectionMethodBodyScanner);

            PostMarkMethodBody(methodIL.Body);

            Debug.Assert(requiresReflectionMethodBodyScanner == CheckRequiresReflectionMethodBodyScanner(methodIL));
            return requiresReflectionMethodBodyScanner;
        }

        bool IsUnreachableBody(MethodIL methodIL)
        {
            return !methodIL.Method.IsStatic
                && !Annotations.IsInstantiated(methodIL.Method.DeclaringType)
                && MethodBodyScanner.IsWorthConvertingToThrow(methodIL);
        }


        partial void PostMarkMethodBody(MethodBody body);

        void MarkInterfacesNeededByBodyStack(MethodIL methodIL)
        {
            // If a type could be on the stack in the body and an interface it implements could be on the stack on the body
            // then we need to mark that interface implementation.  When this occurs it is not safe to remove the interface implementation from the type
            // even if the type is never instantiated
            var implementations = new InterfacesOnStackScanner(Context).GetReferencedInterfaces(methodIL);
            if (implementations == null)
                return;

            foreach (var (implementation, type) in implementations)
                MarkInterfaceImplementation(implementation, new MessageOrigin(type));
        }

        bool InstructionRequiresReflectionMethodBodyScannerForFieldAccess(Instruction instruction)
            => instruction.OpCode.Code switch
            {
                // Field stores (Storing value to annotated field must be checked)
                Code.Stfld or
                Code.Stsfld or
                // Field address loads (as those can be used to store values to annotated field and thus must be checked)
                Code.Ldflda or
                Code.Ldsflda
                    => ReflectionMethodBodyScanner.RequiresReflectionMethodBodyScannerForAccess(Context, (FieldReference)instruction.Operand),
                // For ref fields, ldfld loads an address which can be used to store values to annotated fields
                Code.Ldfld or Code.Ldsfld when ((FieldReference)instruction.Operand).FieldType.IsByRefOrPointer()
                    => ReflectionMethodBodyScanner.RequiresReflectionMethodBodyScannerForAccess(Context, (FieldReference)instruction.Operand),
                // Other field operations are not interesting as they don't need to be checked
                _ => false
            };

        protected virtual void MarkInstruction(Instruction instruction, MethodDefinition method, ref bool requiresReflectionMethodBodyScanner, ref MessageOrigin origin)
        {
            switch (instruction.OpCode.OperandType)
            {
                case OperandType.InlineField:
                    requiresReflectionMethodBodyScanner |= InstructionRequiresReflectionMethodBodyScannerForFieldAccess(instruction);

                    origin = new MessageOrigin(origin, instruction.Offset);
                    MarkField((FieldReference)instruction.Operand, new DependencyInfo(DependencyKind.FieldAccess, method), origin);
                    break;

                case OperandType.InlineMethod:
                {
                    (DependencyKind dependencyKind, bool markForReflectionAccess) = instruction.OpCode.Code switch
                    {
                        Code.Jmp => (DependencyKind.DirectCall, false),
                        Code.Call => (DependencyKind.DirectCall, false),
                        Code.Callvirt => (DependencyKind.VirtualCall, false),
                        Code.Newobj => (DependencyKind.Newobj, false),
                        Code.Ldvirtftn => (DependencyKind.Ldvirtftn, true),
                        Code.Ldftn => (DependencyKind.Ldftn, true),
                        _ => throw new InvalidOperationException($"unexpected opcode {instruction.OpCode}")
                    };

                    MethodReference methodReference = (MethodReference)instruction.Operand;

                    requiresReflectionMethodBodyScanner |=
                        ReflectionMethodBodyScanner.RequiresReflectionMethodBodyScannerForCallSite(Context, methodReference);

                    origin = new MessageOrigin(origin, instruction.Offset);
                    if (markForReflectionAccess)
                    {
                        MarkMethodVisibleToReflection(methodReference, new DependencyInfo(dependencyKind, method), origin);
                    }
                    else
                    {
                        MarkMethod(methodReference, new DependencyInfo(dependencyKind, method), origin);
                    }
                    break;
                }

                case OperandType.InlineTok:
                {
                    object token = instruction.Operand;
                    Debug.Assert(instruction.OpCode.Code == Code.Ldtoken);
                    var reason = new DependencyInfo(DependencyKind.Ldtoken, method);
                    origin = new MessageOrigin(origin, instruction.Offset);

                    if (token is TypeReference typeReference)
                    {
                        MarkTypeVisibleToReflection(typeReference, reason, origin);
                    }
                    else if (token is MethodReference methodReference)
                    {
                        MarkMethodVisibleToReflection(methodReference, reason, origin);
                    }
                    else
                    {
                        MarkFieldVisibleToReflection((FieldReference)token, reason, origin);
                    }
                    break;
                }

                case OperandType.InlineType:
                    var operand = (TypeReference)instruction.Operand;
                    switch (instruction.OpCode.Code)
                    {
                        case Code.Newarr:
                            if (Context.TryResolve(operand) is TypeDefinition typeDefinition)
                            {
                                MarkRelevantToVariantCasting(typeDefinition);
                            }
                            break;
                        case Code.Isinst:
                            if (operand is TypeSpecification || operand is GenericParameter)
                                break;

                            if (!Context.CanApplyOptimization(CodeOptimizations.UnusedTypeChecks, method.DeclaringType.Module.Assembly))
                                break;

                            TypeDefinition? type = Context.Resolve(operand);
                            if (type == null)
                                return;

                            if (type.IsInterface)
                                break;

                            _typeMapHandler.ProcessType(type);

                            if (!Annotations.IsInstantiated(type))
                            {
                                _pending_isinst_instr.Add((type, method.Body, instruction));
                                return;
                            }

                            break;
                    }

                    origin = new MessageOrigin(origin, instruction.Offset);
                    MarkType(operand, new DependencyInfo(DependencyKind.InstructionTypeRef, method), origin);
                    break;
            }
        }

        void MarkInterfaceImplementationList(List<InterfaceImplementation> ifaces, MessageOrigin origin, DependencyInfo? reason = null)
        {
            foreach (var iface in ifaces)
            {
                MarkInterfaceImplementation(iface, origin, reason);
            }
        }

        protected internal virtual void MarkInterfaceImplementation(InterfaceImplementation iface, MessageOrigin origin, DependencyInfo? reason = null)
        {
            if (Annotations.IsMarked(iface))
                return;
            Annotations.MarkProcessed(iface, reason ?? new DependencyInfo(DependencyKind.InterfaceImplementationOnType, origin.Provider));

            // Blame the type that has the interfaceimpl, expecting the type itself to get marked for other reasons.
            MarkCustomAttributes(iface, new DependencyInfo(DependencyKind.CustomAttribute, iface), origin);
            // Blame the interface type on the interfaceimpl itself.
            MarkType(iface.InterfaceType, reason ?? new DependencyInfo(DependencyKind.InterfaceImplementationInterfaceType, iface), origin);
        }

        //
        // Extension point for reflection logic handling customization
        //
        protected internal virtual bool ProcessReflectionDependency(MethodBody body, Instruction instruction)
        {
            return false;
        }

        //
        // Tries to mark additional dependencies used in reflection like calls (e.g. typeof(MyClass).GetField("fname"))
        //
        protected virtual void MarkReflectionLikeDependencies(MethodIL methodIL, bool requiresReflectionMethodBodyScanner, MessageOrigin origin)
        {
            Debug.Assert(!CompilerGeneratedState.IsNestedFunctionOrStateMachineMember(methodIL.Method));
            // requiresReflectionMethodBodyScanner tells us whether the method body itself requires a dataflow scan.

            // If the method body owns any compiler-generated code, we might still need to do a scan of it together with
            // all of the compiler-generated code it owns, so first check any compiler-generated callees.
            if (Context.CompilerGeneratedState.TryGetCompilerGeneratedCalleesForUserMethod(methodIL.Method, out List<IMemberDefinition>? compilerGeneratedCallees))
            {
                foreach (var compilerGeneratedCallee in compilerGeneratedCallees)
                {
                    switch (compilerGeneratedCallee)
                    {
                        case MethodDefinition nestedFunction:
                            if (nestedFunction.Body is MethodBody nestedBody)
                            {
                                var nestedOrigin = new MessageOrigin(nestedFunction);
                                requiresReflectionMethodBodyScanner |= MarkAndCheckRequiresReflectionMethodBodyScanner(Context.GetMethodIL(nestedBody), nestedOrigin);
                            }
                            break;
                        case TypeDefinition stateMachineType:
                            foreach (var method in stateMachineType.Methods)
                            {
                                if (method.Body is MethodBody stateMachineBody)
                                {
                                    var stateMachineOrigin = new MessageOrigin(method);
                                    requiresReflectionMethodBodyScanner |= MarkAndCheckRequiresReflectionMethodBodyScanner(Context.GetMethodIL(stateMachineBody), stateMachineOrigin);
                                }
                            }
                            break;
                        default:
                            throw new InvalidOperationException();
                    }
                }
            }

            if (!requiresReflectionMethodBodyScanner)
                return;

            var scanner = new ReflectionMethodBodyScanner(Context, this, origin);
            scanner.InterproceduralScan(methodIL);
        }

        protected class AttributeProviderPair
        {
            public AttributeProviderPair(CustomAttribute attribute, ICustomAttributeProvider provider)
            {
                Attribute = attribute;
                Provider = provider;
            }

            public CustomAttribute Attribute { get; private set; }
            public ICustomAttributeProvider Provider { get; private set; }
        }
    }
}
