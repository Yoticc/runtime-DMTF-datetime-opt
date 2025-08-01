// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#include "common.h"

#include "appdomain.hpp"
#include "peimagelayout.inl"
#include "field.h"
#include "strongnameinternal.h"
#include "excep.h"
#include "eeconfig.h"
#include "gcheaputilities.h"
#include "eventtrace.h"
#include "eeprofinterfaces.h"
#include "dbginterface.h"
#ifndef DACCESS_COMPILE
#include "eedbginterfaceimpl.h"
#endif
#include "comdynamic.h"
#include "mlinfo.h"
#include "posterror.h"
#include "assemblynative.hpp"
#include "stringliteralmap.h"
#include "frozenobjectheap.h"
#include "codeman.h"
#include "comcallablewrapper.h"
#include "eventtrace.h"
#include "comdelegate.h"
#include "siginfo.hpp"
#include "typekey.h"
#include "castcache.h"

#include "caparser.h"
#include "ecall.h"
#include "finalizerthread.h"
#include "threadsuspend.h"

#ifdef FEATURE_COMINTEROP
#include "comtoclrcall.h"
#include "runtimecallablewrapper.h"
#include "olevariant.h"
#include "olecontexthelpers.h"
#endif // FEATURE_COMINTEROP

#if defined(FEATURE_COMWRAPPERS)
#include "rcwrefcache.h"
#endif // FEATURE_COMWRAPPERS

#include "typeequivalencehash.hpp"

#include "appdomain.inl"

#ifndef TARGET_UNIX
#include "dwreport.h"
#endif // !TARGET_UNIX

#include "stringarraylist.h"

#include "../binder/inc/bindertracing.h"
#include "../binder/inc/defaultassemblybinder.h"
#include "../binder/inc/assemblybindercommon.hpp"

// this file handles string conversion errors for itself
#undef  MAKE_TRANSLATIONFAILED

// Define these macro's to do strict validation for jit lock and class
// init entry leaks.  This defines determine if the asserts that
// verify for these leaks are defined or not.  These asserts can
// sometimes go off even if no entries have been leaked so this
// defines should be used with caution.
//
// If we are inside a .cctor when the application shut's down then the
// class init lock's head will be set and this will cause the assert
// to go off.
//
// If we are jitting a method when the application shut's down then
// the jit lock's head will be set causing the assert to go off.

//#define STRICT_CLSINITLOCK_ENTRY_LEAK_DETECTION

static const WCHAR DEFAULT_DOMAIN_FRIENDLY_NAME[] = W("DefaultDomain");

#define STATIC_OBJECT_TABLE_BUCKET_SIZE 1020

// Statics

SPTR_IMPL(AppDomain, AppDomain, m_pTheAppDomain);
SPTR_IMPL(SystemDomain, SystemDomain, m_pSystemDomain);

#ifndef DACCESS_COMPILE

// System Domain Statics
GlobalStringLiteralMap*  SystemDomain::m_pGlobalStringLiteralMap = NULL;
FrozenObjectHeapManager* SystemDomain::m_FrozenObjectHeapManager = NULL;

DECLSPEC_ALIGN(16)
static BYTE         g_pSystemDomainMemory[sizeof(SystemDomain)];

CrstStatic          SystemDomain::m_SystemDomainCrst;
CrstStatic          SystemDomain::m_DelayedUnloadCrst;

// Constructor for the PinnedHeapHandleBucket class.
PinnedHeapHandleBucket::PinnedHeapHandleBucket(PinnedHeapHandleBucket *pNext, PTRARRAYREF pinnedHandleArrayObj, DWORD size)
: m_pNext(pNext)
, m_ArraySize(size)
, m_CurrentPos(0)
, m_CurrentEmbeddedFreePos(0) // hint for where to start a search for an embedded free item
{
    CONTRACTL
    {
        THROWS;
        GC_NOTRIGGER;
        MODE_COOPERATIVE;
        INJECT_FAULT(COMPlusThrowOM(););
    }
    CONTRACTL_END;

    // Retrieve the pointer to the data inside the array. This is legal since the array
    // is located in the pinned object heap and is guaranteed not to move.
    m_pArrayDataPtr = (OBJECTREF *)pinnedHandleArrayObj->GetDataPtr();

    // Store the array in a strong handle to keep it alive.
    m_hndHandleArray = AppDomain::GetCurrentDomain()->CreateStrongHandle((OBJECTREF)pinnedHandleArrayObj);
}


// Destructor for the PinnedHeapHandleBucket class.
PinnedHeapHandleBucket::~PinnedHeapHandleBucket()
{
    CONTRACTL
    {
        NOTHROW;
        GC_NOTRIGGER;
    }
    CONTRACTL_END;

    if (m_hndHandleArray)
    {
        DestroyStrongHandle(m_hndHandleArray);
        m_hndHandleArray = NULL;
    }
}


// Allocate handles from the bucket.
OBJECTREF *PinnedHeapHandleBucket::AllocateHandles(DWORD nRequested)
{
    CONTRACTL
    {
        NOTHROW;
        GC_NOTRIGGER;
        MODE_COOPERATIVE;
    }
    CONTRACTL_END;

    _ASSERTE(nRequested > 0 && nRequested <= GetNumRemainingHandles());
    _ASSERTE(m_pArrayDataPtr == (OBJECTREF*)((PTRARRAYREF)ObjectFromHandle(m_hndHandleArray))->GetDataPtr());

    // Store the handles in the buffer that was passed in
    OBJECTREF* ret = &m_pArrayDataPtr[m_CurrentPos];
    m_CurrentPos += nRequested;

    return ret;
}

// look for a free item embedded in the table
OBJECTREF *PinnedHeapHandleBucket::TryAllocateEmbeddedFreeHandle()
{
    CONTRACTL
    {
        NOTHROW;
        GC_NOTRIGGER;
        MODE_COOPERATIVE;
    }
    CONTRACTL_END;

    OBJECTREF pPreallocatedSentinelObject = ObjectFromHandle(g_pPreallocatedSentinelObject);
    _ASSERTE(pPreallocatedSentinelObject  != NULL);

    for (int  i = m_CurrentEmbeddedFreePos; i < m_CurrentPos; i++)
    {
        if (m_pArrayDataPtr[i] == pPreallocatedSentinelObject)
        {
            m_CurrentEmbeddedFreePos = i;
            m_pArrayDataPtr[i] = NULL;
            return &m_pArrayDataPtr[i];
        }
    }

    // didn't find it (we don't bother wrapping around for a full search, it's not worth it to try that hard, we'll get it next time)

    m_CurrentEmbeddedFreePos = 0;
    return NULL;
}

// enumerate the handles in the bucket
void PinnedHeapHandleBucket::EnumStaticGCRefs(promote_func* fn, ScanContext* sc)
{
    for (int i = 0; i < m_CurrentPos; i++)
    {
        fn((Object**)&m_pArrayDataPtr[i], sc, 0);
    }
}


// Maximum bucket size will be 64K on 32-bit and 128K on 64-bit.
// We subtract out a small amount to leave room for the object
// header and length of the array.

#define MAX_BUCKETSIZE (16384 - 4)

// Constructor for the PinnedHeapHandleTable class.
PinnedHeapHandleTable::PinnedHeapHandleTable(DWORD InitialBucketSize)
: m_pHead(NULL)
, m_NextBucketSize(InitialBucketSize)
, m_pFreeSearchHint(NULL)
, m_cEmbeddedFree(0)
{
    CONTRACTL
    {
        THROWS;
        GC_TRIGGERS;
        MODE_COOPERATIVE;
        INJECT_FAULT(COMPlusThrowOM(););
    }
    CONTRACTL_END;

    m_Crst.Init(CrstPinnedHeapHandleTable, CRST_UNSAFE_COOPGC);
}


// Destructor for the PinnedHeapHandleTable class.
PinnedHeapHandleTable::~PinnedHeapHandleTable()
{
    CONTRACTL
    {
        NOTHROW;
        GC_NOTRIGGER;
    }
    CONTRACTL_END;

    // Delete the buckets.
    while (m_pHead)
    {
        PinnedHeapHandleBucket *pOld = m_pHead;
        m_pHead = pOld->GetNext();
        delete pOld;
    }
}


// Allocate handles from the large heap handle table.
// This function is thread-safe for concurrent invocation by multiple callers
OBJECTREF* PinnedHeapHandleTable::AllocateHandles(DWORD nRequested)
{
    CONTRACTL
    {
        THROWS;
        GC_TRIGGERS;
        MODE_COOPERATIVE;
        PRECONDITION(nRequested > 0);
        INJECT_FAULT(COMPlusThrowOM(););
    }
    CONTRACTL_END;

#ifdef _DEBUG
    _ASSERTE(!m_Crst.OwnedByCurrentThread());
#endif

    // beware: we leave and re-enter this lock below in this method
    CrstHolderWithState lockHolder(&m_Crst);

    if (nRequested == 1 && m_cEmbeddedFree != 0)
    {
        // special casing singleton requests to look for slots that can be re-used

        // we need to do this because string literals are allocated one at a time and then sometimes
        // released.  we do not wish for the number of handles consumed by string literals to
        // increase forever as assemblies are loaded and unloaded

        if (m_pFreeSearchHint == NULL)
            m_pFreeSearchHint = m_pHead;

        while (m_pFreeSearchHint)
        {
            OBJECTREF* pObjRef = m_pFreeSearchHint->TryAllocateEmbeddedFreeHandle();
            if (pObjRef != NULL)
            {
                // the slot is to have been prepared with a null ready to go
                _ASSERTE(*pObjRef == NULL);
                m_cEmbeddedFree--;
                return pObjRef;
            }
            m_pFreeSearchHint = m_pFreeSearchHint->GetNext();
        }

        // the search doesn't wrap around so it's possible that we might have embedded free items
        // and not find them but that's ok, we'll get them on the next alloc... all we're trying to do
        // is to not have big leaks over time.
    }


    // Retrieve the remaining number of handles in the bucket.
    DWORD numRemainingHandlesInBucket = (m_pHead != NULL) ? m_pHead->GetNumRemainingHandles() : 0;
    PTRARRAYREF pinnedHandleArrayObj = NULL;
    DWORD nextBucketSize = min<DWORD>(m_NextBucketSize * 2, MAX_BUCKETSIZE);

    // create a new block if this request doesn't fit in the current block
    if (nRequested > numRemainingHandlesInBucket)
    {
        // create a new bucket for this allocation
        // We need a block big enough to hold the requested handles
        DWORD newBucketSize = max(m_NextBucketSize, nRequested);

        // Leave the lock temporarily to do the GC allocation
        //
        // Why do we need to do certain work outside the lock? Because if we didn't this can happen:
        // 1. AllocateHandles needs the GC to allocate
        // 2. Anything which invokes the GC might also get suspended by the managed debugger which
        //    will block the thread inside the lock
        // 3. The managed debugger can run function-evaluation on any thread
        // 4. Those func-evals might need to allocate handles
        // 5. The func-eval can't acquire the lock to allocate handles because the thread in
        //    step (3) still holds the lock. The thread in step (3) won't release the lock until the
        //    debugger allows it to resume. The debugger won't resume until the funceval completes.
        // 6. This either creates a deadlock or forces the debugger to abort the func-eval with a bad
        //    user experience.
        //
        // This is only a partial fix to the func-eval problem. Some of the callers to AllocateHandles()
        // are holding their own different locks farther up the stack. To address this more completely
        // we probably need to change the fundamental invariant that all GC suspend points are also valid
        // debugger suspend points. Changes in that area have proven to be error-prone in the past and we
        // don't yet have the appropriate testing to validate that a future attempt gets it correct.
        lockHolder.Release();
        {
            OVERRIDE_TYPE_LOAD_LEVEL_LIMIT(CLASS_LOADED);
            pinnedHandleArrayObj = (PTRARRAYREF)AllocateObjectArray(newBucketSize, g_pObjectClass, /* bAllocateInPinnedHeap = */TRUE);
        }
        lockHolder.Acquire();

        // after leaving and re-entering the lock anything we verified or computed above using internal state could
        // have changed. We need to retest if we still need the new allocation.
        numRemainingHandlesInBucket = (m_pHead != NULL) ? m_pHead->GetNumRemainingHandles() : 0;
        if (nRequested > numRemainingHandlesInBucket)
        {
            if (m_pHead != NULL)
            {
                // mark the handles in that remaining region as available for re-use
                ReleaseHandlesLocked(m_pHead->CurrentPos(), numRemainingHandlesInBucket);

                // mark what's left as having been used
                m_pHead->ConsumeRemaining();
            }

            m_pHead = new PinnedHeapHandleBucket(m_pHead, pinnedHandleArrayObj, newBucketSize);

            // we already computed nextBucketSize to be double the previous size above, but it is possible that
            // other threads increased m_NextBucketSize while the lock was unheld. We want to ensure
            // m_NextBucketSize never shrinks even if nextBucketSize is no longer m_NextBucketSize*2.
            m_NextBucketSize = max(m_NextBucketSize, nextBucketSize);
        }
        else
        {
            // we didn't need the allocation after all
            // no handle has been created to root this so the GC may be able to reclaim and reuse it
            pinnedHandleArrayObj = NULL;
        }
    }

    return m_pHead->AllocateHandles(nRequested);
}

//*****************************************************************************
// Release object handles allocated using AllocateHandles().
// This function is thread-safe for concurrent invocation by multiple callers
void PinnedHeapHandleTable::ReleaseHandles(OBJECTREF *pObjRef, DWORD nReleased)
{
    CONTRACTL
    {
        NOTHROW;
        GC_NOTRIGGER;
        MODE_COOPERATIVE;
        PRECONDITION(CheckPointer(pObjRef));
    }
    CONTRACTL_END;

#ifdef _DEBUG
    _ASSERTE(!m_Crst.OwnedByCurrentThread());
#endif

    CrstHolder ch(&m_Crst);
    ReleaseHandlesLocked(pObjRef, nReleased);
}

void PinnedHeapHandleTable::ReleaseHandlesLocked(OBJECTREF *pObjRef, DWORD nReleased)
{
    CONTRACTL
    {
        NOTHROW;
        GC_NOTRIGGER;
        MODE_COOPERATIVE;
        PRECONDITION(CheckPointer(pObjRef));
    }
    CONTRACTL_END;

#ifdef _DEBUG
    _ASSERTE(m_Crst.OwnedByCurrentThread());
#endif

    OBJECTREF pPreallocatedSentinelObject = ObjectFromHandle(g_pPreallocatedSentinelObject);
    _ASSERTE(pPreallocatedSentinelObject  != NULL);


    // Add the released handles to the list of available handles.
    for (DWORD i = 0; i < nReleased; i++)
    {
        SetObjectReference(&pObjRef[i], pPreallocatedSentinelObject);
    }

    m_cEmbeddedFree += nReleased;
}

// enumerate the handles in the handle table
void PinnedHeapHandleTable::EnumStaticGCRefs(promote_func* fn, ScanContext* sc)
{
    for (PinnedHeapHandleBucket *pBucket = m_pHead; pBucket != nullptr; pBucket = pBucket->GetNext())
    {
        pBucket->EnumStaticGCRefs(fn, sc);
    }
}

#undef LOADERHEAP_PROFILE_COUNTER

void AppDomain::ClearBinderContext()
{
    CONTRACTL
    {
        NOTHROW;
        GC_TRIGGERS;
        MODE_PREEMPTIVE;
    }
    CONTRACTL_END;

    if (m_pDefaultBinder)
    {
        delete m_pDefaultBinder;
        m_pDefaultBinder = NULL;
    }
}

void AppDomain::ShutdownFreeLoaderAllocators()
{
    // If we're called from managed code (i.e. the finalizer thread) we take a lock in
    // LoaderAllocator::CleanupFailedTypeInit, which may throw. Otherwise we're called
    // from the app-domain shutdown path in which we can avoid taking the lock.
    CONTRACTL
    {
        GC_TRIGGERS;
        THROWS;
        MODE_ANY;
        CAN_TAKE_LOCK;
    }
    CONTRACTL_END;

    CrstHolder ch(GetLoaderAllocatorReferencesLock());

    // Shutdown the LoaderAllocators associated with collectible assemblies
    while (m_pDelayedLoaderAllocatorUnloadList != NULL)
    {
        LoaderAllocator * pCurrentLoaderAllocator = m_pDelayedLoaderAllocatorUnloadList;
        // Remove next loader allocator from the list
        m_pDelayedLoaderAllocatorUnloadList = m_pDelayedLoaderAllocatorUnloadList->m_pLoaderAllocatorDestroyNext;

        // For loader allocator finalization, we need to be careful about cleaning up per-appdomain allocations
        // and synchronizing with GC using delay unload list. We need to wait for next Gen2 GC to finish to ensure
        // that GC heap does not have any references to the MethodTables being unloaded.

        pCurrentLoaderAllocator->CleanupFailedTypeInit();

        pCurrentLoaderAllocator->CleanupHandles();

        GCX_COOP();
        SystemDomain::System()->AddToDelayedUnloadList(pCurrentLoaderAllocator);
    }
} // AppDomain::ShutdownFreeLoaderAllocators

//---------------------------------------------------------------------------------------
//
// Register the loader allocator for deletion in code:AppDomain::ShutdownFreeLoaderAllocators.
//
void AppDomain::RegisterLoaderAllocatorForDeletion(LoaderAllocator * pLoaderAllocator)
{
    CONTRACTL
    {
        GC_TRIGGERS;
        NOTHROW;
        MODE_ANY;
        CAN_TAKE_LOCK;
    }
    CONTRACTL_END;

    CrstHolder ch(GetLoaderAllocatorReferencesLock());

    pLoaderAllocator->m_pLoaderAllocatorDestroyNext = m_pDelayedLoaderAllocatorUnloadList;
    m_pDelayedLoaderAllocatorUnloadList = pLoaderAllocator;
}

void AppDomain::SetNativeDllSearchDirectories(LPCWSTR wszNativeDllSearchDirectories)
{
    STANDARD_VM_CONTRACT;

    SString sDirectories(wszNativeDllSearchDirectories);

    if (sDirectories.GetCount() > 0)
    {
        SString::CIterator start = sDirectories.Begin();
        SString::CIterator itr = sDirectories.Begin();
        SString::CIterator end = sDirectories.End();
        SString qualifiedPath;

        while (itr != end)
        {
            start = itr;
            BOOL found = sDirectories.Find(itr, PATH_SEPARATOR_CHAR_W);
            if (!found)
            {
                itr = end;
            }

            SString qualifiedPath(sDirectories, start, itr);

            if (found)
            {
                itr++;
            }

            unsigned len = qualifiedPath.GetCount();

            if (len > 0)
            {
                if (qualifiedPath[len - 1] != DIRECTORY_SEPARATOR_CHAR_W)
                {
                    qualifiedPath.Append(DIRECTORY_SEPARATOR_CHAR_W);
                }

                NewHolder<SString> stringHolder(new SString(qualifiedPath));
                IfFailThrow(m_NativeDllSearchDirectories.Append(stringHolder.GetValue()));
                stringHolder.SuppressRelease();
            }
        }
    }
}

