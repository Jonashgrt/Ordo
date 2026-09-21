#pragma once

#include <cstdarg>

#include <Cor/Defines.hh>
#include <Cor/BaseTypes.hh>
#include <Cor/AtomicTypes.hh>
#include <Cor/Allocator.hh>

#include <Ordo/Fwd.hh>
#include <Ordo/Job.hh>
#include <Ordo/Enums.hh>
#include <Ordo/Scheduler.hh>

#include "LeanWin32.hh"

namespace ordo
{
	struct FWorker;
	struct FFiber;
	struct FAtomicCounterState;

	enum class EYieldReason : U32
	{
		None = 0,
		Completed,
		Parked
	};

	enum class EFiberState : I32
	{
		Idle = 0,
		Running,
		Parking,
		Parked,
		Ready
	};

	struct FTaskPacket
	{
		FJob Job;
		FAtomicCounterState* pCompletionCounter;
	};

	union UQueueValue
	{
		FTaskPacket Task;
		FFiber* pFiber;
		uintptr_t Bits;
	};

	// Data layout based on Dmitry Vyukov's bounded MPMC queue.
	// See THIRD_PARTY_NOTICES.md for copyright and license terms.
	struct FMPMCCell
	{
		cor::FAtomic64 Sequence;
		UQueueValue Value;
	};

	struct FMPMCQueue
	{
		FMPMCCell* pCells;
		cor::FAllocator* pAllocator;
		U64 Mask;
		COR_ALIGNAS(COR_CACHE_LINE_SIZE) cor::FAtomic64 EnqueuePosition;
		COR_ALIGNAS(COR_CACHE_LINE_SIZE) cor::FAtomic64 DequeuePosition;
	};

	struct FWorkDeque
	{
		FTaskPacket* pItems;
		cor::FAllocator* pAllocator;
		U64 Mask;
		COR_ALIGNAS(COR_CACHE_LINE_SIZE) cor::FAtomic64 Top;
		COR_ALIGNAS(COR_CACHE_LINE_SIZE) cor::FAtomic64 Bottom;
	};

	struct COR_ALIGNAS(COR_CACHE_LINE_SIZE) FFiber
	{
		SLIST_ENTRY FreeEntry;
		void* pNativeFiber;
		FWorker* pWorker;
		FFiber* pWaitNext;
		FFiber* pReadyNext;
		FTaskPacket Task;
		cor::FAtomic32 State;
#if ORDO_DEBUG
		cor::FAtomic32 RunningThreadId;
#endif
		EYieldReason YieldReason;
		ESuspendReason SuspendReason;
		U32 Index;
		U32 PinnedWorkerIndex;
	};

	struct COR_ALIGNAS(COR_CACHE_LINE_SIZE) FWorker
	{
		HANDLE hThread;
		DWORD ThreadId;
		void* pDispatcherFiber;
		DWORD_PTR PreviousAffinityMask;
		bool bConvertedThreadToFiber;
		FWorkDeque LocalQueues[U32(EPriority::Count)];
		FMPMCQueue InjectionQueues[U32(EPriority::Count)];

		// When current worker wakes suspended fiber, ScheduleReadyFiber() places it into its private queue
		// Pinned fibers awakened by another worker instead use locked pinned queue
		FFiber* pOwnedReadyHead;
		FFiber* pOwnedReadyTail;
		U32 NumOwnedReadyFibers;

		// Pending, but not started since no unused fiber stack available
		// Prevents perpetual dequeueing during fber starvation
		FTaskPacket PendingTask;
		bool bHasPendingTask;

		COR_ALIGNAS(COR_CACHE_LINE_SIZE) SRWLOCK PinnedReadyLock;
		FFiber* pPinnedReadyHead;
		FFiber* pPinnedReadyTail;
		cor::FAtomic32 NumPinnedReadyFibers;

		FAtomicCounterState* pDeferredCompletionCounter;
		U32 NumDeferredCounterCompletions;
		U32 NumUnflushedCompletions;
		U32 Index;
		U32 StealState;
	};

	struct FAtomicCounterState
	{
		cor::FAtomic32 Value;
		cor::FAtomic32 ActiveWaiters;
		cor::FAtomic32 NumMainThreadWaiters;
		SRWLOCK WaitLock;
		CONDITION_VARIABLE ExternalCondition;
		FFiber* pWaitHead;
		FFiber* pWaitTail;
	};

	struct FMutexState
	{
		cor::FAtomic32 bLocked;
		cor::FAtomic32 NumExternalWaiters;
		cor::FAtomic32 NumMainThreadWaiters;
		SRWLOCK WaitLock;
		CONDITION_VARIABLE ExternalCondition;
		FFiber* pWaitHead;
		FFiber* pWaitTail;
	};

	struct COR_ALIGNAS(COR_CACHE_LINE_SIZE) FScheduler
	{
		FSchedulerDesc Desc;
		FWorker* pWorkers;
		FFiber* pFibers;
		U32 NumWorkerThreads;
		U32 NumFibers;
		U32 NumStartedWorkers;
		U32 NumCreatedThreads;

		COR_ALIGNAS(COR_CACHE_LINE_SIZE) cor::FAtomic32 bAcceptingJobs;
		COR_ALIGNAS(COR_CACHE_LINE_SIZE) cor::FAtomic32 bStopRequested;
		COR_ALIGNAS(COR_CACHE_LINE_SIZE) cor::FAtomic32 NumReadyWorkers;
										 cor::FAtomic32 NumWorkerStartFailures;
		COR_ALIGNAS(COR_CACHE_LINE_SIZE) cor::FAtomic32 NumLiveSyncObjects;
		COR_ALIGNAS(COR_CACHE_LINE_SIZE) cor::FAtomic32 NumGlobalReadyFibers;
		COR_ALIGNAS(COR_CACHE_LINE_SIZE) cor::FAtomic64 NumOutstandingJobs;

		COR_ALIGNAS(COR_CACHE_LINE_SIZE) cor::FAtomic64 WorkEpoch;
		COR_ALIGNAS(COR_CACHE_LINE_SIZE) SLIST_HEADER FreeFibers;
		FMPMCQueue ReadyFibers;
		COR_ALIGNAS(COR_CACHE_LINE_SIZE) cor::FAtomic32 NextInjectionWorker;
		HANDLE hIdleEvent;
		HANDLE hWorkersReadyEvent;
	};
}

static_assert
(
	sizeof(ordo::FAtomicCounterState) <= ORDO_SYNC_STORAGE_SIZE,
	"Increase ORDO_SYNC_STORAGE_SIZE for FAtomicCounterState"
);