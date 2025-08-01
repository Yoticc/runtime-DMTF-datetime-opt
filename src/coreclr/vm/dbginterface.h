// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
//
// CLR Debug Interface Header
//



#ifndef _dbgInterface_h_
#define _dbgInterface_h_

#include "common.h"
#include "eedbginterface.h"
#include "corjit.h"
#include "../debug/inc/dbgipcevents.h"
#include "primitives.h"

typedef DPTR(struct ICorDebugInfo::NativeVarInfo) PTR_NativeVarInfo;

typedef void (*FAVORCALLBACK)(void *);

class DebuggerSteppingInfo;

//
// The purpose of this object is to serve as an entry point to the
// debugger, which used to reside in a separate DLL.
//

class DebugInterface
{
    VPTR_BASE_VTABLE_CLASS_AND_CTOR(DebugInterface);

public:

    //
    // Functions exported from the debugger to the EE.
    //

#ifndef DACCESS_COMPILE

    virtual HRESULT Startup(void) = 0;

    virtual HRESULT StartupPhase2(Thread * pThread) = 0;

    // Some callers into the debugger (e.g., ETW rundown) know they will need the lazy
    // data initialized but cannot afford to have it initialized unpredictably or inside a
    // lock.  They can use this function to force the data to be initialized at a
    // controlled point in time
    virtual void InitializeLazyDataIfNecessary() = 0;

    virtual void SetEEInterface(EEDebugInterface* i) = 0;

    virtual void StopDebugger(void) = 0;

    virtual BOOL IsStopped(void) = 0;

    virtual void ThreadCreated(Thread* pRuntimeThread) = 0;

    virtual void ThreadStarted(Thread* pRuntimeThread) = 0;

    virtual void DetachThread(Thread *pRuntimeThread) = 0;

    virtual void AppDomainCreated(AppDomain * pAppDomain) = 0;

    // Called when a module is being loaded into an AppDomain.
    // This includes when a domain neutral module is loaded into a new AppDomain.
    // This is called only when a debugger is attached, and will occur after the
    // related LoadAssembly calls and before any
    // LoadClass calls for this module.
    virtual void LoadModule(Module *     pRuntimeModule,  // the module being loaded
                            LPCWSTR      psModuleName,    // module file name
                            DWORD        dwModuleName,    // number of characters in file name excludign null
                            Assembly *   pAssembly,       // the assembly the module belongs to
                            DomainAssembly * pDomainAssembly,
                            BOOL         fAttaching) = 0; // true if this notification is due to a debugger
                                                          // being attached to the process

    // Called for all modules in an AppDomain when the AppDomain is unloaded.
    // This includes domain neutral modules that are also loaded into other domains.
    // This is called only when a debugger is attached, and will occur after all UnloadClass
    // calls and before any UnloadAssembly or RemoveAppDomainFromIPCBlock calls realted
    // to this module.  On CLR shutdown, we are not guaranteed to get UnloadModule calls for
    // all outstanding loaded modules.
    virtual void UnloadModule(Module* pRuntimeModule) = 0;

    // Called when a Module* is being destroyed.
    // Specifically, the Module has completed unloading (which may have been done asyncronously), all resources
    // associated are being freed, and the Module* is about to become invalid.  The debugger should remove all
    // references to this Module*.
    // NOTE: This is called REGARDLESS of whether a debugger is attached or not, and will occur after any other
    // notifications about this module (including any RemoveAppDomainFromIPCBlock call indicating the module's
    // domain has been unloaded).
    virtual void DestructModule(Module *pModule) = 0;

    virtual BOOL LoadClass(TypeHandle th,
                           mdTypeDef classMetadataToken,
                           Module *classModule) = 0;

    virtual void UnloadClass(mdTypeDef classMetadataToken,
                             Module *classModule) = 0;

    // Filter we call in 1st-pass to dispatch a CHF callback.
    // pCatchStackAddress really should be a Frame* onto the stack. That way the CHF stack address
    // and the debugger's stacktrace Frames will match up.
    // This is only called by stubs.
    virtual LONG NotifyOfCHFFilter(EXCEPTION_POINTERS* pExceptionPointers, PVOID pCatchStackAddr) = 0;


    virtual bool FirstChanceNativeException(EXCEPTION_RECORD *exception,
                                       CONTEXT *context,
                                       DWORD code,
                                       Thread *thread,
                                       BOOL fIsVEH = TRUE) = 0;

    // pThread is thread that exception is on.
    // currentSP is stack frame of the throw site.
    // currentIP is ip of the throw site.
    // pStubFrame = NULL if the currentSp is for a non-stub frame (ie, a regular JITed caught).
    // For stub-based throws, pStubFrame is the EE Frame of the stub.
    virtual bool FirstChanceManagedException(Thread *pThread, SIZE_T currentIP, SIZE_T currentSP) = 0;