OBJECTREF* AppDomain::AllocateObjRefPtrsInLargeTable(int nRequested, DynamicStaticsInfo* pStaticsInfo, MethodTable *pMTToFillWithStaticBoxes, bool isClassInitdeByUpdatingStaticPointer)
{
    CONTRACTL
    {
        THROWS;
        GC_TRIGGERS;
        MODE_ANY;
        PRECONDITION((nRequested > 0));
        INJECT_FAULT(COMPlusThrowOM(););
    }
    CONTRACTL_END;

    if (pStaticsInfo && pStaticsInfo->GetGCStaticsPointer() != NULL)
    {
        // Allocation already happened
        return pStaticsInfo->GetGCStaticsPointer();
    }

    GCX_COOP();

    // Make sure the large heap handle table is initialized.
    if (!m_pPinnedHeapHandleTable)
        InitPinnedHeapHandleTable();

    // Allocate the handles.
    OBJECTREF* result = m_pPinnedHeapHandleTable->AllocateHandles(nRequested);
    if (pMTToFillWithStaticBoxes != NULL)
    {
        GCPROTECT_BEGININTERIOR(result);
        pMTToFillWithStaticBoxes->AllocateRegularStaticBoxes(&result);
        GCPROTECT_END();
    }
    if (pStaticsInfo)
    {
        // race with other threads that might be doing the same concurrent allocation
        if (!pStaticsInfo->InterlockedUpdateStaticsPointer(/*isGCPointer*/ true, (TADDR)result, isClassInitdeByUpdatingStaticPointer))
        {
            // we lost the race, release our handles and use the handles from the
            // winning thread
            m_pPinnedHeapHandleTable->ReleaseHandles(result, nRequested);
            result = pStaticsInfo->GetGCStaticsPointer();
        }
    }

    return result;
}

#endif // !DACCESS_COMPILE

#ifdef FEATURE_COMINTEROP
#ifndef DACCESS_COMPILE

OBJECTREF AppDomain::GetMissingObject()
{
    CONTRACTL
    {
        THROWS;
        GC_TRIGGERS;
        MODE_COOPERATIVE;
    }
    CONTRACTL_END;

    if (!m_hndMissing)
    {
        // Get the field
        FieldDesc *pValueFD = CoreLibBinder::GetField(FIELD__MISSING__VALUE);

        pValueFD->CheckRunClassInitThrowing();

        // Retrieve the value static field and store it.
        OBJECTHANDLE hndMissing = CreateHandle(pValueFD->GetStaticOBJECTREF());

        if (InterlockedCompareExchangeT(&m_hndMissing, hndMissing, NULL) != NULL)
        {
            // Exchanged failed. The m_hndMissing did not equal NULL and was returned.
            DestroyHandle(hndMissing);
        }
    }

    return ObjectFromHandle(m_hndMissing);
}

#endif // DACCESS_COMPILE
#endif // FEATURE_COMINTEROP

#ifndef DACCESS_COMPILE


STRINGREF* AppDomain::IsStringInterned(STRINGREF *pString)
{
    CONTRACTL
    {
        GC_TRIGGERS;
        THROWS;
        MODE_COOPERATIVE;
        PRECONDITION(CheckPointer(pString));
        INJECT_FAULT(COMPlusThrowOM(););
    }
    CONTRACTL_END;

    return GetLoaderAllocator()->IsStringInterned(pString);
}

STRINGREF* AppDomain::GetOrInternString(STRINGREF *pString)
{
    CONTRACTL
    {
        GC_TRIGGERS;
        THROWS;
        MODE_COOPERATIVE;
        PRECONDITION(CheckPointer(pString));
        INJECT_FAULT(COMPlusThrowOM(););
    }
    CONTRACTL_END;

    return GetLoaderAllocator()->GetOrInternString(pString);
}

void AppDomain::InitPinnedHeapHandleTable()
{
    CONTRACTL
    {
        THROWS;
        GC_TRIGGERS;
        MODE_COOPERATIVE;
        INJECT_FAULT(COMPlusThrowOM(););
    }
    CONTRACTL_END;

    PinnedHeapHandleTable* pTable = new PinnedHeapHandleTable(STATIC_OBJECT_TABLE_BUCKET_SIZE);
    if(InterlockedCompareExchangeT<PinnedHeapHandleTable*>(&m_pPinnedHeapHandleTable, pTable, NULL) != NULL)
    {
        // another thread beat us to initializing the field, delete our copy
        delete pTable;
    }
}


//*****************************************************************************
//*****************************************************************************
//*****************************************************************************

void *SystemDomain::operator new(size_t size, void *pInPlace)
{
    LIMITED_METHOD_CONTRACT;
    return pInPlace;
}


void SystemDomain::operator delete(void *pMem)
{
    LIMITED_METHOD_CONTRACT;
    // Do nothing - new() was in-place
}

void SystemDomain::Attach()
{
    CONTRACTL
    {
        THROWS;
        GC_TRIGGERS;
        MODE_ANY;
        PRECONDITION(m_pSystemDomain == NULL);
        INJECT_FAULT(COMPlusThrowOM(););
    }
    CONTRACTL_END;

    // Initialize stub managers
    PrecodeStubManager::Init();
    JumpStubStubManager::Init();
    RangeSectionStubManager::Init();
    ILStubManager::Init();
    InteropDispatchStubManager::Init();
    StubLinkStubManager::Init();
    TailCallStubManager::Init();
#ifdef FEATURE_TIERED_COMPILATION
    CallCountingStubManager::Init();
#endif

    m_SystemDomainCrst.Init(CrstSystemDomain, (CrstFlags)(CRST_REENTRANCY | CRST_TAKEN_DURING_SHUTDOWN));
    m_DelayedUnloadCrst.Init(CrstSystemDomainDelayedUnloadList, CRST_UNSAFE_COOPGC);

    // Create the global SystemDomain and initialize it.
    m_pSystemDomain = new (&g_pSystemDomainMemory[0]) SystemDomain();
    // No way it can fail since g_pSystemDomainMemory is a static array.
    CONSISTENCY_CHECK(CheckPointer(m_pSystemDomain));

    LOG((LF_CLASSLOADER,
         LL_INFO10,
         "Created system domain at %p\n",
         m_pSystemDomain));

    // Create the one and only app domain
    AppDomain::Create();

    // Each domain gets its own ReJitManager, and ReJitManager has its own static
    // initialization to run
    ReJitManager::InitStatic();

#ifdef FEATURE_READYTORUN
    InitReadyToRunStandaloneMethodMetadata();
#endif // FEATURE_READYTORUN
}


void SystemDomain::DetachBegin()
{
    WRAPPER_NO_CONTRACT;
    // Shut down the domain and its children (but don't deallocate anything just
    // yet).

    if (GetThreadNULLOk() == NULL)
    {
        return;
    }

    if(m_pSystemDomain)
        m_pSystemDomain->Stop();
}

void SystemDomain::DetachEnd()
{
    CONTRACTL
    {
        NOTHROW;
        GC_TRIGGERS;
        MODE_ANY;
    }
    CONTRACTL_END;
    // Shut down the domain and its children (but don't deallocate anything just
    // yet).
    if(m_pSystemDomain)
    {
        GCX_PREEMP();
        AppDomain* pAppDomain = GetAppDomain();
        if (pAppDomain)
            pAppDomain->ClearBinderContext();
    }
}

void SystemDomain::Stop()
{
    WRAPPER_NO_CONTRACT;
    AppDomain::GetCurrentDomain()->Stop();
}

void SystemDomain::PreallocateSpecialObjects()
{
    CONTRACTL
    {
        THROWS;
        GC_TRIGGERS;
        MODE_COOPERATIVE;
        INJECT_FAULT(COMPlusThrowOM(););
    }
    CONTRACTL_END;

    _ASSERTE(g_pPreallocatedSentinelObject == NULL);

    OBJECTREF pPreallocatedSentinelObject = AllocateObject(g_pObjectClass);
    g_pPreallocatedSentinelObject = AppDomain::GetCurrentDomain()->CreatePinningHandle( pPreallocatedSentinelObject );
}

void SystemDomain::CreatePreallocatedExceptions()
{
    CONTRACTL
    {
        THROWS;
        GC_TRIGGERS;
        MODE_COOPERATIVE;
        INJECT_FAULT(COMPlusThrowOM(););
    }
    CONTRACTL_END;

    EXCEPTIONREF pOutOfMemory = (EXCEPTIONREF)AllocateObject(g_pOutOfMemoryExceptionClass);
    pOutOfMemory->SetHResult(COR_E_OUTOFMEMORY);
    pOutOfMemory->SetXCode(EXCEPTION_COMPLUS);
    _ASSERTE(g_pPreallocatedOutOfMemoryException == NULL);
    g_pPreallocatedOutOfMemoryException = AppDomain::GetCurrentDomain()->CreateHandle(pOutOfMemory);


    EXCEPTIONREF pStackOverflow = (EXCEPTIONREF)AllocateObject(g_pStackOverflowExceptionClass);
    pStackOverflow->SetHResult(COR_E_STACKOVERFLOW);
    pStackOverflow->SetXCode(EXCEPTION_COMPLUS);
    _ASSERTE(g_pPreallocatedStackOverflowException == NULL);
    g_pPreallocatedStackOverflowException = AppDomain::GetCurrentDomain()->CreateHandle(pStackOverflow);


    EXCEPTIONREF pExecutionEngine = (EXCEPTIONREF)AllocateObject(g_pExecutionEngineExceptionClass);
    pExecutionEngine->SetHResult(COR_E_EXECUTIONENGINE);
    pExecutionEngine->SetXCode(EXCEPTION_COMPLUS);
    _ASSERTE(g_pPreallocatedExecutionEngineException == NULL);
    g_pPreallocatedExecutionEngineException = AppDomain::GetCurrentDomain()->CreateHandle(pExecutionEngine);
}

void SystemDomain::Init()
{
    STANDARD_VM_CONTRACT;

    // The AppDomain should have already been created
    _ASSERTE(AppDomain::GetCurrentDomain() != NULL);

    HRESULT hr = S_OK;

#ifdef _DEBUG
    LOG((
        LF_EEMEM,
        LL_INFO10,
        "sizeof(EEClass)     = %d\n"
        "sizeof(MethodTable) = %d\n"
        "sizeof(MethodDesc)= %d\n"
        "sizeof(FieldDesc)   = %d\n"
        "sizeof(Module)      = %d\n",
        sizeof(EEClass),
        sizeof(MethodTable),
        sizeof(MethodDesc),
        sizeof(FieldDesc),
        sizeof(Module)
        ));
#endif // _DEBUG

    m_pSystemPEAssembly = NULL;
    m_pSystemAssembly = NULL;

    // Get the install directory so we can find CoreLib
    IfFailThrow(GetClrModuleDirectory(m_SystemDirectory));
    m_SystemDirectory.Normalize();

    // At this point m_SystemDirectory should already be canonicalized
    m_BaseLibrary.Append(m_SystemDirectory);
    m_BaseLibrary.Append(g_pwBaseLibrary);
    m_BaseLibrary.Normalize();

    LoadBaseSystemClasses();

    {
        // We are about to start allocating objects, so we must be in cooperative mode.
        // However, many of the entrypoints to the system (DllGetClassObject and all
        // PInvoke exports) get called multiple times.  Sometimes they initialize the EE,
        // but generally they remain in preemptive mode.  So we really want to push/pop
        // the state here:
        GCX_COOP();

        CreatePreallocatedExceptions();
        PreallocateSpecialObjects();

        // Finish loading CoreLib now.
        m_pSystemAssembly->EnsureActive();

        // Set AwareLock's offset of the holding OS thread ID field into ThreadBlockingInfo's static field. That can be used
        // when doing managed debugging to get the OS ID of the thread holding the lock. The offset is currently not zero, and
        // zero is used in managed code to determine if the static variable has been initialized.
        _ASSERTE(AwareLock::GetOffsetOfHoldingOSThreadId() != 0);
        CoreLibBinder::GetField(FIELD__THREAD_BLOCKING_INFO__OFFSET_OF_LOCK_OWNER_OS_THREAD_ID)
            ->SetStaticValue32(AwareLock::GetOffsetOfHoldingOSThreadId());
    }
}

void SystemDomain::LazyInitGlobalStringLiteralMap()
{
    CONTRACTL
    {
        THROWS;
        GC_TRIGGERS;
        MODE_ANY;
        INJECT_FAULT(COMPlusThrowOM(););
    }
    CONTRACTL_END;

    // Allocate the global string literal map.
    NewHolder<GlobalStringLiteralMap> pGlobalStringLiteralMap(new GlobalStringLiteralMap());

    // Initialize the global string literal map.
    pGlobalStringLiteralMap->Init();

    if (InterlockedCompareExchangeT<GlobalStringLiteralMap *>(&m_pGlobalStringLiteralMap, pGlobalStringLiteralMap, NULL) == NULL)
    {
        pGlobalStringLiteralMap.SuppressRelease();
    }
}

void SystemDomain::LazyInitFrozenObjectsHeap()
{
    CONTRACTL
    {
        THROWS;
        GC_TRIGGERS;
        MODE_ANY;
        INJECT_FAULT(COMPlusThrowOM(););
    }
    CONTRACTL_END;

    NewHolder<FrozenObjectHeapManager> pFoh(new FrozenObjectHeapManager());
    if (InterlockedCompareExchangeT<FrozenObjectHeapManager*>(&m_FrozenObjectHeapManager, pFoh, nullptr) == nullptr)
    {
        pFoh.SuppressRelease();
    }
}

/*static*/ void SystemDomain::EnumAllStaticGCRefs(promote_func* fn, ScanContext* sc)
{
    CONTRACTL
    {
        NOTHROW;
        GC_NOTRIGGER;
        MODE_COOPERATIVE;
    }
    CONTRACTL_END;

    _ASSERTE(GCHeapUtilities::IsGCInProgress() &&
             GCHeapUtilities::IsServerHeap()   &&
             IsGCSpecialThread());

    AppDomain* pAppDomain = ::GetAppDomain();
    if (pAppDomain)
    {
        pAppDomain->EnumStaticGCRefs(fn, sc);
    }
}

extern "C" PCODE g_pGetGCStaticBase;
PCODE g_pGetGCStaticBase;
extern "C" PCODE g_pGetNonGCStaticBase;
PCODE g_pGetNonGCStaticBase;
extern "C" PCODE g_pPollGC;
PCODE g_pPollGC;
#if defined(TARGET_X86) && defined(TARGET_WINDOWS)
extern "C" PCODE g_pThrowOverflowException;
PCODE g_pThrowOverflowException;
extern "C" PCODE g_pThrowDivideByZeroException;
PCODE g_pThrowDivideByZeroException;
#endif // defined(TARGET_X86) && defined(TARGET_WINDOWS)