    virtual void FirstChanceManagedExceptionCatcherFound(Thread *pThread,
                                                         MethodDesc *pMD, TADDR pMethodAddr,
                                                         BYTE *currentSP,
                                                         EE_ILEXCEPTION_CLAUSE *pEHClause) = 0;

    virtual LONG LastChanceManagedException(EXCEPTION_POINTERS * pExceptionInfo,
            Thread *thread,
            BOOL jitAttachRequested) = 0;

    virtual void ManagedExceptionUnwindBegin(Thread *pThread) = 0;

    virtual void DeleteInterceptContext(void *pContext) = 0;

    virtual void ExceptionFilter(MethodDesc *fd, TADDR pMethodAddr,
                                 SIZE_T offset,
                                 BYTE *pStack) = 0;

    virtual void ExceptionHandle(MethodDesc *fd, TADDR pMethodAddr,
                                 SIZE_T offset,
                                 BYTE *pStack) = 0;

    virtual void SendUserBreakpoint(Thread *thread) = 0;

    // Send an UpdateModuleSyms event, and block waiting for the debugger to continue it.
    virtual void SendUpdateModuleSymsEventAndBlock(Module *pRuntimeModule) = 0;

    //
    // RequestFavor gets the debugger helper thread to call a function. It's
    // typically called when the current thread can't call the function directly,
    // e.g, there isn't enough stack space.
    //
    // RequestFavor ensures that the helper thread has been initialized to
    // execute favors and then calls Debugger:DoFavor. It blocks until the
    // favor callback completes.
    //
    // Parameters:
    //   fp    - Favour callback function
    //   pData - the parameter passed to the favor callback function.
    //
    // Return values:
    //   S_OK if the function succeeds, else a failure HRESULT
    //
    virtual HRESULT RequestFavor(FAVORCALLBACK fp, void * pData) = 0;

#endif // #ifndef DACCESS_COMPILE

    // JITComplete() is called after a method is jit-compiled, successfully or not

#ifndef DACCESS_COMPILE

    virtual void JITComplete(NativeCodeVersion nativeCodeVersion, TADDR newAddress) = 0;

    //
    // EnC functions
    //
#ifdef FEATURE_METADATA_UPDATER
    // Notify that an existing method has been edited in a loaded type
    virtual HRESULT UpdateFunction(MethodDesc* md, SIZE_T enCVersion) = 0;

    // Notify that a new method has been added to a loaded type
    virtual HRESULT AddFunction(MethodDesc* md, SIZE_T enCVersion) = 0;

    virtual HRESULT UpdateNotYetLoadedFunction(mdMethodDef token, Module * pModule, SIZE_T enCVersion) = 0;

    // Notify that a field has been added
    virtual HRESULT AddField(FieldDesc* fd, SIZE_T enCVersion) = 0;

    // Notify that the EE has completed the remap and is about to resume execution
    virtual HRESULT RemapComplete(MethodDesc *pMd, TADDR addr, SIZE_T nativeOffset) = 0;

    // Used by the codemanager FixContextForEnC() to update
    virtual HRESULT MapILInfoToCurrentNative(MethodDesc *pMD,
                                             SIZE_T ilOffset,
                                             TADDR nativeFnxStart,
                                             SIZE_T *nativeOffset) = 0;


    // Used by EditAndContinueModule::FixContextAndResume
    virtual void SendSetThreadContextNeeded(CONTEXT *context, DebuggerSteppingInfo *pDebuggerSteppingInfo = nullptr) = 0;
    virtual BOOL IsOutOfProcessSetContextEnabled() = 0;
#endif // FEATURE_METADATA_UPDATER

    // Get debugger variable information for a specific version of a method
    virtual     void GetVarInfo(MethodDesc *       fd,         // [IN] method of interest
                                CORDB_ADDRESS nativeCodeAddress,    // [IN] which edit version
                                SIZE_T *           cVars,      // [OUT] size of 'vars'
                                const ICorDebugInfo::NativeVarInfo **vars     // [OUT] map telling where local vars are stored
                                ) = 0;

    virtual void getBoundaries(MethodDesc * ftn,
                       unsigned int *cILOffsets, DWORD **pILOffsets,
                       ICorDebugInfo::BoundaryTypes* implicitBoundaries) = 0;

    virtual void getVars(MethodDesc * ftn,
                 ULONG32 *cVars, ICorDebugInfo::ILVarInfo **vars,
                 bool *extendOthers) = 0;