void SystemDomain::LoadBaseSystemClasses()
{
    STANDARD_VM_CONTRACT;

    ETWOnStartup(LdSysBases_V1, LdSysBasesEnd_V1);

    EX_TRY
    {
        m_pSystemPEAssembly = PEAssembly::OpenSystem();

        // Only partially load the system assembly. Other parts of the code will want to access
        // the globals in this function before finishing the load.
        m_pSystemAssembly = DefaultDomain()->LoadAssembly(NULL, m_pSystemPEAssembly, FILE_LOAD_BEFORE_TYPE_LOAD);

        // Set up binder for CoreLib
        CoreLibBinder::AttachModule(m_pSystemAssembly->GetModule());

        // Load Object
        g_pObjectClass = CoreLibBinder::GetClass(CLASS__OBJECT);

        // Now that ObjectClass is loaded, we can set up
        // the system for finalizers.  There is no point in deferring this, since we need
        // to know this before we allocate our first object.
        g_pObjectFinalizerMD = CoreLibBinder::GetMethod(METHOD__OBJECT__FINALIZE);


        g_pCanonMethodTableClass = CoreLibBinder::GetClass(CLASS____CANON);

        // NOTE: !!!IMPORTANT!!! ValueType and Enum MUST be loaded one immediately after
        //                       the other, because we have coded MethodTable::IsChildValueType
        //                       in such a way that it depends on this behaviour.
        // Load the ValueType class
        g_pValueTypeClass = CoreLibBinder::GetClass(CLASS__VALUE_TYPE);

        // Load the enum class
        g_pEnumClass = CoreLibBinder::GetClass(CLASS__ENUM);
        _ASSERTE(!g_pEnumClass->IsValueType());

        // Load System.RuntimeType
        g_pRuntimeTypeClass = CoreLibBinder::GetClass(CLASS__CLASS);
        _ASSERTE(g_pRuntimeTypeClass->IsFullyLoaded());

        // Load Array class
        g_pArrayClass = CoreLibBinder::GetClass(CLASS__ARRAY);

        // Calling a method on IList<T> for an array requires redirection to a method on
        // the SZArrayHelper class. Retrieving such methods means calling
        // GetActualImplementationForArrayGenericIListMethod, which calls FetchMethod for
        // the corresponding method on SZArrayHelper. This basically results in a class
        // load due to a method call, which the debugger cannot handle, so we pre-load
        // the SZArrayHelper class here.
        g_pSZArrayHelperClass = CoreLibBinder::GetClass(CLASS__SZARRAYHELPER);

        // Load Nullable class
        g_pNullableClass = CoreLibBinder::GetClass(CLASS__NULLABLE);

        // Load the Object array class.
        g_pPredefinedArrayTypes[ELEMENT_TYPE_OBJECT] = ClassLoader::LoadArrayTypeThrowing(TypeHandle(g_pObjectClass));

        // Boolean has to be loaded first to break cycle in IComparisonOperations and IEqualityOperators
        CoreLibBinder::LoadPrimitiveType(ELEMENT_TYPE_BOOLEAN);

        // Int32 has to be loaded next to break cycle in IShiftOperators
        CoreLibBinder::LoadPrimitiveType(ELEMENT_TYPE_I4);

        // Make sure all primitive types are loaded
        for (int et = ELEMENT_TYPE_VOID; et <= ELEMENT_TYPE_R8; et++)
            CoreLibBinder::LoadPrimitiveType((CorElementType)et);

        CoreLibBinder::LoadPrimitiveType(ELEMENT_TYPE_I);
        CoreLibBinder::LoadPrimitiveType(ELEMENT_TYPE_U);

        g_TypedReferenceMT = CoreLibBinder::GetClass(CLASS__TYPED_REFERENCE);

        // unfortunately, the following cannot be delay loaded since the jit
        // uses it to compute method attributes within a function that cannot
        // handle Complus exception and the following call goes through a path
        // where a complus exception can be thrown. It is unfortunate, because
        // we know that the delegate class and multidelegate class are always
        // guaranteed to be found.
        g_pDelegateClass = CoreLibBinder::GetClass(CLASS__DELEGATE);
        g_pMulticastDelegateClass = CoreLibBinder::GetClass(CLASS__MULTICAST_DELEGATE);

        // further loading of nonprimitive types may need casting support.
        // initialize cast cache here.
        CastCache::Initialize();

        // used by IsImplicitInterfaceOfSZArray
        CoreLibBinder::GetClass(CLASS__IENUMERABLEGENERIC);
        CoreLibBinder::GetClass(CLASS__ICOLLECTIONGENERIC);
        CoreLibBinder::GetClass(CLASS__ILISTGENERIC);
        CoreLibBinder::GetClass(CLASS__IREADONLYCOLLECTIONGENERIC);
        CoreLibBinder::GetClass(CLASS__IREADONLYLISTGENERIC);

        // Load String
        g_pStringClass = CoreLibBinder::LoadPrimitiveType(ELEMENT_TYPE_STRING);

        ECall::PopulateManagedStringConstructors();

        g_pExceptionClass = CoreLibBinder::GetClass(CLASS__EXCEPTION);
        g_pOutOfMemoryExceptionClass = CoreLibBinder::GetException(kOutOfMemoryException);
        g_pStackOverflowExceptionClass = CoreLibBinder::GetException(kStackOverflowException);
        g_pExecutionEngineExceptionClass = CoreLibBinder::GetException(kExecutionEngineException);
        g_pThreadAbortExceptionClass = CoreLibBinder::GetException(kThreadAbortException);

        g_pThreadClass = CoreLibBinder::GetClass(CLASS__THREAD);

        g_pWeakReferenceClass = CoreLibBinder::GetClass(CLASS__WEAKREFERENCE);
        g_pWeakReferenceOfTClass = CoreLibBinder::GetClass(CLASS__WEAKREFERENCEGENERIC);

        g_pCastHelpers = CoreLibBinder::GetClass(CLASS__CASTHELPERS);

        g_pIDynamicInterfaceCastableInterface = CoreLibBinder::GetClass(CLASS__IDYNAMICINTERFACECASTABLE);

    #ifdef FEATURE_COMINTEROP
        if (g_pConfig->IsBuiltInCOMSupported())
        {
            g_pBaseCOMObject = CoreLibBinder::GetClass(CLASS__COM_OBJECT);
        }
        else
        {
            g_pBaseCOMObject = NULL;
        }
    #endif

#ifdef FEATURE_EH_FUNCLETS
        g_pEHClass = CoreLibBinder::GetClass(CLASS__EH);
        g_pExceptionServicesInternalCallsClass = CoreLibBinder::GetClass(CLASS__EXCEPTIONSERVICES_INTERNALCALLS);
        g_pStackFrameIteratorClass = CoreLibBinder::GetClass(CLASS__STACKFRAMEITERATOR);
#endif

        g_pGetGCStaticBase = CoreLibBinder::GetMethod(METHOD__STATICSHELPERS__GET_GC_STATIC)->GetMultiCallableAddrOfCode();
        g_pGetNonGCStaticBase = CoreLibBinder::GetMethod(METHOD__STATICSHELPERS__GET_NONGC_STATIC)->GetMultiCallableAddrOfCode();
        g_pPollGC = CoreLibBinder::GetMethod(METHOD__THREAD__POLLGC)->GetMultiCallableAddrOfCode();
#if defined(TARGET_X86) && defined(TARGET_WINDOWS)
        g_pThrowOverflowException = CoreLibBinder::GetMethod(METHOD__THROWHELPERS__THROWOVERFLOWEXCEPTION)->GetMultiCallableAddrOfCode();
        g_pThrowDivideByZeroException = CoreLibBinder::GetMethod(METHOD__THROWHELPERS__THROWDIVIDEBYZEROEXCEPTION)->GetMultiCallableAddrOfCode();
#endif // TARGET_32BIT

    #ifdef PROFILING_SUPPORTED
        // Note that g_profControlBlock.fBaseSystemClassesLoaded must be set to TRUE only after
        // all base system classes are loaded.  Profilers are not allowed to call any type-loading
        // APIs until g_profControlBlock.fBaseSystemClassesLoaded is TRUE.  It is important that
        // all base system classes need to be loaded before profilers can trigger the type loading.
        g_profControlBlock.fBaseSystemClassesLoaded = TRUE;
    #endif // PROFILING_SUPPORTED

    #if defined(_DEBUG)
        g_CoreLib.Check();
        g_CoreLib.CheckExtended();
    #endif // _DEBUG
    }
    EX_HOOK
    {
        Exception *ex = GET_EXCEPTION();

        LogErrorToHost("Failed to load System.Private.CoreLib.dll (error code 0x%08X)", ex->GetHR());
        MAKE_UTF8PTR_FROMWIDE_NOTHROW(filePathUtf8, SystemDomain::System()->BaseLibrary())
        if (filePathUtf8 != NULL)
        {
            LogErrorToHost("Path: %s", filePathUtf8);
        }
        SString err;
        ex->GetMessage(err);
        LogErrorToHost("Error message: %s", err.GetUTF8());
    }
    EX_END_HOOK;
}

#endif // !DACCESS_COMPILE

#ifndef DACCESS_COMPILE

#if defined(FEATURE_COMINTEROP_APARTMENT_SUPPORT)

Thread::ApartmentState SystemDomain::GetEntryPointThreadAptState(IMDInternalImport* pScope, mdMethodDef mdMethod)
{
    STANDARD_VM_CONTRACT;

    HRESULT hr;
    IfFailThrow(hr = pScope->GetCustomAttributeByName(mdMethod,
                                                      DEFAULTDOMAIN_MTA_TYPE,
                                                      NULL,
                                                      NULL));
    BOOL fIsMTA = FALSE;
    if(hr == S_OK)
        fIsMTA = TRUE;

    IfFailThrow(hr = pScope->GetCustomAttributeByName(mdMethod,
                                                      DEFAULTDOMAIN_STA_TYPE,
                                                      NULL,
                                                      NULL));
    BOOL fIsSTA = FALSE;
    if (hr == S_OK)
        fIsSTA = TRUE;

    if (fIsSTA && fIsMTA)
        COMPlusThrowHR(COR_E_CUSTOMATTRIBUTEFORMAT);

    if (fIsSTA)
        return Thread::AS_InSTA;
    else if (fIsMTA)
        return Thread::AS_InMTA;

    return Thread::AS_Unknown;
}

void SystemDomain::SetThreadAptState (Thread::ApartmentState state)
{
    STANDARD_VM_CONTRACT;

    Thread* pThread = GetThread();
    if(state == Thread::AS_InSTA)
    {
        Thread::ApartmentState pState = pThread->SetApartment(Thread::AS_InSTA);
        _ASSERTE(pState == Thread::AS_InSTA);
    }
    else
    {
        // If an apartment state was not explicitly requested, default to MTA
        Thread::ApartmentState pState = pThread->SetApartment(Thread::AS_InMTA);
        _ASSERTE(pState == Thread::AS_InMTA);
    }
}
#endif // defined(FEATURE_COMINTEROP_APARTMENT_SUPPORT)

/*static*/
bool SystemDomain::IsReflectionInvocationMethod(MethodDesc* pMeth)
{
    CONTRACTL
    {
        THROWS;
        GC_TRIGGERS;
        MODE_ANY;
    }
    CONTRACTL_END;

    MethodTable* pCaller = pMeth->GetMethodTable();

    // All reflection invocation methods are defined in CoreLib.
    if (!pCaller->GetModule()->IsSystem())
        return false;

    // Check for dynamically generated Invoke methods.
    if (pMeth->IsLCGMethod())
    {
        // Even if a user-created DynamicMethod uses the same naming convention, it will likely not
        // get here since since DynamicMethods by default are created in a special (non-system) module.
        // If this is not sufficient for conflict prevention, we can create a new private module.
        return (strncmp(pMeth->GetName(), "InvokeStub_", ARRAY_SIZE("InvokeStub_") - 1) == 0);
    }

    /* List of types that should be skipped to identify true caller */
    static const BinderClassID reflectionInvocationTypes[] = {
        CLASS__METHOD,
        CLASS__METHOD_BASE,
        CLASS__METHOD_INFO,
        CLASS__CONSTRUCTOR,
        CLASS__CONSTRUCTOR_INFO,
        CLASS__CLASS,
        CLASS__TYPE_HANDLE,
        CLASS__METHOD_HANDLE,
        CLASS__FIELD_HANDLE,
        CLASS__TYPE,
        CLASS__FIELD,
        CLASS__RT_FIELD_INFO,
        CLASS__FIELD_INFO,
        CLASS__EVENT,
        CLASS__EVENT_INFO,
        CLASS__PROPERTY,
        CLASS__PROPERTY_INFO,
        CLASS__ACTIVATOR,
        CLASS__ARRAY,
        CLASS__ASSEMBLYBASE,
        CLASS__ASSEMBLY,
        CLASS__TYPE_DELEGATOR,
        CLASS__RUNTIME_HELPERS,
        CLASS__DYNAMICMETHOD,
        CLASS__DELEGATE,
        CLASS__MULTICAST_DELEGATE,
        CLASS__METHODBASEINVOKER,
        CLASS__INITHELPERS,
        CLASS__STATICSHELPERS,
    };

    static bool fInited = false;

    if (!VolatileLoad(&fInited))
    {
        // Make sure all types are loaded so that we can use faster GetExistingClass()
        for (unsigned i = 0; i < ARRAY_SIZE(reflectionInvocationTypes); i++)
        {
            CoreLibBinder::GetClass(reflectionInvocationTypes[i]);
        }

        VolatileStore(&fInited, true);
    }

    if (!pCaller->HasInstantiation())
    {
        for (unsigned i = 0; i < ARRAY_SIZE(reflectionInvocationTypes); i++)
        {
            if (CoreLibBinder::GetExistingClass(reflectionInvocationTypes[i]) == pCaller)
                return true;
        }
    }

    return false;
}

struct CallersDataWithStackMark
{
    StackCrawlMark* stackMark;
    BOOL foundMe;
    MethodDesc* pFoundMethod;
    MethodDesc* pPrevMethod;
};

/*static*/
Module* SystemDomain::GetCallersModule(StackCrawlMark* stackMark)

{
    CONTRACTL
    {
        THROWS;
        GC_TRIGGERS;
        MODE_ANY;
        INJECT_FAULT(COMPlusThrowOM(););
    }
    CONTRACTL_END;

    if (stackMark == NULL)
        return NULL;

    GCX_COOP();

    CallersDataWithStackMark cdata;
    ZeroMemory(&cdata, sizeof(CallersDataWithStackMark));
    cdata.stackMark = stackMark;

    GetThread()->StackWalkFrames(CallersMethodCallbackWithStackMark, &cdata, FUNCTIONSONLY | LIGHTUNWIND);

    if(cdata.pFoundMethod) {
        return cdata.pFoundMethod->GetModule();
    } else
        return NULL;
}

struct CallersData
{
    int skip;
    MethodDesc* pMethod;
};

/*static*/
Assembly* SystemDomain::GetCallersAssembly(StackCrawlMark *stackMark)
{
    WRAPPER_NO_CONTRACT;
    Module* mod = GetCallersModule(stackMark);
    if (mod)
        return mod->GetAssembly();
    return NULL;
}

/*private static*/
StackWalkAction SystemDomain::CallersMethodCallbackWithStackMark(CrawlFrame* pCf, VOID* data)
{
    CONTRACTL
    {
        THROWS;
        GC_TRIGGERS;
        MODE_COOPERATIVE;
        INJECT_FAULT(COMPlusThrowOM(););
    }
    CONTRACTL_END;


    MethodDesc *pFunc = pCf->GetFunction();

    /* We asked to be called back only for functions */
    _ASSERTE(pFunc);

    CallersDataWithStackMark* pCaller = (CallersDataWithStackMark*) data;
    if (pCaller->stackMark)
    {
        if (!pCf->IsInCalleesFrames(pCaller->stackMark))
        {
            // save the current in case it is the one we want
            pCaller->pPrevMethod = pFunc;
            return SWA_CONTINUE;
        }

        // LookForMe stack crawl marks needn't worry about reflection or
        // remoting frames on the stack. Each frame above (newer than) the
        // target will be captured by the logic above. Once we transition to
        // finding the stack mark below the AofRA, we know that we hit the
        // target last time round and immediately exit with the cached result.

        if (*(pCaller->stackMark) == LookForMe)
        {
            pCaller->pFoundMethod = pCaller->pPrevMethod;
            return SWA_ABORT;
        }
    }

    // Skip reflection and remoting frames that could lie between a stack marked
    // method and its true caller (or that caller and its own caller). These
    // frames are infrastructure and logically transparent to the stack crawling
    // algorithm.

    // Skipping remoting frames. We always skip entire client to server spans
    // (though we see them in the order server then client during a stack crawl
    // obviously).

    // We spot the server dispatcher end because all calls are dispatched
    // through a single method: StackBuilderSink._PrivateProcessMessage.

    Frame* frame = pCf->GetFrame();
    _ASSERTE(pCf->IsFrameless() || frame);

    // Skipping reflection frames. We don't need to be quite as exhaustive here
    // as the security or reflection stack walking code since we know this logic
    // is only invoked for selected methods in CoreLib itself. So we're
    // reasonably sure we won't have any sensitive methods late bound invoked on
    // constructors, properties or events. This leaves being invoked via
    // MethodInfo, Type or Delegate (and depending on which invoke overload is
    // being used, several different reflection classes may be involved).

    if (SystemDomain::IsReflectionInvocationMethod(pFunc))
        return SWA_CONTINUE;

    // Return the first non-reflection/remoting frame if no stack mark was
    // supplied.
    if (!pCaller->stackMark)
    {
        pCaller->pFoundMethod = pFunc;
        return SWA_ABORT;
    }

    // If we got here, we must already be in the frame containing the stack mark and we are not looking for "me".
    _ASSERTE(pCaller->stackMark &&
             pCf->IsInCalleesFrames(pCaller->stackMark) &&
             *(pCaller->stackMark) != LookForMe);

    // When looking for caller's caller, we delay returning results for another
    // round (the way this is structured, we will still be able to skip
    // reflection and remoting frames between the caller and the caller's
    // caller).

    if ((*(pCaller->stackMark) == LookForMyCallersCaller) &&
        (pCaller->pFoundMethod == NULL))
    {
        pCaller->pFoundMethod = pFunc;
        return SWA_CONTINUE;
    }

    pCaller->pFoundMethod = pFunc;

    return SWA_ABORT;
}

/*private static*/
StackWalkAction SystemDomain::CallersMethodCallback(CrawlFrame* pCf, VOID* data)
{
    LIMITED_METHOD_CONTRACT;
    MethodDesc *pFunc = pCf->GetFunction();

    /* We asked to be called back only for functions */
    _ASSERTE(pFunc);

    CallersData* pCaller = (CallersData*) data;
    if(pCaller->skip == 0) {
        pCaller->pMethod = pFunc;
        return SWA_ABORT;
    }
    else {
        pCaller->skip--;
        return SWA_CONTINUE;
    }
}

void AppDomain::Create()
{
    STANDARD_VM_CONTRACT;

    _ASSERTE(m_pTheAppDomain == NULL);

    NewHolder<AppDomain> pDomain(new AppDomain());
    pDomain->Init();
    pDomain->CreateDefaultBinder();

    m_pTheAppDomain = pDomain.Extract();

    LOG((LF_CLASSLOADER | LF_CORDB,
         LL_INFO10,
         "Created the app domain at %p\n", m_pTheAppDomain));
}

#ifdef DEBUGGING_SUPPORTED

void SystemDomain::PublishAppDomainAndInformDebugger (AppDomain *pDomain)
{
    CONTRACTL
    {
        if(!g_fEEInit) {THROWS;} else {DISABLED(NOTHROW);};
        if(!g_fEEInit) {GC_TRIGGERS;} else {DISABLED(GC_NOTRIGGER);};
        MODE_ANY;
    }
    CONTRACTL_END;

    LOG((LF_CORDB, LL_INFO100, "SD::PADAID: Adding 0x%x\n", pDomain));

    //
    // We need to synchronize this routine with the attach logic.  The "normal"
    // attach case uses the HelperThread and TrapAllRuntimeThreads to synchronize
    // the runtime before sending any of the events (including AppDomainCreates)
    // to the right-side.  Thus, we can synchronize with this case by forcing us
    // to go co-operative.  If we were already co-op, then the helper thread will
    // wait to start the attach until all co-op threads are paused.  If we were
    // pre-emptive, then going co-op will suspend us until the HelperThread finishes.
    //
    // The second case is under the IPC event for ATTACHING, which is where there are
    // zero app domains, so it is considered an 'early attach' case.  To synchronize
    // with this we have to grab and hold the AppDomainDB lock.
    //


    // Send event to debugger if one is attached.
    if (CORDebuggerAttached())
    {
        GCX_COOP();
        g_pDebugInterface->AppDomainCreated(pDomain);
    }
}

#endif // DEBUGGING_SUPPORTED

#ifdef PROFILING_SUPPORTED
void SystemDomain::NotifyProfilerStartup()
{
    CONTRACTL
    {
        NOTHROW;
        GC_TRIGGERS;
        MODE_PREEMPTIVE;
    }
    CONTRACTL_END;

    {
        BEGIN_PROFILER_CALLBACK(CORProfilerTrackAppDomainLoads());
        _ASSERTE(System());
        (&g_profControlBlock)->AppDomainCreationStarted((AppDomainID) System());
        END_PROFILER_CALLBACK();
    }

    {
        BEGIN_PROFILER_CALLBACK(CORProfilerTrackAppDomainLoads());
        _ASSERTE(System());
        (&g_profControlBlock)->AppDomainCreationFinished((AppDomainID) System(), S_OK);
        END_PROFILER_CALLBACK();
    }

    {
        BEGIN_PROFILER_CALLBACK(CORProfilerTrackAppDomainLoads());
        _ASSERTE(AppDomain::GetCurrentDomain());
        (&g_profControlBlock)->AppDomainCreationStarted((AppDomainID) AppDomain::GetCurrentDomain());
        END_PROFILER_CALLBACK();
    }

    {
        BEGIN_PROFILER_CALLBACK(CORProfilerTrackAppDomainLoads());
        _ASSERTE(AppDomain::GetCurrentDomain());
        (&g_profControlBlock)->AppDomainCreationFinished((AppDomainID) AppDomain::GetCurrentDomain(), S_OK);
        END_PROFILER_CALLBACK();
    }
}

HRESULT SystemDomain::NotifyProfilerShutdown()
{
    CONTRACTL
    {
        NOTHROW;
        GC_TRIGGERS;
        MODE_PREEMPTIVE;
    }
    CONTRACTL_END;

    {
        BEGIN_PROFILER_CALLBACK(CORProfilerTrackAppDomainLoads());
        _ASSERTE(System());
        (&g_profControlBlock)->AppDomainShutdownStarted((AppDomainID) System());
        END_PROFILER_CALLBACK();
    }

    {
        BEGIN_PROFILER_CALLBACK(CORProfilerTrackAppDomainLoads());
        _ASSERTE(System());
        (&g_profControlBlock)->AppDomainShutdownFinished((AppDomainID) System(), S_OK);
        END_PROFILER_CALLBACK();
    }

    {
        BEGIN_PROFILER_CALLBACK(CORProfilerTrackAppDomainLoads());
        _ASSERTE(AppDomain::GetCurrentDomain());
        (&g_profControlBlock)->AppDomainShutdownStarted((AppDomainID) AppDomain::GetCurrentDomain());
        END_PROFILER_CALLBACK();
    }

    {
        BEGIN_PROFILER_CALLBACK(CORProfilerTrackAppDomainLoads());
        _ASSERTE(AppDomain::GetCurrentDomain());
        (&g_profControlBlock)->AppDomainShutdownFinished((AppDomainID) AppDomain::GetCurrentDomain(), S_OK);
        END_PROFILER_CALLBACK();
    }
    return (S_OK);
}
#endif // PROFILING_SUPPORTED