    virtual BOOL CheckGetPatchedOpcode(CORDB_ADDRESS_TYPE *address, /*OUT*/ PRD_TYPE *pOpcode) = 0;

    virtual PRD_TYPE GetPatchedOpcode(CORDB_ADDRESS_TYPE *ip) = 0;

    virtual void TraceCall(const BYTE *target) = 0;

    virtual bool ThreadsAtUnsafePlaces(void) = 0;

    virtual HRESULT LaunchDebuggerForUser(Thread * pThread, EXCEPTION_POINTERS * pExceptionInfo, BOOL sendManagedEvent, BOOL explicitUserRequest) = 0;

    // Launches a debugger and waits for it to attach
    virtual void JitAttach(Thread * pThread, EXCEPTION_POINTERS * pExceptionInfo, BOOL willSendManagedEvent, BOOL explicitUserRequest) = 0;

    // Prepares for a jit attach and decides which of several potentially
    // racing threads get to launch the debugger
    virtual BOOL PreJitAttach(BOOL willSendManagedEvent, BOOL willLaunchDebugger, BOOL explicitUserRequest) = 0;

    // Waits for a jit attach to complete
    virtual void WaitForDebuggerAttach() = 0;

    // Completes the jit attach, unblocking all threads waiting for attach,
    // regardless of whether or not the debugger actually attached
    virtual void PostJitAttach() = 0;

    virtual void SendUserBreakpointAndSynchronize(Thread * pThread) = 0;

    virtual void SendLogMessage(int iLevel,
                                SString * pSwitchName,
                                SString * pMessage) = 0;

    // send a custom notification from the target to the RS. This will become an ICorDebugThread and
    // ICorDebugAppDomain on the RS.
    virtual void SendCustomDebuggerNotification(Thread * pThread, DomainAssembly * pDomainAssembly, mdTypeDef classToken) = 0;

    // Send an MDA notification. This ultimately translates to an ICorDebugMDA object on the Right-Side.
    virtual void SendMDANotification(
        Thread * pThread, // may be NULL. Lets us send on behalf of other threads.
        SString * szName,
        SString * szDescription,
        SString * szXML,
        CorDebugMDAFlags flags,
        BOOL bAttach
    ) = 0;

    virtual bool IsJMCMethod(Module* pModule, mdMethodDef tkMethod) = 0;

    virtual void SendLogSwitchSetting (int iLevel,
                                       int iReason,
                                       _In_z_ LPCWSTR pLogSwitchName,
                                       _In_z_ LPCWSTR pParentSwitchName) = 0;

    virtual bool IsLoggingEnabled (void) = 0;

    virtual bool GetILOffsetFromNative (MethodDesc *PFD,
                                                                            const BYTE *pbAddr,
                                                                            DWORD nativeOffset,
                                                                            DWORD *ilOffset) = 0;

    virtual HRESULT GetILToNativeMapping(PCODE pNativeCodeStartAddress,
                                         ULONG32 cMap,
                                         ULONG32 *pcMap,
                                         COR_DEBUG_IL_TO_NATIVE_MAP map[]) = 0;

#ifdef DEBUG
    virtual HRESULT GetILToNativeMappingIntoArrays(
        MethodDesc * pMethodDesc,
        PCODE pNativeCodeStartAddress,
        USHORT cMapMax,
        USHORT * pcMap,
        UINT ** prguiILOffset,
        UINT ** prguiNativeOffset) = 0;
#endif // DEBUG

    virtual DWORD GetHelperThreadID(void ) = 0;

    // Called for all assemblies in an AppDomain when the AppDomain is unloaded.
    // This includes domain neutral assemblies that are also loaded into other domains.
    // This is called only when a debugger is attached, and will occur after all UnloadClass
    // and UnloadModule calls and before any RemoveAppDomainFromIPCBlock calls realted
    // to this assembly.  On CLR shutdown, we are not guaranteed to get UnloadAssembly calls for
    // all outstanding loaded assemblies.
    virtual void UnloadAssembly(DomainAssembly * pDomainAssembly) = 0;

    virtual HRESULT SetILInstrumentedCodeMap(MethodDesc *fd,
                                             BOOL fStartJit,
                                             ULONG32 cILMapEntries,
                                             COR_IL_MAP rgILMapEntries[]) = 0;

    virtual void EarlyHelperThreadDeath(void) = 0;

    virtual void ShutdownBegun(void) = 0;

    virtual void LockDebuggerForShutdown(void) = 0;

    virtual void DisableDebugger(void) = 0;

    virtual HRESULT NameChangeEvent(AppDomain *pAppDomain,
                                    Thread *pThread) = 0;