AppDomain::AppDomain()
    : m_handleStore{NULL}
    , m_pPinnedHeapHandleTable{NULL}
    , m_pDefaultBinder{NULL}
    , m_pRefClassFactHash{NULL}
#ifdef FEATURE_COMINTEROP
    , m_pRefDispIDCache{NULL}
    , m_hndMissing{NULL}
#endif //FEATURE_COMINTEROP
    , m_pDelayedLoaderAllocatorUnloadList{NULL}
    , m_friendlyName{NULL}
    , m_pRootAssembly{NULL}
#ifdef FEATURE_COMINTEROP
    , m_pRCWCache{NULL}
#endif //FEATURE_COMINTEROP
#ifdef FEATURE_COMWRAPPERS
    , m_pRCWRefCache{NULL}
#endif // FEATURE_COMWRAPPERS
    , m_ForceTrivialWaitOperations{false}
#ifdef FEATURE_TYPEEQUIVALENCE
    , m_pTypeEquivalenceTable{NULL}
#endif // FEATURE_TYPEEQUIVALENCE
{
    // initialize fields so the appdomain can be safely destructed
    // shouldn't call anything that can fail here - use ::Init instead
    CONTRACTL
    {
        THROWS;
        GC_TRIGGERS;
        MODE_ANY;
        FORBID_FAULT;
    }
    CONTRACTL_END;

    m_JITLock.PreInit();
    m_ClassInitLock.PreInit();
    m_ILStubGenLock.PreInit();
    m_NativeTypeLoadLock.PreInit();
    m_FileLoadLock.PreInit();

#ifdef _DEBUG
    m_Assemblies.Debug_SetAppDomain(this);
#endif // _DEBUG

} // AppDomain::AppDomain

AppDomain::~AppDomain()
{
    CONTRACTL
    {
        NOTHROW;
        GC_TRIGGERS;
        MODE_ANY;
    }
    CONTRACTL_END;

    m_AssemblyCache.Clear();
}

//*****************************************************************************
//*****************************************************************************
//*****************************************************************************
void AppDomain::Init()
{
    STANDARD_VM_CONTRACT;

    //
    //   The JIT lock and the CCtor locks are at the same level (and marked as
    //   UNSAFE_SAME_LEVEL) because they are all part of the same deadlock detection mechanism. We
    //   see through cycles of JITting and .cctor execution and then explicitly allow the cycle to
    //   be broken by giving access to uninitialized classes.  If there is no cycle or if the cycle
    //   involves other locks that arent part of this special deadlock-breaking semantics, then
    //   we continue to block.
    //
    m_JITLock.Init(CrstJit, CrstFlags(CRST_REENTRANCY | CRST_UNSAFE_SAMELEVEL));
    m_ClassInitLock.Init(CrstClassInit, CrstFlags(CRST_REENTRANCY | CRST_UNSAFE_SAMELEVEL));
    m_ILStubGenLock.Init(CrstILStubGen, CrstFlags(CRST_REENTRANCY));
    m_NativeTypeLoadLock.Init(CrstInteropData, CrstFlags(CRST_REENTRANCY));
    m_crstGenericDictionaryExpansionLock.Init(CrstGenericDictionaryExpansion);
    m_FileLoadLock.Init(CrstAssemblyLoader, CrstFlags(CRST_DEFAULT));
    m_DomainCacheCrst.Init(CrstAppDomainCache);

    // Has to switch thread to GC_NOTRIGGER while being held
    m_crstAssemblyList.Init(CrstAssemblyList, CrstFlags(
        CRST_GC_NOTRIGGER_WHEN_TAKEN | CRST_DEBUGGER_THREAD | CRST_TAKEN_DURING_SHUTDOWN));

    m_crstLoaderAllocatorReferences.Init(CrstLoaderAllocatorReferences);
    m_MethodTableExposedClassObjectCrst.Init(CrstMethodTableExposedObject);

    // Set up the binding caches
    m_AssemblyCache.Init(&m_DomainCacheCrst, GetHighFrequencyHeap());

    m_handleStore = GCHandleUtilities::GetGCHandleManager()->GetGlobalHandleStore();
    if (!m_handleStore)
    {
        COMPlusThrowOM();
    }

#ifdef FEATURE_TYPEEQUIVALENCE
    m_TypeEquivalenceCrst.Init(CrstTypeEquivalenceMap);
#endif

    m_ReflectionCrst.Init(CrstReflection, CRST_UNSAFE_ANYMODE);
    m_RefClassFactCrst.Init(CrstClassFactInfoHash);

#ifdef FEATURE_TIERED_COMPILATION
    m_tieredCompilationManager.Init();
#endif

    m_nativeImageLoadCrst.Init(CrstNativeImageLoad);

    m_typeIDMap.Init();
    GetLoaderAllocator()->InitVirtualCallStubManager();
} // AppDomain::Init

void AppDomain::Stop()
{
    CONTRACTL
    {
        NOTHROW;
        MODE_ANY;
        GC_TRIGGERS;
    }
    CONTRACTL_END;

#ifdef FEATURE_MULTICOREJIT
    GetMulticoreJitManager().StopProfile(true);
#endif

    // Set the unloaded flag before notifying the debugger
    GetLoaderAllocator()->SetIsUnloaded();

#ifdef DEBUGGING_SUPPORTED
    if (IsDebuggerAttached())
        NotifyDebuggerUnload();
#endif // DEBUGGING_SUPPORTED
}


#endif //!DACCESS_COMPILE

#ifndef DACCESS_COMPILE

void AppDomain::AddAssembly(DomainAssembly * assem)
{
    CONTRACTL
    {
        THROWS;
        GC_TRIGGERS;
        MODE_ANY;
        INJECT_FAULT(COMPlusThrowOM(););
    }
    CONTRACTL_END;

    {
        CrstHolder ch(GetAssemblyListLock());

        // Attempt to find empty space in assemblies list
        DWORD asmCount = m_Assemblies.GetCount_Unlocked();
        for (DWORD i = 0; i < asmCount; ++i)
        {
            if (m_Assemblies.Get_UnlockedNoReference(i) == NULL)
            {
                m_Assemblies.Set_Unlocked(i, assem);
                return;
            }
        }

        // If empty space not found, simply add to end of list
        IfFailThrow(m_Assemblies.Append_Unlocked(assem));
    }
}

void AppDomain::RemoveAssembly(DomainAssembly * pAsm)
{
    CONTRACTL
    {
        NOTHROW;
        GC_NOTRIGGER;
    }
    CONTRACTL_END;

    CrstHolder ch(GetAssemblyListLock());
    DWORD asmCount = m_Assemblies.GetCount_Unlocked();
    for (DWORD i = 0; i < asmCount; ++i)
    {
        if (m_Assemblies.Get_UnlockedNoReference(i) == pAsm)
        {
            m_Assemblies.Set_Unlocked(i, NULL);
            return;
        }
    }

    _ASSERTE(!"Unreachable");
}

BOOL AppDomain::ContainsAssembly(Assembly * assem)
{
    WRAPPER_NO_CONTRACT;
    AssemblyIterator i = IterateAssembliesEx((AssemblyIterationFlags)(
        kIncludeLoaded | kIncludeExecution));
    CollectibleAssemblyHolder<Assembly *> pAssembly;

    while (i.Next(pAssembly.This()))
    {
        if (pAssembly == assem)
            return TRUE;
    }

    return FALSE;
}

EEClassFactoryInfoHashTable* AppDomain::SetupClassFactHash()
{
    CONTRACTL
    {
        THROWS;
        GC_TRIGGERS;
        MODE_ANY;
        INJECT_FAULT(COMPlusThrowOM(););
    }
    CONTRACTL_END;

    CrstHolder ch(&m_ReflectionCrst);

    if (m_pRefClassFactHash == NULL)
    {
        AllocMemHolder<void> pCache(GetLowFrequencyHeap()->AllocMem(S_SIZE_T(sizeof (EEClassFactoryInfoHashTable))));
        EEClassFactoryInfoHashTable *tmp = new (pCache) EEClassFactoryInfoHashTable;
        LockOwner lock = {&m_RefClassFactCrst,IsOwnerOfCrst};
        if (!tmp->Init(20, &lock))
            COMPlusThrowOM();
        pCache.SuppressRelease();
        m_pRefClassFactHash = tmp;
    }

    return m_pRefClassFactHash;
}

#ifdef FEATURE_COMINTEROP
DispIDCache* AppDomain::SetupRefDispIDCache()
{
    CONTRACTL
    {
        THROWS;
        GC_TRIGGERS;
        MODE_ANY;
        INJECT_FAULT(COMPlusThrowOM(););
    }
    CONTRACTL_END;

    CrstHolder ch(&m_ReflectionCrst);

    if (m_pRefDispIDCache == NULL)
    {
        AllocMemHolder<void> pCache = GetLowFrequencyHeap()->AllocMem(S_SIZE_T(sizeof (DispIDCache)));

        DispIDCache *tmp = new (pCache) DispIDCache;
        tmp->Init();

        pCache.SuppressRelease();
        m_pRefDispIDCache = tmp;
    }

    return m_pRefDispIDCache;
}

#endif // FEATURE_COMINTEROP

FileLoadLock *FileLoadLock::Create(PEFileListLock *pLock, PEAssembly * pPEAssembly, Assembly *pAssembly)
{
    CONTRACTL
    {
        THROWS;
        GC_TRIGGERS;
        MODE_ANY;
        PRECONDITION(pLock->HasLock());
        PRECONDITION(pLock->FindFileLock(pPEAssembly) == NULL);
        INJECT_FAULT(COMPlusThrowOM(););
    }
    CONTRACTL_END;

    NewHolder<FileLoadLock> result(new FileLoadLock(pLock, pPEAssembly, pAssembly));

    pLock->AddElement(result);
    result->AddRef(); // Add one ref on behalf of the ListLock's reference. The corresponding Release() happens in FileLoadLock::CompleteLoadLevel.
    return result.Extract();
}

FileLoadLock::~FileLoadLock()
{
    CONTRACTL
    {
        DESTRUCTOR_CHECK;
        NOTHROW;
        GC_TRIGGERS;
        MODE_ANY;
    }
    CONTRACTL_END;
    ((PEAssembly *) m_data)->Release();
}

Assembly *FileLoadLock::GetAssembly()
{
    LIMITED_METHOD_CONTRACT;
    return m_pAssembly;
}

FileLoadLevel FileLoadLock::GetLoadLevel()
{
    LIMITED_METHOD_CONTRACT;
    return m_level;
}

// Acquire will return FALSE and not take the lock if the file
// has already been loaded to the target level.  Otherwise,
// it will return TRUE and take the lock.
//
// Note that the taker must release the lock via IncrementLoadLevel.

BOOL FileLoadLock::Acquire(FileLoadLevel targetLevel)
{
    WRAPPER_NO_CONTRACT;

    // If we are already loaded to the desired level, the lock is "free".
    if (m_level >= targetLevel)
        return FALSE;

    if (!DeadlockAwareEnter())
    {
        // We failed to get the lock due to a deadlock.
        return FALSE;
    }

    if (m_level >= targetLevel)
    {
        Leave();
        return FALSE;
    }

    return TRUE;
}

BOOL FileLoadLock::CanAcquire(FileLoadLevel targetLevel)
{
    // If we are already loaded to the desired level, the lock is "free".
    if (m_level >= targetLevel)
        return FALSE;

    return CanDeadlockAwareEnter();
}

#if !defined(DACCESS_COMPILE) && (defined(LOGGING) || defined(STRESS_LOG))
static const char *fileLoadLevelName[] =
{
    "CREATE",                             // FILE_LOAD_CREATE
    "BEGIN",                              // FILE_LOAD_BEGIN
    "BEFORE_TYPE_LOAD",                   // FILE_LOAD_BEFORE_TYPE_LOAD
    "EAGER_FIXUPS",                       // FILE_LOAD_EAGER_FIXUPS
    "DELIVER_EVENTS",                     // FILE_LOAD_DELIVER_EVENTS
    "VTABLE FIXUPS",                      // FILE_LOAD_VTABLE_FIXUPS
    "LOADED",                             // FILE_LOADED
    "ACTIVE",                             // FILE_ACTIVE
};
#endif // !DACCESS_COMPILE && (LOGGING || STRESS_LOG)

BOOL FileLoadLock::CompleteLoadLevel(FileLoadLevel level, BOOL success)
{
    CONTRACTL
    {
        MODE_ANY;
        GC_TRIGGERS;
        THROWS;
        PRECONDITION(HasLock());
    }
    CONTRACTL_END;

    // Increment may happen more than once if reentrancy occurs (e.g. LoadLibrary)
    if (level > m_level)
    {
        // Must complete each level in turn, unless we have an error
        CONSISTENCY_CHECK(m_pAssembly->IsError() || (level == (m_level+1)));
        // Remove the lock from the list if the load is completed
        if (level >= FILE_ACTIVE)
        {
            {
                GCX_COOP();
                PEFileListLockHolder lock((PEFileListLock*)m_pList);

#if _DEBUG
                BOOL fDbgOnly_SuccessfulUnlink =
#endif
                    m_pList->Unlink(this);
                _ASSERTE(fDbgOnly_SuccessfulUnlink);

                m_pAssembly->ClearLoading();

                CONSISTENCY_CHECK(m_dwRefCount >= 2); // Caller (LoadAssembly) should have 1 refcount and m_pList should have another which was acquired in FileLoadLock::Create.

                m_level = (FileLoadLevel)level;

                // Dev11 bug 236344
                // In AppDomain::IsLoading, if the lock is taken on m_pList and then FindFileLock returns NULL,
                // we depend on the DomainAssembly's load level being up to date. Hence we must update the load
                // level while the m_pList lock is held.
                if (success)
                    m_pAssembly->SetLoadLevel(level);
            }


            Release(); // Release m_pList's refcount on this lock, which was acquired in FileLoadLock::Create

        }
        else
        {
            m_level = (FileLoadLevel)level;

            if (success)
                m_pAssembly->SetLoadLevel(level);
        }

#ifndef DACCESS_COMPILE
        switch(level)
        {
            case FILE_LOAD_DELIVER_EVENTS:
            case FILE_LOADED:
            case FILE_ACTIVE: // The timing of stress logs is not critical, so even for the FILE_ACTIVE stage we need not do it while the m_pList lock is held.
                STRESS_LOG3(LF_CLASSLOADER, LL_INFO100, "Completed Load Level %s for Assembly %p - success = %i\n", fileLoadLevelName[level], m_pAssembly, success);
                break;
            default:
                break;
        }
#endif

        return TRUE;
    }
    else
        return FALSE;
}

void FileLoadLock::SetError(Exception *ex)
{
    CONTRACTL
    {
        MODE_ANY;
        GC_TRIGGERS;
        THROWS;
        PRECONDITION(CheckPointer(ex));
        PRECONDITION(HasLock());
        INJECT_FAULT(COMPlusThrowOM(););
    }
    CONTRACTL_END;

    m_cachedHR = ex->GetHR();

    LOG((LF_LOADER, LL_WARNING, "LOADER: ***%s*\t!!!Non-transient error 0x%x\n",
        m_pAssembly->GetSimpleName(), m_cachedHR));

    m_pAssembly->SetError(ex);

    CompleteLoadLevel(FILE_ACTIVE, FALSE);
}

void FileLoadLock::AddRef()
{
    LIMITED_METHOD_CONTRACT;
    InterlockedIncrement((LONG *) &m_dwRefCount);
}

UINT32 FileLoadLock::Release()
{
    CONTRACTL
    {
        NOTHROW;
        GC_TRIGGERS;
        MODE_ANY;
    }
    CONTRACTL_END;

    LONG count = InterlockedDecrement((LONG *) &m_dwRefCount);
    if (count == 0)
        delete this;

    return count;
}

FileLoadLock::FileLoadLock(PEFileListLock *pLock, PEAssembly * pPEAssembly, Assembly *pAssembly)
  : ListLockEntry(pLock, pPEAssembly, "File load lock"),
    m_level((FileLoadLevel) (FILE_LOAD_CREATE)),
    m_pAssembly(pAssembly),
    m_cachedHR(S_OK)
{
    WRAPPER_NO_CONTRACT;
    pPEAssembly->AddRef();
}

void FileLoadLock::HolderLeave(FileLoadLock *pThis)
{
    LIMITED_METHOD_CONTRACT;
    pThis->Leave();
}


//
// Assembly loading:
//
// Assembly loading is carefully layered to avoid deadlocks in the
// presence of circular loading dependencies.
// A LoadLevel is associated with each assembly as it is being loaded.  During the
// act of loading (abstractly, increasing its load level), its lock is
// held, and the current load level is stored on the thread.  Any
// recursive loads during that period are automatically restricted to
// only partially load the dependent assembly to the same level as the
// caller (or to one short of that level in the presence of a deadlock
// loop.)
//
// Each loading stage must be carfully constructed so that
// this constraint is expected and can be dealt with.
//
// Note that there is one case where this still doesn't handle recursion, and that is the
// security subsystem. The security system runs managed code, and thus must typically fully
// initialize assemblies of permission sets it is trying to use. (And of course, these may be used
// while those assemblies are initializing.)  This is dealt with in the historical manner - namely
// the security system passes in a special flag which says that it will deal with null return values
// in the case where a load cannot be safely completed due to such issues.
//

void AppDomain::LoadSystemAssemblies()
{
    STANDARD_VM_CONTRACT;

    // The only reason to make an assembly a "system assembly" is if the EE is caching
    // pointers to stuff in the assembly.  Because this is going on, we need to preserve
    // the invariant that the assembly is loaded into every app domain.
    //
    // Right now we have only one system assembly. We shouldn't need to add any more.

    LoadAssembly(NULL, SystemDomain::System()->SystemPEAssembly(), FILE_ACTIVE);
}

// This checks if the thread has initiated (or completed) loading at the given level.  A false guarantees that
// (a) The current thread (or a thread blocking on the current thread) has not started loading the file
//      at the given level, and
// (b) No other thread had started loading the file at this level at the start of this function call.

// Note that another thread may start loading the file at that level in a race with the completion of
// this function.  However, the caller still has the guarantee that such a load started after this
// function was called (and e.g. any state in place before the function call will be seen by the other thread.)
//
// Conversely, a true guarantees that either the current thread has started the load step, or another
// thread has completed the load step.
//

BOOL AppDomain::IsLoading(Assembly *pAssembly, FileLoadLevel level)
{
    // Cheap out
    if (pAssembly->GetLoadLevel() < level)
    {
        FileLoadLock *pLock = NULL;
        {
            LoadLockHolder lock(this);

            pLock = (FileLoadLock *) lock->FindFileLock(pAssembly->GetPEAssembly());

            if (pLock == NULL)
            {
                // No thread involved with loading
                return pAssembly->GetLoadLevel() >= level;
            }

            pLock->AddRef();
        }

        FileLoadLockRefHolder lockRef(pLock);

        if (pLock->Acquire(level))
        {
            // We got the lock - therefore no other thread has started this loading step yet.
            pLock->Leave();
            return FALSE;
        }

        // We didn't get the lock - either this thread is already doing the load,
        // or else the load has already finished.
    }
    return TRUE;
}

// CheckLoading is a weaker form of IsLoading, which will not block on
// other threads waiting for their status.  This is appropriate for asserts.
CHECK AppDomain::CheckLoading(Assembly *pAssembly, FileLoadLevel level)
{
    // Cheap out
    if (pAssembly->GetLoadLevel() < level)
    {
        FileLoadLock *pLock = NULL;

        LoadLockHolder lock(this);

        pLock = (FileLoadLock *) lock->FindFileLock(pAssembly->GetPEAssembly());

        if (pLock != NULL
            && pLock->CanAcquire(level))
        {
            // We can get the lock - therefore no other thread has started this loading step yet.
            CHECK_FAILF(("Loading step %d has not been initiated yet", level));
        }

        // We didn't get the lock - either this thread is already doing the load,
        // or else the load has already finished.
    }

    CHECK_OK;
}

CHECK AppDomain::CheckCanLoadTypes(Assembly *pAssembly)
{
    CONTRACTL
    {
        THROWS;
        GC_TRIGGERS;
        MODE_ANY;
    }
    CONTRACTL_END;
    CHECK_MSG(CheckValidModule(pAssembly->GetModule()),
              "Type loading can occur only when executing in the assembly's app domain");
    CHECK_OK;
}

CHECK AppDomain::CheckCanExecuteManagedCode(MethodDesc* pMD)
{
    CONTRACTL
    {
        THROWS;
        GC_TRIGGERS;
        MODE_ANY;
    }
    CONTRACTL_END;

    Module* pModule=pMD->GetModule();

    CHECK_MSG(CheckValidModule(pModule),
              "Managed code can only run when executing in the module's app domain");

    if (!pMD->IsInterface() || pMD->IsStatic()) //interfaces require no activation for instance methods
    {
        //cctor could have been interrupted by ADU
        CHECK_MSG(pModule->CheckActivated(),
              "Managed code can only run when its module has been activated in the current app domain");
    }

    CHECK_OK;
}

#endif // !DACCESS_COMPILE

void AppDomain::LoadAssembly(Assembly *pAssembly,
                               FileLoadLevel targetLevel)
{
    CONTRACTL
    {
        if (FORBIDGC_LOADER_USE_ENABLED()) NOTHROW; else THROWS;
        if (FORBIDGC_LOADER_USE_ENABLED()) GC_NOTRIGGER; else GC_TRIGGERS;
        if (FORBIDGC_LOADER_USE_ENABLED()) FORBID_FAULT; else { INJECT_FAULT(COMPlusThrowOM();); }
        INJECT_FAULT(COMPlusThrowOM(););
    }
    CONTRACTL_END;

    // Quick exit if finished
    if (pAssembly->GetLoadLevel() >= targetLevel)
        return;

    // Handle the error case
    pAssembly->ThrowIfError(targetLevel);


#ifndef DACCESS_COMPILE

    if (pAssembly->IsLoading())
    {
        GCX_PREEMP();

        // Load some more if appropriate
        LoadLockHolder lock(this);

        FileLoadLock* pLockEntry = (FileLoadLock *) lock->FindFileLock(pAssembly->GetPEAssembly());
        if (pLockEntry == NULL)
        {
            _ASSERTE (!pAssembly->IsLoading());
            return;
        }

        pLockEntry->AddRef();

        lock.Release();

        LoadAssembly(pLockEntry, targetLevel);
    }

#else // DACCESS_COMPILE
    DacNotImpl();
#endif // DACCESS_COMPILE
}

#ifndef DACCESS_COMPILE

thread_local LoadLevelLimiter* LoadLevelLimiter::t_currentLoadLevelLimiter = nullptr;

namespace
{
    FileLoadLevel GetCurrentFileLoadLevel()
    {
        WRAPPER_NO_CONTRACT;
        if (LoadLevelLimiter::GetCurrent() == NULL)
            return FILE_ACTIVE;
        else
            return (FileLoadLevel)(LoadLevelLimiter::GetCurrent()->GetLoadLevel()-1);
    }
}

Assembly *AppDomain::LoadAssembly(AssemblySpec* pSpec,
                                  PEAssembly * pPEAssembly,
                                  FileLoadLevel targetLevel)
{
    CONTRACT(Assembly *)
    {
        GC_TRIGGERS;
        THROWS;
        MODE_ANY;
        PRECONDITION(CheckPointer(pPEAssembly));
        POSTCONDITION(CheckPointer(RETVAL));
        INJECT_FAULT(COMPlusThrowOM(););
    }
    CONTRACT_END;

    if (pSpec == nullptr)
    {
        // skip caching, since we don't have anything to base it on
        RETURN LoadAssemblyInternal(pSpec, pPEAssembly, targetLevel);
    }

    Assembly* pRetVal = NULL;
    EX_TRY
    {
        pRetVal = LoadAssemblyInternal(pSpec, pPEAssembly, targetLevel);
    }
    EX_HOOK
    {
        Exception* pEx = GET_EXCEPTION();
        if (!pEx->IsTransient())
        {
            // Setup the binder reference in AssemblySpec from the PEAssembly if one is not already set.
            AssemblyBinder* pCurrentBinder = pSpec->GetBinder();
            AssemblyBinder* pBinderFromPEAssembly = pPEAssembly->GetAssemblyBinder();

            if (pCurrentBinder == NULL)
            {
                // Set the binding context we got from the PEAssembly if AssemblySpec does not
                // have that information
                _ASSERTE(pBinderFromPEAssembly != NULL);
                pSpec->SetBinder(pBinderFromPEAssembly);
            }
#if defined(_DEBUG)
            else
            {
                // Binding context in the spec should be the same as the binding context in the PEAssembly
                _ASSERTE(pCurrentBinder == pBinderFromPEAssembly);
            }
#endif // _DEBUG

            if (!EEFileLoadException::CheckType(pEx))
            {
                StackSString name;
                pSpec->GetDisplayName(0, name);
                pEx=new EEFileLoadException(name, pEx->GetHR(), pEx);
                AddExceptionToCache(pSpec, pEx);
                PAL_CPP_THROW(Exception *, pEx);
            }
            else
                AddExceptionToCache(pSpec, pEx);
        }
    }
    EX_END_HOOK;

    RETURN pRetVal;
}


Assembly *AppDomain::LoadAssemblyInternal(AssemblySpec* pIdentity,
                                              PEAssembly * pPEAssembly,
                                              FileLoadLevel targetLevel)
{
    CONTRACT(Assembly *)
    {
        GC_TRIGGERS;
        THROWS;
        MODE_ANY;
        PRECONDITION(CheckPointer(pPEAssembly));
        PRECONDITION(::GetAppDomain()==this);
        POSTCONDITION(CheckPointer(RETVAL));
        POSTCONDITION(RETVAL->GetLoadLevel() >= GetCurrentFileLoadLevel()
                      || RETVAL->GetLoadLevel() >= targetLevel);
        POSTCONDITION(RETVAL->CheckNoError(targetLevel));
        INJECT_FAULT(COMPlusThrowOM(););
    }
    CONTRACT_END;


    Assembly * result;

    // Go into preemptive mode since this may take a while.
    GCX_PREEMP();

    // Check for existing fully loaded assembly, or for an assembly which has failed during the loading process.
    result = FindAssembly(pPEAssembly, FindAssemblyOptions_IncludeFailedToLoad);

    if (result == NULL)
    {
        LoaderAllocator *pLoaderAllocator = NULL;

        AssemblyBinder *pAssemblyBinder = pPEAssembly->GetAssemblyBinder();
        // Assemblies loaded with CustomAssemblyBinder need to use a different LoaderAllocator if
        // marked as collectible
        pLoaderAllocator = pAssemblyBinder->GetLoaderAllocator();
        if (pLoaderAllocator == NULL)
        {
            pLoaderAllocator = this->GetLoaderAllocator();
        }

        // Allocate the DomainAssembly a bit early to avoid GC mode problems. We could potentially avoid
        // a rare redundant allocation by moving this closer to FileLoadLock::Create, but it's not worth it.
        AllocMemTracker amTracker;
        AllocMemTracker *pamTracker = &amTracker;
        NewHolder<DomainAssembly> pDomainAssembly = new DomainAssembly(pPEAssembly, pLoaderAllocator, pamTracker);

        LoadLockHolder lock(this);

        // Find the list lock entry
        FileLoadLock * fileLock = (FileLoadLock *)lock->FindFileLock(pPEAssembly);
        bool registerNewAssembly = false;
        if (fileLock == NULL)
        {
            // Check again in case we were racing
            result = FindAssembly(pPEAssembly, FindAssemblyOptions_IncludeFailedToLoad);
            if (result == NULL)
            {
                // We are the first one in - create the DomainAssembly
                registerNewAssembly = true;
                fileLock = FileLoadLock::Create(lock, pPEAssembly, pDomainAssembly->GetAssembly());
                pDomainAssembly.SuppressRelease();
                pamTracker->SuppressRelease();

                // Set the assembly module to be tenured now that we know it won't be deleted
                pDomainAssembly->GetAssembly()->SetIsTenured();
                if (pDomainAssembly->GetAssembly()->IsCollectible())
                {
                    // We add the assembly to the LoaderAllocator only when we are sure that it can be added
                    // and won't be deleted in case of a concurrent load from the same ALC
                    ((AssemblyLoaderAllocator *)pLoaderAllocator)->AddDomainAssembly(pDomainAssembly);
                }
            }
        }
        else
        {
            fileLock->AddRef();
        }

        lock.Release();

        if (result == NULL)
        {
            // We pass our ref on fileLock to LoadAssembly to release.

            // Note that if we throw here, we will poison fileLock with an error condition,
            // so it will not be removed until app domain unload.  So there is no need
            // to release our ref count.
            result = LoadAssembly(fileLock, targetLevel);
        }
        else
        {
            result->EnsureLoadLevel(targetLevel);
        }

        if (registerNewAssembly)
        {
            pPEAssembly->GetAssemblyBinder()->AddLoadedAssembly(pDomainAssembly->GetAssembly());
        }
    }
    else
    {
        result->EnsureLoadLevel(targetLevel);
    }

    // Cache result in all cases, since found pPEAssembly could be from a different AssemblyRef than pIdentity
    if (pIdentity == NULL)
    {
        AssemblySpec spec;
        spec.InitializeSpec(result->GetPEAssembly());
        GetAppDomain()->AddAssemblyToCache(&spec, result);
    }
    else
    {
        GetAppDomain()->AddAssemblyToCache(pIdentity, result);
    }

    RETURN result;
} // AppDomain::LoadAssembly

Assembly *AppDomain::LoadAssembly(FileLoadLock *pLock, FileLoadLevel targetLevel)
{
    CONTRACT(Assembly *)
    {
        STANDARD_VM_CHECK;
        PRECONDITION(CheckPointer(pLock));
        PRECONDITION(AppDomain::GetCurrentDomain() == this);
        POSTCONDITION(RETVAL->GetLoadLevel() >= GetCurrentFileLoadLevel()
                      || RETVAL->GetLoadLevel() >= targetLevel);
        POSTCONDITION(RETVAL->CheckNoError(targetLevel));
    }
    CONTRACT_END;

    Assembly *pAssembly = pLock->GetAssembly();

    // Make sure we release the lock on exit
    FileLoadLockRefHolder lockRef(pLock);

    // Do a quick out check for the already loaded case.
    if (pLock->GetLoadLevel() >= targetLevel)
    {
        pAssembly->ThrowIfError(targetLevel);

        RETURN pAssembly;
    }

    // Initialize a loading queue.  This will hold any loads which are triggered recursively but
    // which cannot be immediately satisfied due to anti-deadlock constraints.

    // PendingLoadQueues are allocated on the stack during a load, and
    // shared with all nested loads on the same thread. (Note that we won't use
    // "candidate" if we are in a recursive load; that's OK since they are cheap to
    // construct.)
    FileLoadLevel immediateTargetLevel = targetLevel;
    {
        LoadLevelLimiter limit;
        limit.Activate();

        // We cannot set a target level higher than that allowed by the limiter currently.
        // This is because of anti-deadlock constraints.
        if (immediateTargetLevel > limit.GetLoadLevel())
            immediateTargetLevel = limit.GetLoadLevel();

        LOG((LF_LOADER, LL_INFO100, "LOADER: ***%s*\t>>>Load initiated, %s/%s\n",
             pAssembly->GetSimpleName(),
             fileLoadLevelName[immediateTargetLevel], fileLoadLevelName[targetLevel]));

        // Now loop and do the load incrementally to the target level.
        if (pLock->GetLoadLevel() < immediateTargetLevel)
        {
            while (pLock->Acquire(immediateTargetLevel))
            {
                FileLoadLevel workLevel;
                {
                    FileLoadLockHolder fileLock(pLock);

                    // Work level is next step to do
                    workLevel = (FileLoadLevel)(fileLock->GetLoadLevel()+1);

                    // Set up the anti-deadlock constraint: we cannot safely recursively load any assemblies
                    // on this thread to a higher level than this assembly is being loaded now.
                    // Note that we do allow work at a parallel level; any deadlocks caused here will
                    // be resolved by the deadlock detection in the FileLoadLocks.
                    limit.SetLoadLevel(workLevel);

                    LOG((LF_LOADER,
                         (workLevel == FILE_LOAD_BEGIN
                          || workLevel == FILE_LOADED
                          || workLevel == FILE_ACTIVE)
                         ? LL_INFO10 : LL_INFO1000,
                         "LOADER: %p:***%s*\t   loading at level %s\n",
                         this, pAssembly->GetSimpleName(), fileLoadLevelName[workLevel]));

                    TryIncrementalLoad(pAssembly, workLevel, fileLock);
                }
            }

            if (pLock->GetLoadLevel() == immediateTargetLevel-1)
            {
                LOG((LF_LOADER, LL_INFO100, "LOADER: ***%s*\t<<<Load limited due to detected deadlock, %s\n",
                     pAssembly->GetSimpleName(),
                     fileLoadLevelName[immediateTargetLevel-1]));
            }
        }

        LOG((LF_LOADER, LL_INFO100, "LOADER: ***%s*\t<<<Load completed, %s\n",
             pAssembly->GetSimpleName(),
             fileLoadLevelName[pLock->GetLoadLevel()]));

    }

    // There may have been an error stored on the domain file by another thread, or from a previous load
    pAssembly->ThrowIfError(targetLevel);

    // There are two normal results from the above loop.
    //
    // 1. We succeeded in loading the file to the current thread's load level.
    // 2. We succeeded in loading the file to the current thread's load level - 1, due
    //      to deadlock condition with another thread loading the same assembly.
    //
    // Either of these are considered satisfactory results, as code inside a load must expect
    // a parial load result.
    //
    // However, if load level elevation has occurred, then it is possible for a deadlock to
    // prevent us from loading an assembly which was loading before the elevation at a radically
    // lower level.  In such a case, we throw an exception which transiently fails the current
    // load, since it is likely we have not satisfied the caller.
    // (An alternate, and possibly preferable, strategy here would be for all callers to explicitly
    // specify the minimum load level acceptable and throw if not reached.)

    pAssembly->RequireLoadLevel((FileLoadLevel)(immediateTargetLevel-1));
    RETURN pAssembly;
}

void AppDomain::TryIncrementalLoad(Assembly *pAssembly, FileLoadLevel workLevel, FileLoadLockHolder &lockHolder)
{
    STANDARD_VM_CONTRACT;

    // This is factored out so we don't call EX_TRY in a loop (EX_TRY can _alloca)

    BOOL released = FALSE;
    FileLoadLock* pLoadLock = lockHolder.GetValue();

    EX_TRY
    {
        // Do the work
        BOOL success = pAssembly->DoIncrementalLoad(workLevel);

        // Complete the level.
        if (pLoadLock->CompleteLoadLevel(workLevel, success) &&
            pLoadLock->GetLoadLevel()==FILE_LOAD_DELIVER_EVENTS)
        {
            lockHolder.Release();
            released = TRUE;
            pAssembly->DeliverAsyncEvents();
        };
    }
    EX_HOOK
    {
        Exception *pEx = GET_EXCEPTION();

        //We will cache this error and wire this load to forever fail,
        // unless the exception is transient or the file is loaded OK but just cannot execute
        if (!pEx->IsTransient() && !pAssembly->IsLoaded())
        {
            if (released)
            {
                // Reobtain lock to increment level. (Note that another thread may
                // have already done it which is OK.
                if (pLoadLock->Acquire(workLevel)) // note pLockHolder->Acquire isn't wired up to actually take the lock
                {
                    // note lockHolder.Acquire isn't wired up to actually take the lock
                    lockHolder = pLoadLock;
                    released = FALSE;
                }
            }

            if (!released)
            {
                // Report the error in the lock
                pLoadLock->SetError(pEx);
            }

            if (!EEFileLoadException::CheckType(pEx))
                EEFileLoadException::Throw(pAssembly->GetPEAssembly(), pEx->GetHR(), pEx);
        }

        // Otherwise, we simply abort this load, and can retry later on.
        // @todo cleanup: make sure that each level is restartable after an exception, and
        // leaves no bad side effects
    }
    EX_END_HOOK;
}

// Checks whether the module is valid to be in the given app domain (need not be yet loaded)
CHECK AppDomain::CheckValidModule(Module * pModule)
{
    CONTRACTL
    {
        THROWS;
        GC_TRIGGERS;
        MODE_ANY;
    }
    CONTRACTL_END;

    if (pModule->GetDomainAssembly() != NULL)
        CHECK_OK;

    CHECK_OK;
}

void AppDomain::SetupSharedStatics()
{
    CONTRACTL
    {
        THROWS;
        GC_TRIGGERS;
        MODE_ANY;
        INJECT_FAULT(COMPlusThrowOM(););
    }
    CONTRACTL_END;

    LOG((LF_CLASSLOADER, LL_INFO10000, "STATICS: SetupSharedStatics()"));

    // don't do any work in init stage. If not init only do work in non-shared case if are default domain
    _ASSERTE(!g_fEEInit);

    // Because we are allocating/referencing objects, need to be in cooperative mode
    GCX_COOP();

    // This is a convenient place to initialize String.Empty.
    // It is treated as intrinsic by the JIT as so the static constructor would never run.
    // Leaving it uninitialized would confuse debuggers.

    // String should not have any static constructors, so this should be safe. It will just ensure that statics are allocated
    g_pStringClass->CheckRunClassInitThrowing();

    FieldDesc * pEmptyStringFD = CoreLibBinder::GetField(FIELD__STRING__EMPTY);
    OBJECTREF* pEmptyStringHandle = (OBJECTREF*)
        ((TADDR)g_pStringClass->GetDynamicStaticsInfo()->GetGCStaticsPointer()+pEmptyStringFD->GetOffset());
    SetObjectReference( pEmptyStringHandle, StringObject::GetEmptyString());
}