    // send an event to the RS indicating that there's a Ctrl-C or Ctrl-Break
    virtual BOOL SendCtrlCToDebugger(DWORD dwCtrlType) = 0;

    // Allows the debugger to keep an up to date list of special threads
    virtual HRESULT UpdateSpecialThreadList(DWORD cThreadArrayLength,
                                            DWORD *rgdwThreadIDArray) = 0;

    virtual DWORD GetRCThreadId(void) = 0;

    virtual HRESULT GetVariablesFromOffset(MethodDesc                 *pMD,
                                           UINT                        varNativeInfoCount,
                                           ICorDebugInfo::NativeVarInfo *varNativeInfo,
                                           SIZE_T                      offsetFrom,
                                           CONTEXT                    *pCtx,
                                           SIZE_T                     *rgVal1,
                                           SIZE_T                     *rgVal2,
                                           UINT                       uRgValSize,
                                           BYTE                     ***rgpVCs) = 0;

    virtual HRESULT SetVariablesAtOffset(MethodDesc                 *pMD,
                                         UINT                        varNativeInfoCount,
                                         ICorDebugInfo::NativeVarInfo *varNativeInfo,
                                         SIZE_T                      offsetTo,
                                         CONTEXT                    *pCtx,
                                         SIZE_T                     *rgVal1,
                                         SIZE_T                     *rgVal2,
                                         BYTE                      **rgpVCs) = 0;

    virtual BOOL IsThreadContextInvalid(Thread *pThread, CONTEXT *pCtx) = 0;

    // For Just-My-Code (aka Just-User-Code).
    // The jit inserts probes that look like.
    // if (*pAddr != 0) call g_pDebugInterface->OnMethodEnter()

    // Invoked when we enter a user method.
    // pIP is an ip within the method, right after the prolog.
    virtual void OnMethodEnter(void * pIP) = 0;

    // Given a method, the debugger provides the address of the flag.
    // This allows the debugger to store the flag whereever it wants
    // and with whatever granularity (per-module, per-class, per-function, etc).
    virtual DWORD* GetJMCFlagAddr(Module * pModule) = 0;

    // notification for SQL fiber debugging support
    virtual void CreateConnection(CONNID dwConnectionId, _In_z_ WCHAR *wzName) = 0;
    virtual void DestroyConnection(CONNID dwConnectionId) = 0;
    virtual void ChangeConnection(CONNID dwConnectionId) = 0;

    //
    // This function is used to identify the helper thread.
    //
    virtual bool ThisIsHelperThread(void) = 0;

    virtual HRESULT ReDaclEvents(PSECURITY_DESCRIPTOR securityDescriptor) = 0;

    virtual BOOL ShouldAutoAttach() = 0;
    virtual BOOL FallbackJITAttachPrompt() = 0;

#ifdef FEATURE_INTEROP_DEBUGGING
    virtual LONG FirstChanceSuspendHijackWorker(PCONTEXT pContext, PEXCEPTION_RECORD pExceptionRecord, BOOL fIsVEH = TRUE) = 0;
#endif

    // Helper method for cleaning up transport socket
    virtual void CleanupTransportSocket(void) = 0;

#endif // #ifndef DACCESS_COMPILE

#ifdef DACCESS_COMPILE
    virtual void EnumMemoryRegions(CLRDataEnumMemoryFlags flags) = 0;
    virtual void EnumMemoryRegionsIfFuncEvalFrame(CLRDataEnumMemoryFlags flags, Frame * pFrame) = 0;
#endif
#ifndef DACCESS_COMPILE
    virtual void SuspendForGarbageCollectionStarted() = 0;
    virtual void SuspendForGarbageCollectionCompleted() = 0;
    virtual void ResumeForGarbageCollectionStarted() = 0;
#endif
    virtual BOOL IsSynchronizing() = 0;

#ifndef DACCESS_COMPILE
    virtual HRESULT DeoptimizeMethod(Module* pModule, mdMethodDef methodDef) = 0;
    virtual HRESULT IsMethodDeoptimized(Module *pModule, mdMethodDef methodDef, BOOL *pResult) = 0;
    virtual void MulticastTraceNextStep(DELEGATEREF pbDel, INT32 count) = 0;
    virtual void ExternalMethodFixupNextStep(PCODE address) = 0;
#endif //DACCESS_COMPILE
};

#ifndef DACCESS_COMPILE
// Helper to make GCC compile. GCC can't handle putting a virtual call in a filter.
struct NotifyOfCHFFilterWrapperParam { void *pFrame; };
LONG NotifyOfCHFFilterWrapper(EXCEPTION_POINTERS *pExceptionInfo, PVOID pNotifyOfCHFFilterWrapperParam);
#endif


#endif // _dbgInterface_h_