Assembly * AppDomain::FindAssembly(PEAssembly * pPEAssembly, FindAssemblyOptions options/* = FindAssemblyOptions_None*/)
{
    CONTRACTL
    {
        NOTHROW;
        GC_NOTRIGGER;
        MODE_ANY;
    }
    CONTRACTL_END;

    const bool includeFailedToLoad = (options & FindAssemblyOptions_IncludeFailedToLoad) != 0;

    if (pPEAssembly->HasHostAssembly())
    {
        Assembly * pAssembly = pPEAssembly->GetHostAssembly()->GetRuntimeAssembly();
        if (pAssembly != nullptr && (pAssembly->IsLoaded() || (includeFailedToLoad && pAssembly->IsError())))
        {
            return pAssembly;
        }
        return nullptr;
    }

    AssemblyIterator i = IterateAssembliesEx((AssemblyIterationFlags)(
        kIncludeLoaded |
        (includeFailedToLoad ? kIncludeFailedToLoad : 0) |
        kIncludeExecution));
    CollectibleAssemblyHolder<Assembly *> pAssembly;

    while (i.Next(pAssembly.This()))
    {
        PEAssembly * pManifestFile = pAssembly->GetPEAssembly();
        if (pManifestFile &&
            pManifestFile->Equals(pPEAssembly))
        {
            return pAssembly;
        }
    }
    return NULL;
}

void AppDomain::SetFriendlyName(LPCWSTR pwzFriendlyName)
{
    CONTRACTL
    {
        THROWS;
        if (GetThreadNULLOk()) {GC_TRIGGERS;} else {DISABLED(GC_NOTRIGGER);}
        MODE_ANY;
        INJECT_FAULT(COMPlusThrowOM(););
    }
    CONTRACTL_END;

    // Do all computations into a temporary until we're ensured of success
    SString tmpFriendlyName;


    if (pwzFriendlyName)
        tmpFriendlyName.Set(pwzFriendlyName);
    else
        tmpFriendlyName.Set(DEFAULT_DOMAIN_FRIENDLY_NAME);

    tmpFriendlyName.Normalize();

    // This happens at most twice in a process, so don't worry about freeing the old one.
    LPWSTR newFriendlyName = new WCHAR[tmpFriendlyName.GetCount() + 1];
    u16_strcpy_s(newFriendlyName, tmpFriendlyName.GetCount() + 1, tmpFriendlyName.GetUnicode());

    m_friendlyName = newFriendlyName;

    if(g_pDebugInterface)
    {
        // inform the attached debugger that the name of this appdomain has changed.
        if (IsDebuggerAttached())
            g_pDebugInterface->NameChangeEvent(this, NULL);
    }
}
#endif // !DACCESS_COMPILE

LPCWSTR AppDomain::GetFriendlyName()
{
    CONTRACT (LPCWSTR)
    {
        NOTHROW;
        GC_NOTRIGGER;
        MODE_ANY;
        POSTCONDITION(CheckPointer(RETVAL, NULL_OK));
        INJECT_FAULT(COMPlusThrowOM(););
    }
    CONTRACT_END;

    if (m_friendlyName == NULL)
        RETURN DEFAULT_DOMAIN_FRIENDLY_NAME;

    RETURN (LPCWSTR)m_friendlyName;
}

#ifndef DACCESS_COMPILE

LPCWSTR AppDomain::GetFriendlyNameForDebugger()
{
    CONTRACT (LPCWSTR)
    {
        NOTHROW;
        if (GetThreadNULLOk()) {GC_TRIGGERS;} else {DISABLED(GC_NOTRIGGER);}
        MODE_ANY;
        POSTCONDITION(CheckPointer(RETVAL));
    }
    CONTRACT_END;


    if (m_friendlyName == NULL)
    {
        BOOL fSuccess = FALSE;

        EX_TRY
        {
            SetFriendlyName(NULL);

            fSuccess = TRUE;
        }
        EX_CATCH
        {
            // Gobble all exceptions.
        }
        EX_END_CATCH

        if (!fSuccess)
        {
            RETURN W("");
        }
    }

    RETURN m_friendlyName;
}


#endif // !DACCESS_COMPILE

#ifndef DACCESS_COMPILE

BOOL AppDomain::AddFileToCache(AssemblySpec* pSpec, PEAssembly * pPEAssembly)
{
    CONTRACTL
    {
        THROWS;
        GC_TRIGGERS;
        MODE_ANY;
        PRECONDITION(CheckPointer(pSpec));
        INJECT_FAULT(COMPlusThrowOM(););
    }
    CONTRACTL_END;

    GCX_PREEMP();
    DomainCacheCrstHolderForGCCoop holder(this);

    return m_AssemblyCache.StorePEAssembly(pSpec, pPEAssembly);
}

BOOL AppDomain::AddAssemblyToCache(AssemblySpec* pSpec, Assembly *pAssembly)
{
    CONTRACTL
    {
        THROWS;
        GC_TRIGGERS;
        MODE_ANY;
        PRECONDITION(CheckPointer(pSpec));
        PRECONDITION(CheckPointer(pAssembly));
        INJECT_FAULT(COMPlusThrowOM(););
    }
    CONTRACTL_END;

    GCX_PREEMP();
    DomainCacheCrstHolderForGCCoop holder(this);

    // !!! suppress exceptions
    BOOL bRetVal = m_AssemblyCache.StoreAssembly(pSpec, pAssembly);
    return bRetVal;
}

BOOL AppDomain::AddExceptionToCache(AssemblySpec* pSpec, Exception *ex)
{
    CONTRACTL
    {
        THROWS;
        GC_TRIGGERS;
        MODE_ANY;
        PRECONDITION(CheckPointer(pSpec));
        INJECT_FAULT(COMPlusThrowOM(););
    }
    CONTRACTL_END;

    if (ex->IsTransient())
        return TRUE;

    GCX_PREEMP();
    DomainCacheCrstHolderForGCCoop holder(this);

    // !!! suppress exceptions
    return m_AssemblyCache.StoreException(pSpec, ex);
}

void AppDomain::AddUnmanagedImageToCache(LPCWSTR libraryName, NATIVE_LIBRARY_HANDLE hMod)
{
    CONTRACTL
    {
        THROWS;
        GC_NOTRIGGER;
        MODE_ANY;
        PRECONDITION(CheckPointer(libraryName));
        PRECONDITION(CheckPointer(hMod));
        INJECT_FAULT(COMPlusThrowOM(););
    }
    CONTRACTL_END;

    DomainCacheCrstHolderForGCPreemp lock(this);

    const UnmanagedImageCacheEntry *existingEntry = m_unmanagedCache.LookupPtr(libraryName);
    if (existingEntry != NULL)
    {
        _ASSERTE(existingEntry->Handle == hMod);
        return;
    }

    size_t len = (u16_strlen(libraryName) + 1) * sizeof(WCHAR);
    AllocMemHolder<WCHAR> copiedName(GetLowFrequencyHeap()->AllocMem(S_SIZE_T(len)));
    memcpy(copiedName, libraryName, len);

    m_unmanagedCache.Add(UnmanagedImageCacheEntry{ copiedName, hMod });
    copiedName.SuppressRelease();
}

NATIVE_LIBRARY_HANDLE AppDomain::FindUnmanagedImageInCache(LPCWSTR libraryName)
{
    CONTRACT(NATIVE_LIBRARY_HANDLE)
    {
        THROWS;
        GC_NOTRIGGER;
        MODE_ANY;
        PRECONDITION(CheckPointer(libraryName));
        POSTCONDITION(CheckPointer(RETVAL,NULL_OK));
        INJECT_FAULT(COMPlusThrowOM(););
    }
    CONTRACT_END;

    DomainCacheCrstHolderForGCPreemp lock(this);

    const UnmanagedImageCacheEntry *existingEntry = m_unmanagedCache.LookupPtr(libraryName);
    if (existingEntry == NULL)
        RETURN NULL;

    RETURN existingEntry->Handle;
}

BOOL AppDomain::RemoveFileFromCache(PEAssembly * pPEAssembly)
{
    CONTRACTL
    {
        GC_TRIGGERS;
        PRECONDITION(CheckPointer(pPEAssembly));
    }
    CONTRACTL_END;

    LoadLockHolder lock(this);
    FileLoadLock *fileLock = (FileLoadLock *)lock->FindFileLock(pPEAssembly);

    if (fileLock == NULL)
        return FALSE;

    VERIFY(lock->Unlink(fileLock));

    fileLock->Release();

    return TRUE;
}

BOOL AppDomain::RemoveAssemblyFromCache(Assembly* pAssembly)
{
    CONTRACTL
    {
        THROWS;
        GC_TRIGGERS;
        MODE_ANY;
        PRECONDITION(CheckPointer(pAssembly));
        INJECT_FAULT(COMPlusThrowOM(););
    }
    CONTRACTL_END;

    GCX_PREEMP();
    DomainCacheCrstHolderForGCCoop holder(this);

    return m_AssemblyCache.RemoveAssembly(pAssembly);
}

BOOL AppDomain::IsCached(AssemblySpec *pSpec)
{
    WRAPPER_NO_CONTRACT;

    // Check to see if this fits our rather loose idea of a reference to CoreLib.
    // If so, don't use fusion to bind it - do it ourselves.
    if (pSpec->IsCoreLib())
        return TRUE;

    return m_AssemblyCache.Contains(pSpec);
}

PEAssembly* AppDomain::FindCachedFile(AssemblySpec* pSpec, BOOL fThrow /*=TRUE*/)
{
    CONTRACTL
    {
        if (fThrow) {
            GC_TRIGGERS;
            THROWS;
        }
        else {
            GC_NOTRIGGER;
            NOTHROW;
        }
        MODE_ANY;
    }
    CONTRACTL_END;

    // Check to see if this fits our rather loose idea of a reference to CoreLib.
    // If so, don't use fusion to bind it - do it ourselves.
    if (fThrow && pSpec->IsCoreLib())
    {
        CONSISTENCY_CHECK(SystemDomain::System()->SystemAssembly() != NULL);
        PEAssembly * pPEAssembly = SystemDomain::System()->SystemPEAssembly();
        pPEAssembly->AddRef();
        return pPEAssembly;
    }

    return m_AssemblyCache.LookupFile(pSpec, fThrow);
}


BOOL AppDomain::PostBindResolveAssembly(AssemblySpec  *pPrePolicySpec,
                                        AssemblySpec  *pPostPolicySpec,
                                        HRESULT        hrBindResult,
                                        AssemblySpec **ppFailedSpec)
{
    STATIC_CONTRACT_THROWS;
    STATIC_CONTRACT_GC_TRIGGERS;
    PRECONDITION(CheckPointer(pPrePolicySpec));
    PRECONDITION(CheckPointer(pPostPolicySpec));
    PRECONDITION(CheckPointer(ppFailedSpec));

    BOOL fFailure = TRUE;
    *ppFailedSpec = pPrePolicySpec;

    PEAssemblyHolder result;

    if ((EEFileLoadException::GetFileLoadKind(hrBindResult) == kFileNotFoundException) ||
        (hrBindResult == FUSION_E_REF_DEF_MISMATCH) ||
        (hrBindResult == FUSION_E_INVALID_NAME))
    {
        result = TryResolveAssemblyUsingEvent(*ppFailedSpec);

        if (result != NULL)
        {
            fFailure = FALSE;

            // Given the post-policy resolve event construction of the CLR binder,
            // chained managed resolve events can race with each other, therefore we do allow
            // the adding of the result to fail. Checking for already chached specs
            // is not an option as it would introduce another race window.
            // The binder does a re-fetch of the
            // original binding spec and therefore will not cause inconsistency here.
            // For the purposes of the resolve event, failure to add to the cache still is a success.
            AddFileToCache(pPrePolicySpec, result);
            if (*ppFailedSpec != pPrePolicySpec)
            {
                AddFileToCache(pPostPolicySpec, result);
            }
        }
    }

    return fFailure;
}

//---------------------------------------------------------------------------------------------------------------------
PEAssembly * AppDomain::BindAssemblySpec(
    AssemblySpec *         pSpec,
    BOOL                   fThrowOnFileNotFound)
{
    STATIC_CONTRACT_THROWS;
    STATIC_CONTRACT_GC_TRIGGERS;
    PRECONDITION(CheckPointer(pSpec));
    PRECONDITION(pSpec->GetAppDomain() == this);
    PRECONDITION(this==::GetAppDomain());

    GCX_PREEMP();

    BOOL fForceReThrow = FALSE;

    BinderTracing::AssemblyBindOperation bindOperation(pSpec);

    HRESULT hrBindResult = S_OK;
    PEAssemblyHolder result;

    bool isCached = false;
    EX_TRY
    {
        isCached = IsCached(pSpec);
        if (!isCached)
        {

            {
                ReleaseHolder<BINDER_SPACE::Assembly> boundAssembly;
                hrBindResult = pSpec->Bind(this, &boundAssembly);

                if (boundAssembly)
                {
                    if (SystemDomain::SystemPEAssembly() && boundAssembly->GetAssemblyName()->IsCoreLib())
                    {
                        // Avoid rebinding to another copy of CoreLib
                        result = SystemDomain::SystemPEAssembly();
                        result.SuppressRelease(); // Didn't get a refcount
                    }
                    else
                    {
                        // IsSystem on the PEAssembly should be false, even for CoreLib satellites
                        result = PEAssembly::Open(boundAssembly);
                    }

                    // Setup the reference to the binder, which performed the bind, into the AssemblySpec
                    AssemblyBinder* pBinder = result->GetAssemblyBinder();
                    _ASSERTE(pBinder != NULL);
                    pSpec->SetBinder(pBinder);

                    // Failure to add simply means someone else beat us to it. In that case
                    // the FindCachedFile call below (after catch block) will update result
                    // to the cached value.
                    AddFileToCache(pSpec, result);
                }
                else
                {
                    // Don't trigger the resolve event for the CoreLib satellite assembly. A misbehaving resolve event may
                    // return an assembly that does not match, and this can cause recursive resource lookups during error
                    // reporting. The CoreLib satellite assembly is loaded from relative locations based on the culture, see
                    // AssemblySpec::Bind().
                    if (!pSpec->IsCoreLibSatellite())
                    {
                        // Trigger the resolve event also for non-throw situation.
                        AssemblySpec NewSpec(this);
                        AssemblySpec *pFailedSpec = NULL;

                        fForceReThrow = TRUE; // Managed resolve event handler can throw

                        BOOL fFailure = PostBindResolveAssembly(pSpec, &NewSpec, hrBindResult, &pFailedSpec);

                        if (fFailure && fThrowOnFileNotFound)
                        {
                            EEFileLoadException::Throw(pFailedSpec, COR_E_FILENOTFOUND, NULL);
                        }
                    }
                }
            }
        }
    }
    EX_CATCH
    {
        Exception *ex = GET_EXCEPTION();

        AssemblySpec NewSpec(this);
        AssemblySpec *pFailedSpec = NULL;

        // Let transient exceptions or managed resolve event handler exceptions propagate
        if (ex->IsTransient() || fForceReThrow)
        {
            EX_RETHROW;
        }

        {
            BOOL fFailure = PostBindResolveAssembly(pSpec, &NewSpec, ex->GetHR(), &pFailedSpec);
            if (fFailure)
            {
                BOOL bFileNotFoundException =
                    (EEFileLoadException::GetFileLoadKind(ex->GetHR()) == kFileNotFoundException);

                if (!bFileNotFoundException)
                {
                    fFailure = AddExceptionToCache(pFailedSpec, ex);
                } // else, fFailure stays TRUE
                // Effectively, fFailure == bFileNotFoundException || AddExceptionToCache(pFailedSpec, ex)

                // Only throw this exception if we are the first in the cache
                if (fFailure)
                {
                    // Store the failure information for DAC to read
                    if (IsDebuggerAttached()) {
                        FailedAssembly *pFailed = new FailedAssembly();
                        pFailed->Initialize(pFailedSpec, ex);
                        IfFailThrow(m_failedAssemblies.Append(pFailed));
                    }

                    if (!bFileNotFoundException || fThrowOnFileNotFound)
                    {
                        // V1.1 App-compatibility workaround. See VSW530166 if you want to whine about it.
                        //
                        // In Everett, if we failed to download an assembly because of a broken network cable,
                        // we returned a FileNotFoundException with a COR_E_FILENOTFOUND hr embedded inside
                        // (which would be exposed when marshaled to native.)
                        //
                        // In Whidbey, we now set the more appropriate INET_E_RESOURCE_NOT_FOUND hr. But
                        // the online/offline switch code in VSTO for Everett hardcoded a check for
                        // COR_E_FILENOTFOUND.
                        //
                        // So now, to keep that code from breaking, we have to remap INET_E_RESOURCE_NOT_FOUND
                        // back to COR_E_FILENOTFOUND. We're doing it here rather down in Fusion so as to affect
                        // the least number of callers.

                        if (ex->GetHR() == INET_E_RESOURCE_NOT_FOUND)
                        {
                            EEFileLoadException::Throw(pFailedSpec, COR_E_FILENOTFOUND, ex);
                        }

                        if (EEFileLoadException::CheckType(ex))
                        {
                            if (pFailedSpec == pSpec)
                            {
                                EX_RETHROW; //preserve the information
                            }
                            else
                            {
                                StackSString exceptionDisplayName, failedSpecDisplayName;

                                ((EEFileLoadException*)ex)->GetName(exceptionDisplayName);
                                pFailedSpec->GetDisplayName(0, failedSpecDisplayName);

                                if (exceptionDisplayName.CompareCaseInsensitive(failedSpecDisplayName) == 0)
                                {
                                    EX_RETHROW; // Throw the original exception. Otherwise, we'd throw an exception that contains the same message twice.
                                }
                            }
                        }

                        EEFileLoadException::Throw(pFailedSpec, ex->GetHR(), ex);
                    }

                }
            }
        }
        RethrowTerminalExceptions();
    }
    EX_END_CATCH

    // Now, if it's a cacheable bind we need to re-fetch the result from the cache, as we may have been racing with another
    // thread to store our result.  Note that we may throw from here, if there is a cached exception.
    // This will release the refcount of the current result holder (if any), and will replace
    // it with a non-addref'ed result
    result = FindCachedFile(pSpec);

    if (result != NULL)
        result->AddRef();

    bindOperation.SetResult(result.GetValue(), isCached);
    return result.Extract();
} // AppDomain::BindAssemblySpec



PEAssembly *AppDomain::TryResolveAssemblyUsingEvent(AssemblySpec *pSpec)
{
    STATIC_CONTRACT_THROWS;
    STATIC_CONTRACT_GC_TRIGGERS;
    STATIC_CONTRACT_MODE_ANY;

    // No assembly resolve on codebase binds
    if (pSpec->GetName() == nullptr)
        return nullptr;

    PEAssembly *result = nullptr;
    EX_TRY
    {
        Assembly *pAssembly = RaiseAssemblyResolveEvent(pSpec);
        if (pAssembly != nullptr)
        {
            PEAssembly* pPEAssembly = pAssembly->GetPEAssembly();
            pPEAssembly->AddRef();
            result = pPEAssembly;
        }

        BinderTracing::ResolutionAttemptedOperation::TraceAppDomainAssemblyResolve(pSpec, result);
    }
    EX_HOOK
    {
        Exception *pEx = GET_EXCEPTION();
        BinderTracing::ResolutionAttemptedOperation::TraceAppDomainAssemblyResolve(pSpec, nullptr, pEx);
        if (!pEx->IsTransient())
        {
            AddExceptionToCache(pSpec, pEx);
            if (!EEFileLoadException::CheckType(pEx))
                EEFileLoadException::Throw(pSpec, pEx->GetHR(), pEx);
        }
    }
    EX_END_HOOK;

    return result;
}

void AppDomain::RaiseLoadingAssemblyEvent(Assembly *pAssembly)
{
    CONTRACTL
    {
        NOTHROW;
        GC_TRIGGERS;
        PRECONDITION(this == GetAppDomain());
        MODE_ANY;
    }
    CONTRACTL_END;

    if (pAssembly->GetPEAssembly()->IsSystem())
    {
        return;
    }

    GCX_COOP();
    FAULT_NOT_FATAL();
    OVERRIDE_TYPE_LOAD_LEVEL_LIMIT(CLASS_LOADED);

    EX_TRY
    {
        if (CoreLibBinder::GetField(FIELD__ASSEMBLYLOADCONTEXT__ASSEMBLY_LOAD)->GetStaticOBJECTREF() != NULL)
        {
            struct {
                OBJECTREF    orThis;
            } gc;
            gc.orThis = NULL;

            ARG_SLOT args[1];
            GCPROTECT_BEGIN(gc);

            gc.orThis = pAssembly->GetExposedObject();

            MethodDescCallSite onAssemblyLoad(METHOD__ASSEMBLYLOADCONTEXT__ON_ASSEMBLY_LOAD);

            // GetExposedAssemblyObject may cause a gc, so call this before filling args[0]
            args[0] = ObjToArgSlot(gc.orThis);

            onAssemblyLoad.Call(args);

            GCPROTECT_END();
        }
    }
    EX_CATCH
    {
    }
    EX_END_CATCH
}

void AppDomain::OnUnhandledException(OBJECTREF* pThrowable)
{
    CONTRACTL
    {
        NOTHROW;
        GC_TRIGGERS;
        MODE_COOPERATIVE;
        PRECONDITION(pThrowable != NULL);
    }
    CONTRACTL_END;

    EX_TRY
    {
        MethodDescCallSite raiseEvent(METHOD__APPCONTEXT__ON_UNHANDLED_EXCEPTION);
        ARG_SLOT args[] =
        {
            ObjToArgSlot(*pThrowable)
        };
        raiseEvent.Call(args);
    }
    EX_CATCH
    {
    }
    EX_END_CATCH  // Swallow any errors.
}

void AppDomain::RaiseExitProcessEvent()
{
    if (!g_fEEStarted)
        return;

    STATIC_CONTRACT_MODE_COOPERATIVE;
    STATIC_CONTRACT_THROWS;
    STATIC_CONTRACT_GC_TRIGGERS;

    // Only finalizer thread during shutdown can call this function.
    _ASSERTE (g_fEEShutDown && FinalizerThread::IsCurrentThreadFinalizer());

    _ASSERTE (GetThread()->PreemptiveGCDisabled());

    MethodDescCallSite onProcessExit(METHOD__APPCONTEXT__ON_PROCESS_EXIT);
    onProcessExit.Call(NULL);
}

DefaultAssemblyBinder *AppDomain::CreateDefaultBinder()
{
    CONTRACT(DefaultAssemblyBinder *)
    {
        GC_TRIGGERS;
        THROWS;
        MODE_ANY;
        POSTCONDITION(CheckPointer(RETVAL));
        INJECT_FAULT(COMPlusThrowOM(););
    }
    CONTRACT_END;

    if (!m_pDefaultBinder)
    {
        ETWOnStartup (FusionAppCtx_V1, FusionAppCtxEnd_V1);

        GCX_PREEMP();

        // Initialize the assembly binder for the default context loads for CoreCLR.
        IfFailThrow(BINDER_SPACE::AssemblyBinderCommon::CreateDefaultBinder(&m_pDefaultBinder));
    }

    RETURN m_pDefaultBinder;
}



//---------------------------------------------------------------------------------------
//
// AppDomain::IsDebuggerAttached - is a debugger attached to this process
//
// Arguments:
//    None
//
// Return Value:
//    TRUE if a debugger is attached to this process, FALSE otherwise.
//
// Notes:
//    This is identical to CORDebuggerAttached.  This exists idependantly for legacy reasons - we used to
//    support attaching to individual AppDomains.  This should probably go away eventually.
//

BOOL AppDomain::IsDebuggerAttached()
{
    LIMITED_METHOD_CONTRACT;

    if (CORDebuggerAttached())
    {
        return TRUE;
    }
    else
    {
        return FALSE;
    }
}

#ifdef DEBUGGING_SUPPORTED

// This is called from the debugger to request notification events from
// Assemblies, Modules, Types in this appdomain.
BOOL AppDomain::NotifyDebuggerLoad(int flags, BOOL attaching)
{
    WRAPPER_NO_CONTRACT;
    BOOL result = FALSE;

    if (!attaching && !IsDebuggerAttached())
        return FALSE;

    AssemblyIterator i;

    // Attach to our assemblies
    LOG((LF_CORDB, LL_INFO100, "AD::NDA: Iterating assemblies\n"));
    i = IterateAssembliesEx((AssemblyIterationFlags)(kIncludeLoaded | kIncludeLoading | kIncludeExecution));
    CollectibleAssemblyHolder<Assembly *> pAssembly;
    while (i.Next(pAssembly.This()))
    {
        result = (pAssembly->NotifyDebuggerLoad(flags, attaching) ||
                  result);
    }

    return result;
}

void AppDomain::NotifyDebuggerUnload()
{
    WRAPPER_NO_CONTRACT;
    if (!IsDebuggerAttached())
        return;

    LOG((LF_CORDB, LL_INFO10, "AD::NDD domain %#08x\n", this));

    LOG((LF_CORDB, LL_INFO100, "AD::NDD: Interating domain bound assemblies\n"));
    AssemblyIterator i = IterateAssembliesEx((AssemblyIterationFlags)(kIncludeLoaded |  kIncludeLoading  | kIncludeExecution));
    CollectibleAssemblyHolder<Assembly *> pAssembly;

    // Detach from our assemblies
    while (i.Next(pAssembly.This()))
    {
        LOG((LF_CORDB, LL_INFO100, "AD::NDD: Iterating assemblies\n"));
        pAssembly->NotifyDebuggerUnload();
    }
}
#endif // DEBUGGING_SUPPORTED


#ifdef FEATURE_COMWRAPPERS

RCWRefCache *AppDomain::GetRCWRefCache()
{
    CONTRACT(RCWRefCache*)
    {
        THROWS;
        GC_NOTRIGGER;
        MODE_ANY;
        POSTCONDITION(CheckPointer(RETVAL));
    }
    CONTRACT_END;

    if (!m_pRCWRefCache) {
        NewHolder<RCWRefCache> pRCWRefCache = new RCWRefCache(this);
        if (InterlockedCompareExchangeT(&m_pRCWRefCache, (RCWRefCache *)pRCWRefCache, NULL) == NULL)
        {
            pRCWRefCache.SuppressRelease();
        }
    }
    RETURN m_pRCWRefCache;
}
#endif // FEATURE_COMWRAPPERS

#ifdef FEATURE_COMINTEROP

RCWCache *AppDomain::CreateRCWCache()
{
    CONTRACT(RCWCache*)
    {
        THROWS;
        GC_TRIGGERS;
        MODE_ANY;
        INJECT_FAULT(COMPlusThrowOM(););
        POSTCONDITION(CheckPointer(RETVAL));
    }
    CONTRACT_END;

    // Initialize the global RCW cleanup list here as well. This is so that it
    // it guaranteed to exist if any RCW's are created, but it is not created
    // unconditionally.
    if (!g_pRCWCleanupList)
    {
        SystemDomain::LockHolder lh;

        if (!g_pRCWCleanupList)
            g_pRCWCleanupList = new RCWCleanupList();
    }
    _ASSERTE(g_pRCWCleanupList);

    if (!m_pRCWCache)
    {
        NewHolder<RCWCache> pRCWCache = new RCWCache(this);
        if (InterlockedCompareExchangeT(&m_pRCWCache, (RCWCache *)pRCWCache, NULL) == NULL)
        {
            pRCWCache.SuppressRelease();
        }
    }

    RETURN m_pRCWCache;
}

void AppDomain::ReleaseRCWs(LPVOID pCtxCookie)
{
    WRAPPER_NO_CONTRACT;
    if (m_pRCWCache)
        m_pRCWCache->ReleaseWrappersWorker(pCtxCookie);
}

void AppDomain::DetachRCWs()
{
    WRAPPER_NO_CONTRACT;
    if (m_pRCWCache)
        m_pRCWCache->DetachWrappersWorker();
}

#endif // FEATURE_COMINTEROP

void AppDomain::ExceptionUnwind(Frame *pFrame)
{
    CONTRACTL
    {
        DISABLED(GC_TRIGGERS);  // EEResourceException
        DISABLED(THROWS);   // EEResourceException
        MODE_ANY;
    }
    CONTRACTL_END;

    LOG((LF_APPDOMAIN, LL_INFO10, "AppDomain::ExceptionUnwind for %8.8x\n", pFrame));
    Thread *pThread = GetThread();

    LOG((LF_APPDOMAIN, LL_INFO10, "AppDomain::ExceptionUnwind: not first transition or abort\n"));
}


#endif // !DACCESS_COMPILE

#ifndef DACCESS_COMPILE

Assembly* AppDomain::RaiseTypeResolveEventThrowing(Assembly* pAssembly, LPCSTR szName, ASSEMBLYREF *pResultingAssemblyRef)
{
    CONTRACTL
    {
        MODE_ANY;
        GC_TRIGGERS;
        THROWS;
        INJECT_FAULT(COMPlusThrowOM(););
    }
    CONTRACTL_END;

    OVERRIDE_TYPE_LOAD_LEVEL_LIMIT(CLASS_LOADED);

    Assembly* pResolvedAssembly = NULL;
    _ASSERTE(strcmp(szName, g_AppDomainClassName));

    GCX_COOP();

    struct {
        ASSEMBLYREF AssemblyRef;
        STRINGREF str;
    } gc;
    gc.AssemblyRef = NULL;
    gc.str = NULL;

    GCPROTECT_BEGIN(gc);

    if (pAssembly != NULL)
        gc.AssemblyRef = (ASSEMBLYREF)pAssembly->GetExposedObject();

    MethodDescCallSite onTypeResolve(METHOD__ASSEMBLYLOADCONTEXT__ON_TYPE_RESOLVE);

    gc.str = StringObject::NewString(szName);
    ARG_SLOT args[2] =
    {
        ObjToArgSlot(gc.AssemblyRef),
        ObjToArgSlot(gc.str)
    };
    gc.AssemblyRef = (ASSEMBLYREF) onTypeResolve.Call_RetOBJECTREF(args);

    if (gc.AssemblyRef != NULL)
    {
        _ASSERTE(CoreLibBinder::IsClass(gc.AssemblyRef->GetMethodTable(), CLASS__ASSEMBLY));

        pResolvedAssembly = gc.AssemblyRef->GetAssembly();

        if (pResultingAssemblyRef)
            *pResultingAssemblyRef = gc.AssemblyRef;
        else
        {
            if (pResolvedAssembly->IsCollectible())
            {
                COMPlusThrow(kNotSupportedException, W("NotSupported_CollectibleBoundNonCollectible"));
            }
        }
    }
    GCPROTECT_END();

    return pResolvedAssembly;
}


Assembly* AppDomain::RaiseResourceResolveEvent(Assembly* pAssembly, LPCSTR szName)
{
    CONTRACT(Assembly*)
    {
        THROWS;
        GC_TRIGGERS;
        MODE_ANY;
        POSTCONDITION(CheckPointer(RETVAL, NULL_OK));
        INJECT_FAULT(COMPlusThrowOM(););
    }
    CONTRACT_END;

    Assembly* pResolvedAssembly = NULL;

    GCX_COOP();

    struct {
        ASSEMBLYREF AssemblyRef;
        STRINGREF str;
    } gc;
    gc.AssemblyRef = NULL;
    gc.str = NULL;

    GCPROTECT_BEGIN(gc);

    if (pAssembly != NULL)
        gc.AssemblyRef=(ASSEMBLYREF)pAssembly->GetExposedObject();

    MethodDescCallSite onResourceResolve(METHOD__ASSEMBLYLOADCONTEXT__ON_RESOURCE_RESOLVE);
    gc.str = StringObject::NewString(szName);
    ARG_SLOT args[2] =
    {
        ObjToArgSlot(gc.AssemblyRef),
        ObjToArgSlot(gc.str)
    };
    gc.AssemblyRef = (ASSEMBLYREF) onResourceResolve.Call_RetOBJECTREF(args);
    if (gc.AssemblyRef != NULL)
    {
        _ASSERTE(CoreLibBinder::IsClass(gc.AssemblyRef->GetMethodTable(), CLASS__ASSEMBLY));

        pResolvedAssembly = gc.AssemblyRef->GetAssembly();
        if (pResolvedAssembly->IsCollectible())
        {
            COMPlusThrow(kNotSupportedException, W("NotSupported_CollectibleAssemblyResolve"));
        }
    }
    GCPROTECT_END();

    RETURN pResolvedAssembly;
}


Assembly *
AppDomain::RaiseAssemblyResolveEvent(
    AssemblySpec * pSpec)
{
    CONTRACT(Assembly*)
    {
        THROWS;
        GC_TRIGGERS;
        MODE_ANY;
        POSTCONDITION(CheckPointer(RETVAL, NULL_OK));
        INJECT_FAULT(COMPlusThrowOM(););
    }
    CONTRACT_END;

    StackSString ssName;
    pSpec->GetDisplayName(0, ssName);

    // Elevate threads allowed loading level.  This allows the host to load an assembly even in a restricted
    // condition.  Note, however, that this exposes us to possible recursion failures, if the host tries to
    // load the assemblies currently being loaded.  (Such cases would then throw an exception.)

    OVERRIDE_LOAD_LEVEL_LIMIT(FILE_ACTIVE);
    OVERRIDE_TYPE_LOAD_LEVEL_LIMIT(CLASS_LOADED);

    GCX_COOP();

    Assembly* pAssembly = NULL;

    struct {
        ASSEMBLYREF AssemblyRef;
        STRINGREF str;
    } gc;
    gc.AssemblyRef = NULL;
    gc.str = NULL;

    GCPROTECT_BEGIN(gc);
    {
        if (pSpec->GetParentAssembly() != NULL)
        {
            gc.AssemblyRef=(ASSEMBLYREF)pSpec->GetParentAssembly()->GetExposedObject();
        }

        MethodDescCallSite onAssemblyResolve(METHOD__ASSEMBLYLOADCONTEXT__ON_ASSEMBLY_RESOLVE);

        gc.str = StringObject::NewString(ssName);
        ARG_SLOT args[2] = {
            ObjToArgSlot(gc.AssemblyRef),
            ObjToArgSlot(gc.str)
        };

        gc.AssemblyRef = (ASSEMBLYREF) onAssemblyResolve.Call_RetOBJECTREF(args);

        if (gc.AssemblyRef != NULL)
        {
            _ASSERTE(CoreLibBinder::IsClass(gc.AssemblyRef->GetMethodTable(), CLASS__ASSEMBLY));

            pAssembly = gc.AssemblyRef->GetAssembly();
            if (pAssembly->IsCollectible())
            {
                COMPlusThrow(kNotSupportedException, W("NotSupported_CollectibleAssemblyResolve"));
            }
        }
    }
    GCPROTECT_END();

    RETURN pAssembly;
} // AppDomain::RaiseAssemblyResolveEvent

void SystemDomain::ProcessDelayedUnloadLoaderAllocators()
{
    CONTRACTL
    {
        NOTHROW;
        GC_TRIGGERS;
        MODE_COOPERATIVE;
    }
    CONTRACTL_END;

    int iGCRefPoint=GCHeapUtilities::GetGCHeap()->CollectionCount(GCHeapUtilities::GetGCHeap()->GetMaxGeneration());
    if (GCHeapUtilities::GetGCHeap()->IsConcurrentGCInProgress())
        iGCRefPoint--;

    LoaderAllocator * pAllocatorsToDelete = NULL;

    {
        CrstHolder lh(&m_DelayedUnloadCrst);

        LoaderAllocator ** ppAllocator=&m_pDelayedUnloadListOfLoaderAllocators;
        while (*ppAllocator!= NULL)
        {
            LoaderAllocator * pAllocator = *ppAllocator;
            if (0 < iGCRefPoint - pAllocator->GetGCRefPoint())
            {
                *ppAllocator = pAllocator->m_pLoaderAllocatorDestroyNext;

                pAllocator->m_pLoaderAllocatorDestroyNext = pAllocatorsToDelete;
                pAllocatorsToDelete = pAllocator;
            }
            else
            {
                ppAllocator = &pAllocator->m_pLoaderAllocatorDestroyNext;
            }
        }
    }

    // Delete collected loader allocators on the finalizer thread. We cannot offload it to appdomain unload thread because of
    // there is not guaranteed to be one, and it is not that expensive operation anyway.
    while (pAllocatorsToDelete != NULL)
    {
        LoaderAllocator * pAllocator = pAllocatorsToDelete;
        pAllocatorsToDelete = pAllocator->m_pLoaderAllocatorDestroyNext;
        delete pAllocator;
    }
}


void AppDomain::EnumStaticGCRefs(promote_func* fn, ScanContext* sc)
{
    CONTRACT_VOID
    {
        NOTHROW;
        GC_NOTRIGGER;
    }
    CONTRACT_END;

    _ASSERTE(GCHeapUtilities::IsGCInProgress() &&
             GCHeapUtilities::IsServerHeap()   &&
             IsGCSpecialThread());

    if (m_pPinnedHeapHandleTable != nullptr)
    {
        m_pPinnedHeapHandleTable->EnumStaticGCRefs(fn, sc);
    }

    RETURN;
}

#endif // !DACCESS_COMPILE

//------------------------------------------------------------------------
PTR_LoaderAllocator AppDomain::GetLoaderAllocator()
{
    WRAPPER_NO_CONTRACT;
    return SystemDomain::GetGlobalLoaderAllocator(); // The one and only domain is not unloadable
}

//------------------------------------------------------------------------
UINT32 AppDomain::GetTypeID(PTR_MethodTable pMT) {
    CONTRACTL {
        THROWS;
        GC_TRIGGERS;
    } CONTRACTL_END;

    return m_typeIDMap.GetTypeID(pMT, true);
}

//------------------------------------------------------------------------
// Returns the ID of the type if found. If not found, returns INVALID_TYPE_ID
UINT32 AppDomain::LookupTypeID(PTR_MethodTable pMT)
{
    CONTRACTL {
        NOTHROW;
        WRAPPER(GC_TRIGGERS);
    } CONTRACTL_END;

    return m_typeIDMap.LookupTypeID(pMT);
}

//------------------------------------------------------------------------
PTR_MethodTable AppDomain::LookupType(UINT32 id) {
    CONTRACTL {
        NOTHROW;
        WRAPPER(GC_TRIGGERS);
        CONSISTENCY_CHECK(id != TYPE_ID_THIS_CLASS);
    } CONTRACTL_END;

    PTR_MethodTable pMT = m_typeIDMap.LookupType(id);

    CONSISTENCY_CHECK(CheckPointer(pMT));
    CONSISTENCY_CHECK(pMT->IsInterface());
    return pMT;
}

#ifndef DACCESS_COMPILE
//---------------------------------------------------------------------------------------
void AppDomain::RemoveTypesFromTypeIDMap(LoaderAllocator* pLoaderAllocator)
{
    CONTRACTL {
        NOTHROW;
        GC_NOTRIGGER;
    } CONTRACTL_END;

    m_typeIDMap.RemoveTypes(pLoaderAllocator);
}
#endif // DACCESS_COMPILE

//---------------------------------------------------------------------------------------
//
BOOL
AppDomain::AssemblyIterator::Next(
    CollectibleAssemblyHolder<Assembly *> * pAssemblyHolder)
{
    CONTRACTL {
        NOTHROW;
        WRAPPER(GC_TRIGGERS); // Triggers only in MODE_COOPERATIVE (by taking the lock)
        MODE_ANY;
    } CONTRACTL_END;

    CrstHolder ch(m_pAppDomain->GetAssemblyListLock());
    return Next_Unlocked(pAssemblyHolder);
}

//---------------------------------------------------------------------------------------
//
// Note: Does not lock the assembly list, but locks collectible assemblies for adding references.
//
BOOL
AppDomain::AssemblyIterator::Next_Unlocked(
    CollectibleAssemblyHolder<Assembly *> * pAssemblyHolder)
{
    CONTRACTL {
        NOTHROW;
        GC_NOTRIGGER;
        MODE_ANY;
    } CONTRACTL_END;

#ifndef DACCESS_COMPILE
    _ASSERTE(m_pAppDomain->GetAssemblyListLock()->OwnedByCurrentThread());
#endif

    while (m_Iterator.Next())
    {
        // Get element from the list/iterator (without adding reference to the assembly)
        DomainAssembly * pDomainAssembly = dac_cast<PTR_DomainAssembly>(m_Iterator.GetElement());
        if (pDomainAssembly == NULL)
        {
            continue;
        }

        Assembly* pAssembly = pDomainAssembly->GetAssembly();
        if (pAssembly->IsError())
        {
            if (m_assemblyIterationFlags & kIncludeFailedToLoad)
            {
                *pAssemblyHolder = pAssembly;
                return TRUE;
            }
            continue; // reject
        }

        // First, reject assemblies whose load status is not to be included in
        // the enumeration

        if (pAssembly->IsAvailableToProfilers() &&
            (m_assemblyIterationFlags & kIncludeAvailableToProfilers))
        {
            // The assembly has reached the state at which we would notify profilers,
            // and we're supposed to include such assemblies in the enumeration. So
            // don't reject it (i.e., noop here, and don't bother with the rest of
            // the load status checks). Check for this first, since
            // kIncludeAvailableToProfilers contains some loaded AND loading
            // assemblies.
        }
        else if (pAssembly->IsLoaded())
        {
            // A loaded assembly
            if (!(m_assemblyIterationFlags & kIncludeLoaded))
            {
                continue; // reject
            }
        }
        else
        {
            // A loading assembly
            if (!(m_assemblyIterationFlags & kIncludeLoading))
            {
                continue; // reject
            }
        }

        // Next, reject assemblies whose execution status is
        // not to be included in the enumeration

        // execution assembly
        if (!(m_assemblyIterationFlags & kIncludeExecution))
        {
            continue; // reject
        }

        // Next, reject collectible assemblies
        if (pAssembly->IsCollectible())
        {
            if (m_assemblyIterationFlags & kExcludeCollectible)
            {
                _ASSERTE(!(m_assemblyIterationFlags & kIncludeCollected));
                continue; // reject
            }

            // Un-tenured collectible assemblies should not be returned. (This can only happen in a brief
            // window during collectible assembly creation. No thread should need to have a pointer
            // to the just allocated DomainAssembly at this stage.)
            if (!pAssembly->GetModule()->IsTenured())
            {
                continue; // reject
            }

            if (pAssembly->GetLoaderAllocator()->AddReferenceIfAlive())
            {   // The assembly is alive

                // Set the holder value (incl. increasing ref-count)
                *pAssemblyHolder = pAssembly;

                // Now release the reference we took in the if-condition
                pAssembly->GetLoaderAllocator()->Release();
                return TRUE;
            }
            // The assembly is not alive anymore (and we didn't increase its ref-count in the
            // if-condition)

            if (!(m_assemblyIterationFlags & kIncludeCollected))
            {
                continue; // reject
            }
            // Set the holder value to assembly with 0 ref-count without increasing the ref-count (won't
            // call Release either)
            pAssemblyHolder->Assign(pAssembly, FALSE);
            return TRUE;
        }

        *pAssemblyHolder = pAssembly;
        return TRUE;
    }

    *pAssemblyHolder = NULL;
    return FALSE;
} // AppDomain::AssemblyIterator::Next_Unlocked

#if !defined(DACCESS_COMPILE)

// Returns S_OK if the assembly was successfully loaded
HRESULT RuntimeInvokeHostAssemblyResolver(INT_PTR pAssemblyLoadContextToBindWithin, BINDER_SPACE::AssemblyName *pAssemblyName, DefaultAssemblyBinder *pDefaultBinder, AssemblyBinder *pBinder, BINDER_SPACE::Assembly **ppLoadedAssembly)
{
    CONTRACTL
    {
        THROWS;
        GC_TRIGGERS;
        MODE_ANY;
        PRECONDITION(pAssemblyName != NULL);
        PRECONDITION(ppLoadedAssembly != NULL);
    }
    CONTRACTL_END;

    HRESULT hr = E_FAIL;

    // Switch to COOP mode since we are going to work with managed references
    GCX_COOP();

    struct
    {
        ASSEMBLYNAMEREF oRefAssemblyName;
        ASSEMBLYREF oRefLoadedAssembly;
    } _gcRefs;

    ZeroMemory(&_gcRefs, sizeof(_gcRefs));

    GCPROTECT_BEGIN(_gcRefs);

    BINDER_SPACE::Assembly *pResolvedAssembly = NULL;

    bool fResolvedAssembly = false;
    BinderTracing::ResolutionAttemptedOperation tracer{pAssemblyName, 0 /*binderID*/, pAssemblyLoadContextToBindWithin, hr};

    // Allocate an AssemblyName managed object
    _gcRefs.oRefAssemblyName = (ASSEMBLYNAMEREF) AllocateObject(CoreLibBinder::GetClass(CLASS__ASSEMBLY_NAME));

    // Initialize the AssemblyName object
    AssemblySpec::InitializeAssemblyNameRef(pAssemblyName, &_gcRefs.oRefAssemblyName);

    bool isSatelliteAssemblyRequest = !pAssemblyName->IsNeutralCulture();

    EX_TRY
    {
        if (pDefaultBinder != NULL)
        {
            // Step 2 (of CustomAssemblyBinder::BindAssemblyByName) - Invoke Load method
            // This is not invoked for TPA Binder since it always returns NULL.
            tracer.GoToStage(BinderTracing::ResolutionAttemptedOperation::Stage::AssemblyLoadContextLoad);

            // Finally, setup arguments for invocation
            MethodDescCallSite methLoadAssembly(METHOD__ASSEMBLYLOADCONTEXT__RESOLVE);

            // Setup the arguments for the call
            ARG_SLOT args[2] =
            {
                PtrToArgSlot(pAssemblyLoadContextToBindWithin), // IntPtr for managed assembly load context instance
                ObjToArgSlot(_gcRefs.oRefAssemblyName), // AssemblyName instance
            };

            // Make the call
            _gcRefs.oRefLoadedAssembly = (ASSEMBLYREF) methLoadAssembly.Call_RetOBJECTREF(args);
            if (_gcRefs.oRefLoadedAssembly != NULL)
            {
                fResolvedAssembly = true;
            }

            hr = fResolvedAssembly ? S_OK : COR_E_FILENOTFOUND;

            // Step 3 (of CustomAssemblyBinder::BindAssemblyByName)
            if (!fResolvedAssembly && !isSatelliteAssemblyRequest)
            {
                tracer.GoToStage(BinderTracing::ResolutionAttemptedOperation::Stage::DefaultAssemblyLoadContextFallback);

                // If we could not resolve the assembly using Load method, then attempt fallback with TPA Binder.
                // Since TPA binder cannot fallback to itself, this fallback does not happen for binds within TPA binder.
                //
                // Switch to pre-emp mode before calling into the binder
                GCX_PREEMP();
                BINDER_SPACE::Assembly *pCoreCLRFoundAssembly = NULL;
                hr = pDefaultBinder->BindUsingAssemblyName(pAssemblyName, &pCoreCLRFoundAssembly);
                if (SUCCEEDED(hr))
                {
                    _ASSERTE(pCoreCLRFoundAssembly != NULL);
                    pResolvedAssembly = pCoreCLRFoundAssembly;
                    fResolvedAssembly = true;
                }
            }
        }

        if (!fResolvedAssembly && isSatelliteAssemblyRequest)
        {
            // Step 4 (of CustomAssemblyBinder::BindAssemblyByName)
            //
            // Attempt to resolve it using the ResolveSatelliteAssembly method.
            // Finally, setup arguments for invocation
            tracer.GoToStage(BinderTracing::ResolutionAttemptedOperation::Stage::ResolveSatelliteAssembly);

            MethodDescCallSite methResolveSateliteAssembly(METHOD__ASSEMBLYLOADCONTEXT__RESOLVESATELLITEASSEMBLY);

            // Setup the arguments for the call
            ARG_SLOT args[2] =
            {
                PtrToArgSlot(pAssemblyLoadContextToBindWithin), // IntPtr for managed assembly load context instance
                ObjToArgSlot(_gcRefs.oRefAssemblyName), // AssemblyName instance
            };

            // Make the call
            _gcRefs.oRefLoadedAssembly = (ASSEMBLYREF) methResolveSateliteAssembly.Call_RetOBJECTREF(args);
            if (_gcRefs.oRefLoadedAssembly != NULL)
            {
                // Set the flag indicating we found the assembly
                fResolvedAssembly = true;
            }

            hr = fResolvedAssembly ? S_OK : COR_E_FILENOTFOUND;
        }

        if (!fResolvedAssembly)
        {
            // Step 5 (of CustomAssemblyBinder::BindAssemblyByName)
            //
            // If we couldn't resolve the assembly using TPA LoadContext as well, then
            // attempt to resolve it using the Resolving event.
            // Finally, setup arguments for invocation
            tracer.GoToStage(BinderTracing::ResolutionAttemptedOperation::Stage::AssemblyLoadContextResolvingEvent);

            MethodDescCallSite methResolveUsingEvent(METHOD__ASSEMBLYLOADCONTEXT__RESOLVEUSINGEVENT);

            // Setup the arguments for the call
            ARG_SLOT args[2] =
            {
                PtrToArgSlot(pAssemblyLoadContextToBindWithin), // IntPtr for managed assembly load context instance
                ObjToArgSlot(_gcRefs.oRefAssemblyName), // AssemblyName instance
            };

            // Make the call
            _gcRefs.oRefLoadedAssembly = (ASSEMBLYREF) methResolveUsingEvent.Call_RetOBJECTREF(args);
            if (_gcRefs.oRefLoadedAssembly != NULL)
            {
                // Set the flag indicating we found the assembly
                fResolvedAssembly = true;
            }

            hr = fResolvedAssembly ? S_OK : COR_E_FILENOTFOUND;
        }

        if (fResolvedAssembly && pResolvedAssembly == NULL)
        {
            // If we are here, assembly was successfully resolved via Load or Resolving events.
            _ASSERTE(_gcRefs.oRefLoadedAssembly != NULL);

            _ASSERTE(CoreLibBinder::IsClass(_gcRefs.oRefLoadedAssembly->GetMethodTable(), CLASS__ASSEMBLY));

            // We were able to get the assembly loaded. Now, get its name since the host could have
            // performed the resolution using an assembly with different name.
            Assembly *pAssembly = _gcRefs.oRefLoadedAssembly->GetAssembly();
            _ASSERTE(pAssembly != NULL);

            // Disallow reflection emitted assemblies returned in assembly resolution extension points
            if (pAssembly->IsDynamic())
            {
                PathString name;
                pAssemblyName->GetDisplayName(name, BINDER_SPACE::AssemblyName::INCLUDE_ALL);
                COMPlusThrowHR(COR_E_INVALIDOPERATION, IDS_HOST_ASSEMBLY_RESOLVER_DYNAMICALLY_EMITTED_ASSEMBLIES_UNSUPPORTED, name);
            }

            // For collectible assemblies, ensure that the parent loader allocator keeps the assembly's loader allocator
            // alive for all its lifetime.
            if (pAssembly->IsCollectible())
            {
                LoaderAllocator *pResultAssemblyLoaderAllocator = pAssembly->GetLoaderAllocator();
                LoaderAllocator *pParentLoaderAllocator = pBinder->GetLoaderAllocator();
                if (pParentLoaderAllocator == NULL)
                {
                    // The AssemblyLoadContext for which we are resolving the Assembly is not collectible.
                    COMPlusThrow(kNotSupportedException, W("NotSupported_CollectibleBoundNonCollectible"));
                }

                _ASSERTE(pResultAssemblyLoaderAllocator);
                pParentLoaderAllocator->EnsureReference(pResultAssemblyLoaderAllocator);
            }

            pResolvedAssembly = pAssembly->GetPEAssembly()->GetHostAssembly();
        }

        if (fResolvedAssembly)
        {
            _ASSERTE(pResolvedAssembly != NULL);

            // Get the BINDER_SPACE::Assembly reference to return back to.
            *ppLoadedAssembly = clr::SafeAddRef(pResolvedAssembly);
            hr = S_OK;

            tracer.SetFoundAssembly(static_cast<BINDER_SPACE::Assembly *>(pResolvedAssembly));
        }
        else
        {
            hr = COR_E_FILENOTFOUND;
        }
    }
    EX_HOOK
    {
        Exception* ex = GET_EXCEPTION();
        tracer.SetException(ex);
    }
    EX_END_HOOK

    GCPROTECT_END();

    return hr;
}
#endif // !defined(DACCESS_COMPILE)

#ifdef DACCESS_COMPILE

void
AppDomain::EnumMemoryRegions(CLRDataEnumMemoryFlags flags, bool enumThis)
{
    SUPPORTS_DAC;

    if (enumThis)
    {
        //sizeof(AppDomain) == 0xeb0
        DAC_ENUM_DTHIS();
        EMEM_OUT(("MEM: %p AppDomain\n", dac_cast<TADDR>(this)));
    }

    // We don't need AppDomain name in triage dumps.
    if (flags != CLRDATA_ENUM_MEM_TRIAGE)
    {
        m_friendlyName.EnumMem();
    }

    if (flags == CLRDATA_ENUM_MEM_HEAP2)
    {
        GetLoaderAllocator()->EnumMemoryRegions(flags);
    }

    m_Assemblies.EnumMemoryRegions(flags);
    AssemblyIterator assem = IterateAssembliesEx((AssemblyIterationFlags)(kIncludeLoaded | kIncludeExecution));
    CollectibleAssemblyHolder<Assembly *> pAssembly;

    while (assem.Next(pAssembly.This()))
    {
        pAssembly->EnumMemoryRegions(flags);
    }
}

void
SystemDomain::EnumMemoryRegions(CLRDataEnumMemoryFlags flags, bool enumThis)
{
    SUPPORTS_DAC;
    if (enumThis)
    {
        DAC_ENUM_DTHIS();
        EMEM_OUT(("MEM: %p SystemAppomain\n", dac_cast<TADDR>(this)));
    }

    if (flags == CLRDATA_ENUM_MEM_HEAP2)
    {
        GetGlobalLoaderAllocator()->EnumMemoryRegions(flags);
    }
    if (m_pSystemPEAssembly.IsValid())
    {
        m_pSystemPEAssembly->EnumMemoryRegions(flags);
    }
    if (m_pSystemAssembly.IsValid())
    {
        m_pSystemAssembly->EnumMemoryRegions(flags);
    }
    if (AppDomain::GetCurrentDomain())
    {
        AppDomain::GetCurrentDomain()->EnumMemoryRegions(flags, true);
    }
}

#endif //DACCESS_COMPILE


PTR_LoaderAllocator SystemDomain::GetGlobalLoaderAllocator()
{
    return PTR_LoaderAllocator(PTR_HOST_MEMBER_TADDR(SystemDomain,System(),m_GlobalAllocator));
}

#if defined(FEATURE_TYPEEQUIVALENCE)

#ifndef DACCESS_COMPILE
TypeEquivalenceHashTable * AppDomain::GetTypeEquivalenceCache()
{
    CONTRACTL
    {
        THROWS;
        GC_TRIGGERS;
        INJECT_FAULT(COMPlusThrowOM());
        MODE_ANY;
    }
    CONTRACTL_END;

    // Take the critical section all of the time in debug builds to ensure that it is safe to take
    // the critical section in the unusual times when it may actually be needed in retail builds
#ifdef _DEBUG
    CrstHolder ch(&m_TypeEquivalenceCrst);
#endif

    if (m_pTypeEquivalenceTable.Load() == NULL)
    {
#ifndef _DEBUG
        CrstHolder ch(&m_TypeEquivalenceCrst);
#endif
        if (m_pTypeEquivalenceTable.Load() == NULL)
        {
            m_pTypeEquivalenceTable = TypeEquivalenceHashTable::Create(this, /* bucket count */ 12, &m_TypeEquivalenceCrst);
        }
    }
    return m_pTypeEquivalenceTable;
}
#endif //!DACCESS_COMPILE

#endif //FEATURE_TYPEEQUIVALENCE

#ifndef DACCESS_COMPILE
// Return native image for a given composite image file name, NULL when not found.
PTR_NativeImage AppDomain::GetNativeImage(LPCUTF8 simpleFileName)
{
    CrstHolder ch(&m_nativeImageLoadCrst);
    PTR_NativeImage pExistingImage;
    if (m_nativeImageMap.Lookup(simpleFileName, &pExistingImage))
    {
        return pExistingImage;
    }
    return nullptr;
}

PTR_NativeImage AppDomain::SetNativeImage(LPCUTF8 simpleFileName, PTR_NativeImage pNativeImage)
{
    CrstHolder ch(&m_nativeImageLoadCrst);
    PTR_NativeImage pExistingImage;
    if (m_nativeImageMap.Lookup(simpleFileName, &pExistingImage))
    {
        return pExistingImage;
    }
    m_nativeImageMap.Add(simpleFileName, pNativeImage);
    return nullptr;
}
#endif//DACCESS_COMPILE
