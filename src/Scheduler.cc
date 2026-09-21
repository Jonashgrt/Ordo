#include <Ordo/Scheduler.hh>

#include <Cor/Defines.hh>
#include <Cor/Bit.hh>
#include <Cor/Atomic.hh>
#include <Cor/Assert.hh>
#include <Cor/Allocator.hh>
#include <Cor/PRNG.hh>

#include "Internal/Defines.hh"
#include "Internal/Types.hh"
#include "Internal/LeanWin32.hh"
#include "Internal/Queues.hh"
#include "Internal/AtomicCounterInternal.hh"

namespace ordo
{
	FScheduler gScheduler = { };

	COR_THREAD_LOCAL FWorker* gpCurrentWorker = nullptr;
	COR_THREAD_LOCAL FFiber* gpCurrentFiber = nullptr;

	void NotifyWorkers()
	{
		cor::IncrementAtomic(&gScheduler.WorkEpoch);
		WakeByAddressAll(&gScheduler.WorkEpoch.Value);
	}


	DWORD_PTR PinWorkerToProcessor(FWorker const* pWorker)
	{
		DWORD const NumProcessors = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
		U32 NumMaskBits = U32(sizeof(DWORD_PTR) * CHAR_BIT);
		if (!(NumProcessors > 0u && NumProcessors <= NumMaskBits))
			return 0u;

		DWORD_PTR Mask = DWORD_PTR{1} << (pWorker->Index % NumProcessors);
		return SetThreadAffinityMask(GetCurrentThread(), Mask);
	}

	bool AttachCurrentThreadAsWorker(FWorker* pWorker)
	{
		COR_ASSERT(pWorker);
		COR_ASSERT(!gpCurrentWorker);
		COR_ASSERT(!gpCurrentFiber);

		gpCurrentWorker = pWorker;
		pWorker->ThreadId = GetCurrentThreadId();
		if (gScheduler.Desc.bPinWorkersToLogicalProcessors)
			pWorker->PreviousAffinityMask = PinWorkerToProcessor(pWorker);

		if (IsThreadAFiber())
		{
			pWorker->pDispatcherFiber = GetCurrentFiber();
		}
		else
		{
			pWorker->pDispatcherFiber = ConvertThreadToFiberEx
			(
				pWorker, 
				FIBER_FLAG_FLOAT_SWITCH
			);
			pWorker->bConvertedThreadToFiber = pWorker->pDispatcherFiber != nullptr;
		}

		if (pWorker->pDispatcherFiber == nullptr)
			cor::IncrementAtomic(&gScheduler.NumWorkerStartFailures);
		else if (gScheduler.Desc.Callbacks.pfnWorkerStarted)
		{
			gScheduler.Desc.Callbacks.pfnWorkerStarted
			(
				gScheduler.Desc.Callbacks.pContext,
				pWorker->Index
			);
		}

		if (U32
		(
			cor::IncrementAtomic(&gScheduler.NumReadyWorkers)
		) == gScheduler.NumWorkerThreads)
		{
			SetEvent(gScheduler.hWorkersReadyEvent);
		}

		if (pWorker->pDispatcherFiber)
			return true;

		// Clean-up on failure
		if (pWorker->PreviousAffinityMask != 0u)
		{
			COR_UNUSED(SetThreadAffinityMask
			(
				GetCurrentThread(),
				pWorker->PreviousAffinityMask
			));
			pWorker->PreviousAffinityMask = 0u;
		}
		gpCurrentWorker = nullptr;
		return false;
	}

	void DetachCurrentThreadFromWorker(FWorker* pWorker)
	{
		COR_ASSERT(pWorker);
		COR_ASSERT(gpCurrentWorker == pWorker);
		COR_ASSERT(!gpCurrentFiber);
		COR_ASSERT(GetCurrentThreadId() == pWorker->ThreadId);
		COR_ASSERT(!IsThreadAFiber() || GetCurrentFiber() == pWorker->pDispatcherFiber);

		if (gScheduler.Desc.Callbacks.pfnWorkerStopped)
		{
			gScheduler.Desc.Callbacks.pfnWorkerStopped
			(
				gScheduler.Desc.Callbacks.pContext,
				pWorker->Index
			);
		}

		if (pWorker->bConvertedThreadToFiber)
		{
			BOOL bConverted = ConvertFiberToThread();
			COR_ASSERT(bConverted);
			pWorker->bConvertedThreadToFiber = false;
		}
		pWorker->pDispatcherFiber = nullptr;

		if (pWorker->PreviousAffinityMask)
		{
			SetThreadAffinityMask
			(
				GetCurrentThread(),
				pWorker->PreviousAffinityMask
			);
			pWorker->PreviousAffinityMask = 0;
		}
		gpCurrentWorker = nullptr;
	}

	void QueuePinnedFiber(FWorker* pWorker, FFiber* pFiber)
	{
		pFiber->pReadyNext = nullptr;
		AcquireSRWLockExclusive(&pWorker->PinnedReadyLock);
		if (pWorker->pPinnedReadyTail != nullptr)
			pWorker->pPinnedReadyTail->pReadyNext = pFiber;
		else
			pWorker->pPinnedReadyHead = pFiber;
		pWorker->pPinnedReadyTail = pFiber;
		cor::IncrementAtomic(&pWorker->NumPinnedReadyFibers);
		ReleaseSRWLockExclusive(&pWorker->PinnedReadyLock);
	}

	void QueueOwnedReadyFiber(FWorker* pWorker, FFiber* pFiber)
	{
		pFiber->pReadyNext = nullptr;
		if (pWorker->pOwnedReadyTail != nullptr)
			pWorker->pOwnedReadyTail->pReadyNext = pFiber;
		else
			pWorker->pOwnedReadyHead = pFiber;
		pWorker->pOwnedReadyTail = pFiber;
		++pWorker->NumOwnedReadyFibers;
	}

	FFiber* PopOwnedReadyFiber(FWorker* pWorker)
	{
		FFiber* pFiber = pWorker->pOwnedReadyHead;
		if (pFiber)
		{
			pWorker->pOwnedReadyHead = pFiber->pReadyNext;
			if (!pWorker->pOwnedReadyHead)
				pWorker->pOwnedReadyTail = nullptr;
			pFiber->pReadyNext = nullptr;
			--pWorker->NumOwnedReadyFibers;
		}
		return pFiber;
	}

	void ScheduleReadyFiber(FFiber* pFiber)
	{
		if (pFiber->PinnedWorkerIndex != ORDO_INVALID_INDEX)
		{
			// Fiber is pinned
			FWorker* pPinnedWorker = &gScheduler.pWorkers[pFiber->PinnedWorkerIndex];
			if (pPinnedWorker == gpCurrentWorker) 
			{
				// If we are the pinned worker, queue unlocked in owned ready queue
				QueueOwnedReadyFiber(pPinnedWorker, pFiber);
			}
			else
			{
				// Otherwise, enqueue in pinned queue of pinned worker
				QueuePinnedFiber(pPinnedWorker, pFiber);
			}
		}
		else if (gpCurrentWorker)
		{
			// Unpinned fiber, this thread is a worker, put in own ready queue
			QueueOwnedReadyFiber(gpCurrentWorker, pFiber);
		}
		else
		{
			// Unpinned fiber, woken externally, put into global MPMC queue
			UQueueValue uQueueValue;
			uQueueValue.pFiber = pFiber;
			cor::IncrementAtomic(&gScheduler.NumGlobalReadyFibers);
			while (!EnqueueMPMC(&gScheduler.ReadyFibers, uQueueValue))
				YieldProcessor();
		}

		if (gScheduler.Desc.IdleBehavior == EIdleBehavior::Sleep)
			NotifyWorkers();
	}

	void WakeFiber(FFiber* pFiber)
	{
		for (;;)
		{
			I32 State = cor::LoadAtomic(&pFiber->State);
			if (State == I32(EFiberState::Parking))
			{
				if (cor::CompareExchangeAtomic
					(
						&pFiber->State,
						I32(EFiberState::Ready),
						I32(EFiberState::Parking)
					) == I32(EFiberState::Parking))
				{
					// Fiber registered as waiter, but stack (i.e., switch to dispatcher) may still be executing
					// Dispatcher will observe Ready and enqueue
					return;
				}
			}
			else if (State == I32(EFiberState::Parked))
			{
				if (cor::CompareExchangeAtomic
					(
						&pFiber->State,
						I32(EFiberState::Ready),
						I32(EFiberState::Parked)
					) == I32(EFiberState::Parked))
				{
					ScheduleReadyFiber(pFiber);
					return;
				}
			}
			else
			{
				COR_ASSERT(false && "A fiber was readied more than once!");
				return;
			}
			YieldProcessor();
		}
	}

	void ParkCurrentFiber(ESuspendReason Reason)
	{
		FFiber* pFiber = gpCurrentFiber;
		FWorker* pWorker = gpCurrentWorker;
		COR_ASSERT(pFiber && pWorker);

#if ORDO_DEBUG
		I32 State = cor::LoadAtomic(&pFiber->State);
		COR_ASSERT(State == I32(EFiberState::Parking) || State == I32(EFiberState::Ready));
#endif

		pFiber->SuspendReason = Reason;
		pFiber->YieldReason = EYieldReason::Parked;

		FCallbacks const* pCallbacks = &gScheduler.Desc.Callbacks;
		if (pCallbacks->pfnFiberSuspended)
		{
			pCallbacks->pfnFiberSuspended
			(
				pCallbacks->pContext,
				pFiber->Index,
				pWorker->Index,
				Reason
			);
		}
		
		SwitchToFiber(pWorker->pDispatcherFiber);
		COR_ASSERT(cor::LoadAtomic(&pFiber->State) == I32(EFiberState::Running));
	}

	bool IsMainThreadDispatcher()
	{
		return !gpCurrentFiber 
			&& gpCurrentWorker == &gScheduler.pWorkers[0]
			&& gpCurrentWorker->Index == 0u
			&& gpCurrentWorker->ThreadId == GetCurrentThreadId()
			&& IsThreadAFiber()
			&& GetCurrentFiber() == gpCurrentWorker->pDispatcherFiber;
	}

	void ReleaseAtomicCounterWaitersLocked
	(
		FAtomicCounterState* pCounter)
	{
		// Counters wait lock is acquired throughout execution of this function
		// Counter must have finished for waiter release
		COR_ASSERT(cor::LoadAtomic(&pCounter->Value) == 0);

		FFiber* pWaiters = pCounter->pWaitHead;
		pCounter->pWaitHead = nullptr;
		pCounter->pWaitTail = nullptr;

		bool bWakeMainThread = cor::LoadAtomic(&pCounter->NumMainThreadWaiters) != 0;
		WakeAllConditionVariable(&pCounter->ExternalCondition);

		if (bWakeMainThread && gScheduler.Desc.IdleBehavior == EIdleBehavior::Sleep)
			NotifyWorkers();
		
		// Wake all waiters 
		while (pWaiters)
		{
			FFiber* pNext = pWaiters->pWaitNext;
			pWaiters->pWaitNext = nullptr;
			WakeFiber(pWaiters);
			pWaiters = pNext;
		}
	}

	void CompleteAtomicCounterBatch
	(
		FAtomicCounterState* pCounter,
		U32 NumCompletions)
	{
		// Number of completions on this counter by calling worker
		I32 Delta = I32(NumCompletions);
		for (;;)
		{
			I32 OldCounterValue = cor::LoadAtomic(&pCounter->Value);
			// Must be leq than remaining counter value
			COR_ASSERT(OldCounterValue >= Delta);
			// This worker finished the batch
			if (OldCounterValue == Delta)
			{
				// See AddAtomicCounterState:
				// Counter finish and waiter release form crit sec
				// on WaitLock so a completed phase cannot be destroyed 
				// and reused beneath its releaser
				AcquireSRWLockExclusive(&pCounter->WaitLock);
				// Load again, since while we waited, more work on this counter could have been submitted
				OldCounterValue = cor::LoadAtomic(&pCounter->Value);
				COR_ASSERT(OldCounterValue >= Delta);
				if (OldCounterValue != Delta)
				{
					ReleaseSRWLockExclusive(&pCounter->WaitLock);
					YieldProcessor();
					continue;
				}
				if (cor::CompareExchangeAtomic
					(
						&pCounter->Value,
						I32{ 0 },
						OldCounterValue
					) == OldCounterValue)
				{
					// Since this worker finished the batch, release waiters on this counter 
					ReleaseAtomicCounterWaitersLocked(pCounter);
					ReleaseSRWLockExclusive(&pCounter->WaitLock);
					return;
				}
				ReleaseSRWLockExclusive(&pCounter->WaitLock);
				YieldProcessor();
				continue;
			}

			// CAS to notice state change by another worker, on fail, we might need to clean up
			// (or spuriously since CASWeak is a bitch)
			// TODO Is seq cst really necessary? 
			if (cor::CompareExchangeAtomicWeak
				(
					&pCounter->Value,
					&OldCounterValue,
					OldCounterValue - Delta, 
					std::memory_order_seq_cst,
					std::memory_order_seq_cst
				)) return;
			YieldProcessor();
		}
	}

	void FlushWorkerCounterCompletions(FWorker* pWorker)
	{
		FAtomicCounterState* pCounter = pWorker->pDeferredCompletionCounter;
		U32 NumCompletions = pWorker->NumDeferredCounterCompletions;
		if (!pCounter || NumCompletions == 0u)
			return;

		pWorker->pDeferredCompletionCounter = nullptr;
		pWorker->NumDeferredCounterCompletions = 0u;
		CompleteAtomicCounterBatch(pCounter, NumCompletions);
	}

	// Workers record completions locally, reduces contention on shared atomics
	void FlushWorkerCompletions(FWorker* pWorker)
	{
		U32 NumCompletions = pWorker->NumUnflushedCompletions;

		FlushWorkerCounterCompletions(pWorker);
		if (NumCompletions == 0u)
			return;

		pWorker->NumUnflushedCompletions = 0u;
		if (cor::AddAtomic(&gScheduler.NumOutstandingJobs, -I64(NumCompletions)) == 0)
		{
			SetEvent(gScheduler.hIdleEvent);
			if (gScheduler.Desc.IdleBehavior == EIdleBehavior::Sleep)
				NotifyWorkers();
		}
	}

	FFiber* PopPinnedFiber(FWorker* pWorker)
	{
		if (cor::LoadAtomic(&pWorker->NumPinnedReadyFibers) == 0)
			return nullptr;

		AcquireSRWLockExclusive(&pWorker->PinnedReadyLock);
		FFiber* pFiber = pWorker->pPinnedReadyHead;
		if (pFiber)
		{
			pWorker->pPinnedReadyHead = pFiber->pReadyNext;
			if (!pWorker->pPinnedReadyHead)
				pWorker->pPinnedReadyTail = nullptr;
			pFiber->pReadyNext = nullptr;
			cor::DecrementAtomic(&pWorker->NumPinnedReadyFibers);
		}
		ReleaseSRWLockExclusive(&pWorker->PinnedReadyLock);
		return pFiber;
	}

	FFiber* WorkerTakeReadyFiber(FWorker* pWorker)
	{
		// Try pinned
		FFiber* pFiber = PopPinnedFiber(pWorker);

		// Try owned
		if (!pFiber)
			pFiber = PopOwnedReadyFiber(pWorker);

		// Try injection
		UQueueValue uValue;
		if (!pFiber && DequeueMPMC(&gScheduler.ReadyFibers, &uValue))
		{
			pFiber = uValue.pFiber;
			cor::DecrementAtomic(&gScheduler.NumGlobalReadyFibers);
		}
		return pFiber;

	}

	bool ActivateResumedFiber
	(
		FWorker* pWorker,
		FFiber* pFiber)
	{
		FCallbacks const* pCallbacks = &gScheduler.Desc.Callbacks;

		if (cor::CompareExchangeAtomic
			(
				&pFiber->State,
				I32(EFiberState::Running),
				I32(EFiberState::Ready)
			) != I32(EFiberState::Ready))
		{
			return false;
		}

		pFiber->pWorker = pWorker;
		pFiber->YieldReason = EYieldReason::None;
		pFiber->PinnedWorkerIndex = ORDO_INVALID_INDEX;
		
		if (pCallbacks->pfnFiberResumed != nullptr)
		{
			pCallbacks->pfnFiberResumed
			(
				pCallbacks->pContext,
				pFiber->Index,
				pWorker->Index
			);
		}
		return true;
	}

	void WorkerRunFiber
	(
		FWorker* pWorker,
		FFiber* pFiber,
		bool bResumed)
	{
		// Method is executed on dispatcher fiber

		if (bResumed)
		{
			if (!ActivateResumedFiber(pWorker, pFiber))
				return;
		}
		else
		{
			pFiber->pWorker = pWorker;
			pFiber->YieldReason = EYieldReason::None;
			// TODO Store might be sufficient here
			cor::ExchangeAtomic
			(
				&pFiber->State,
				I32(EFiberState::Running)
			);
		}

		I32 PreviousState;

#if ORDO_DEBUG
		I32 ThreadId = I32(GetCurrentThreadId());
		PreviousState = cor::CompareExchangeAtomic
		(
			&pFiber->RunningThreadId,
			ThreadId,
			I32{0}
		);
		if (PreviousState != 0)
		{
			COR_ASSERT(PreviousState == 0);
			std::fprintf
			(
				stderr,
				"Fiber %u concurrently entered by threads %d and %d\n",
				pFiber->Index,
				PreviousState,
				ThreadId
			);
			return;
		}
#endif

		// Execute job
		gpCurrentFiber = pFiber;
		SwitchToFiber(pFiber->pNativeFiber);

		// Switched back to dispatcher fiber 
		gpCurrentFiber = nullptr;

#if ORDO_DEBUG
		PreviousState = cor::CompareExchangeAtomic
		(
			&pFiber->RunningThreadId,
			I32{0},
			ThreadId
		);
		COR_ASSERT(PreviousState == ThreadId);
#endif

		if (pFiber->YieldReason == EYieldReason::Completed)
		{
			// Job completed, fiber can be reused 
			pFiber->Task = { };
			pFiber->pWorker = nullptr;
			cor::ExchangeAtomic
			(
				&pFiber->State,
				I32(EFiberState::Idle)
			);
			InterlockedPushEntrySList
			(
				&gScheduler.FreeFibers,
				&pFiber->FreeEntry
			);
		}
		else if (pFiber->YieldReason == EYieldReason::Parked)
		{
			PreviousState = cor::CompareExchangeAtomic
			(
				&pFiber->State,
				I32(EFiberState::Parked),
				I32(EFiberState::Parking)
			);
			if (PreviousState == I32(EFiberState::Ready))
			{
				// See WakeFiber() for transition
				ScheduleReadyFiber(pFiber);
			}
			else
			{
				COR_ASSERT(PreviousState == I32(EFiberState::Parking));
			}
		}
		else
		{
			COR_ASSERT(false, "Fiber returned to dispatcher without yield reason");
		}
	}

	bool WorkerTakeTask
	(
		FWorker* pWorker,
		FTaskPacket* pTask,
		bool bExhaustive)
	{
		UQueueValue uValue;

		if (pWorker->bHasPendingTask)
		{
			*pTask = pWorker->PendingTask;
			pWorker->bHasPendingTask = false;
			return true;
		}

		// Prefer both local priorities before touching a remote workers cache line
		// I.e., prio is enforced only on a local level!! 
		for (I32 Priority = 0; Priority < I32(EPriority::Count); ++Priority)
		{
			// Try local queue
			if (PopWorkDeque
				(
					&pWorker->LocalQueues[Priority],
					pTask
				)) return true;

			// Try injection queue
			if (DequeueMPMC
				(
					&pWorker->InjectionQueues[Priority],
					&uValue
				))
			{
				*pTask = uValue.Task;
				return true;
			}
		}

		// No task assigned to worker, try steal

		// Without this, worker could keep stealing unrleated work while a counter still appeared nonzero
		FlushWorkerCounterCompletions(pWorker);

		if (gScheduler.NumWorkerThreads == 1u)
			return false;

		U32 NumProbes = gScheduler.NumWorkerThreads - 1u;
		if (!bExhaustive && NumProbes > ORDO_STEAL_PROBE_COUNT)
			NumProbes = ORDO_STEAL_PROBE_COUNT;
		U32 Start = cor::XORShift32Next(&pWorker->StealState) % (gScheduler.NumWorkerThreads - 1u);
		for (U32 Probe = 0u; Probe < NumProbes; ++Probe)
		{
			U32 Victim = 
				(
					// Start one position after current worker
					pWorker->Index + 1u  
					// Walk through N - 1 other worker positions
					+ ((Start + Probe) % (gScheduler.NumWorkerThreads - 1u))
				) % gScheduler.NumWorkerThreads;
			for (I32 Priority = 0; Priority < I32(EPriority::Count); ++Priority)
			{
				if (StealWorkDeque
					(
						&gScheduler.pWorkers[Victim].LocalQueues[Priority], 
						pTask
					)) return true;

				if (DequeueMPMC
					(
						&gScheduler.pWorkers[Victim].InjectionQueues[Priority],
						&uValue
					))
				{
					*pTask = uValue.Task;
					return true;
				}
			}
		}
		return false;
	}

	bool WorkerDispatchOne
	(
		FWorker* pWorker,
		bool bExhaustive)
	{
		FFiber* pFiber = WorkerTakeReadyFiber(pWorker);
		if (pFiber)
		{
			WorkerRunFiber(pWorker, pFiber, true);
			return true;
		}

		// Get next task
		FTaskPacket Task;
		if (!WorkerTakeTask(pWorker, &Task, bExhaustive))
			return false;

		// Get free fiber
		PSLIST_ENTRY pFreeEntry = InterlockedPopEntrySList(&gScheduler.FreeFibers);
		if (!pFreeEntry)
		{
			pWorker->PendingTask = Task;
			pWorker->bHasPendingTask = true;
			return false;
		}
		pFiber = CONTAINING_RECORD(pFreeEntry, FFiber, FreeEntry);
		pFiber->Task = Task;
		WorkerRunFiber(pWorker, pFiber, false);
		return true;
	}


	DWORD WaitForWorkerNotification
	(
		I64 ObservedEpoch,
		DWORD TimeoutInMilliseconds)
	{
		if (WaitOnAddress
		(
			&gScheduler.WorkEpoch.Value,
			&ObservedEpoch,
			sizeof(ObservedEpoch),
			TimeoutInMilliseconds
		) != FALSE) return WAIT_OBJECT_0;

		return GetLastError() == ERROR_TIMEOUT 
			? WAIT_TIMEOUT
			: WAIT_FAILED;
	}

	void FinishJob(FFiber* pFiber)
	{
		FWorker* pWorker = pFiber->pWorker;
		if (pFiber->Task.pCompletionCounter)
		{
			// If worker didnt work on this job batch before
			if (pWorker->pDeferredCompletionCounter != pFiber->Task.pCompletionCounter)
			{
				FlushWorkerCounterCompletions(pWorker);
				// It does now
				pWorker->pDeferredCompletionCounter = pFiber->Task.pCompletionCounter;
			}
			++pWorker->NumDeferredCounterCompletions;
			if (pWorker->NumDeferredCounterCompletions >= ORDO_COUNTER_COMPLETION_FLUSH_THRESHOLD)
				FlushWorkerCounterCompletions(pWorker);
		}
		else
		{
			FlushWorkerCounterCompletions(pWorker);
		}

		++pWorker->NumUnflushedCompletions;
		if (pWorker->NumUnflushedCompletions >= ORDO_COMPLETION_FLUSH_THRESHOLD)
			FlushWorkerCompletions(pWorker);
	}

	VOID WINAPI FiberEntryPoint(void* pArg)
	{
		FFiber* pFiber = (FFiber*)pArg;
		U32 NumHotTasks = 0u;

		FCallbacks const* pCallbacks = &gScheduler.Desc.Callbacks;

		for (;;)
		{
			FTaskPacket Task = pFiber->Task;
			COR_ASSERT(Task.Job.pfnEntryPoint);
			
			if (pCallbacks->pfnJobStarted)
			{
				pCallbacks->pfnJobStarted
				(
					pCallbacks->pContext,
					&Task.Job,
					pFiber->Index,
					pFiber->pWorker->Index
				);
			}

			// Execute job
			Task.Job.pfnEntryPoint(Task.Job.pArg);
			
			if (pCallbacks->pfnJobCompleted)
			{
				pCallbacks->pfnJobCompleted
				(
					pCallbacks->pContext,
					&Task.Job,
					pFiber->Index,
					pFiber->pWorker->Index
				);
			}

			FinishJob(pFiber);
			++NumHotTasks;

			if (pFiber->pWorker->NumOwnedReadyFibers == 0u
				&& cor::LoadAtomic(&pFiber->pWorker->NumPinnedReadyFibers) == 0
				&& (NumHotTasks < ORDO_FIBER_MAX_NUM_HOT_REUSES
					|| cor::LoadAtomic(&gScheduler.NumGlobalReadyFibers) == 0))
			{
				FTaskPacket NextTask;
				if (WorkerTakeTask(pFiber->pWorker, &NextTask, false))
				{
					pFiber->Task = NextTask;
					continue;
				}
			}

			NumHotTasks = 0;
			pFiber->YieldReason = EYieldReason::Completed;
			SwitchToFiber(pFiber->pWorker->pDispatcherFiber);
		}
	}

	DWORD WINAPI WorkerEntryPoint(void* pArg)
	{
		FWorker* pWorker = (FWorker*)pArg;
		U32 EmptyRounds = 0u;
		if (!AttachCurrentThreadAsWorker(pWorker))
			return 1u;

		while (!cor::LoadAtomic(&gScheduler.bStopRequested))
		{
			if (WorkerDispatchOne(pWorker, false))
			{
				EmptyRounds = 0u;
				continue;
			}

			FlushWorkerCompletions(pWorker);
			++EmptyRounds;
			if (gScheduler.Desc.IdleBehavior == EIdleBehavior::Spin) YieldProcessor();
			else if (gScheduler.Desc.IdleBehavior == EIdleBehavior::Yield) SwitchToThread();
			else if (EmptyRounds < 64u) YieldProcessor();
			else
			{
				// Go to sleep
				
				// Observe current Epoch
				I64 ObservedEpoch = cor::LoadAtomic(&gScheduler.WorkEpoch);
				cor::AtomicThreadFence(std::memory_order_seq_cst);
				
				// Before sleep, do an exhaustive search on other workers 
				if (!WorkerDispatchOne(pWorker, true)
					&& !cor::LoadAtomic(&gScheduler.bStopRequested)
					)
				{
					WaitForWorkerNotification(ObservedEpoch, INFINITE);
				}
				EmptyRounds = 0u;
			}
		}

		DetachCurrentThreadFromWorker(pWorker);

		return 0;
	}

	FSchedulerDesc DefaultSchedulerDesc()
	{
		FSchedulerDesc Desc = { };
		// Selects all available
		Desc.NumWorkerThreads = 0u;
		Desc.NumFibers = 512u;
		// 256 KiB reserved stack per fiber
		Desc.FiberStackSizeBytes = 256u * 1024u;
		Desc.LocalQueueCapacity = 8 * 512;
		Desc.InjectionQueueCapacity = 512 * 512;
		Desc.IdleBehavior = EIdleBehavior::Sleep;
		Desc.bPinWorkersToLogicalProcessors = false;
		Desc.pAllocator = nullptr;
		return Desc;
	}

	

	void StopAndJoinWorkers()
	{
		cor::ExchangeAtomic(&gScheduler.bStopRequested, I32{1});
		NotifyWorkers();
		for (U32 Offset = 0u; Offset < gScheduler.NumCreatedThreads; ++Offset)
		{
			U32 WorkerIndex = Offset + 1u;
			FWorker* pWorker = &gScheduler.pWorkers[WorkerIndex];
			WaitForSingleObject(pWorker->hThread, INFINITE);
			CloseHandle(pWorker->hThread);
			pWorker->hThread = nullptr;
		}
	}

	void DestroySchedulerStorage()
	{
		if (gScheduler.pFibers)
		{
			for (U32 i = 0u; i < gScheduler.NumFibers; ++i)
			{
				if (gScheduler.pFibers[i].pNativeFiber)
					DeleteFiber(gScheduler.pFibers[i].pNativeFiber);
			}
		}

		if (gScheduler.pWorkers)
		{
			for (U32 i = 0u; i < gScheduler.NumWorkerThreads; ++i)
			{
				for (U32 Priority = 0; Priority < U32(EPriority::Count); ++Priority)
				{
					ShutdownWorkDeque(&gScheduler.pWorkers[i].LocalQueues[Priority]);
					ShutdownMPMCQueue(&gScheduler.pWorkers[i].InjectionQueues[Priority]);
				}
			}
		}

		ShutdownMPMCQueue(&gScheduler.ReadyFibers);

		if (gScheduler.hWorkersReadyEvent)
			CloseHandle(gScheduler.hWorkersReadyEvent);
		if (gScheduler.hIdleEvent)
			CloseHandle(gScheduler.hIdleEvent);

		cor::FAllocator* pAllocator = gScheduler.Desc.pAllocator;
		cor::DeallocateArray
		(
			pAllocator,
			gScheduler.pFibers,
			gScheduler.NumFibers
		);
		cor::DeallocateArray
		(
			pAllocator,
			gScheduler.pWorkers,
			gScheduler.NumWorkerThreads
		);
	}

	bool SubmitPacket(FTaskPacket Task, EPriority Priority, U32 InjectionStart)
	{
		U32 PriorityIndex = U32(Priority);
		if (gpCurrentWorker 
			&& gpCurrentFiber 
			&& PushWorkDeque(&gpCurrentWorker->LocalQueues[PriorityIndex], Task))
		{
			return true;
		}

		for (U32 Offset = 0u; Offset < gScheduler.NumWorkerThreads; ++Offset)
		{
			UQueueValue uValue;
			U32 WorkerIndex = (InjectionStart + Offset) % gScheduler.NumWorkerThreads;
			uValue.Task = Task;
			if (EnqueueMPMC(&gScheduler.pWorkers[WorkerIndex].InjectionQueues[PriorityIndex], uValue))
				return true;
		}

		return false;
	}

	EResult Submit(FJob Job, EPriority Priority, HAtomicCounter* pCounter)
	{
		return SubmitBatch(&Job, 1u, Priority, pCounter) == 1u 
			? EResult::Success 
			: EResult::QueueFull;
	}

	U32 SubmitBatch
	(
		FJob const* pJobs, 
		U32 NumJobs, 
		EPriority Priority, 
		HAtomicCounter* pCounter)
	{
		if ((NumJobs != 0u && !pJobs)
			|| NumJobs == 0u
			|| NumJobs > U32(I32_MAX)
			|| U32(Priority) >= U32(EPriority::Count)
			|| !cor::LoadAtomic(&gScheduler.bAcceptingJobs))
		{
			return 0u;
		}

		bool bLocalSubmission = gpCurrentWorker && gpCurrentFiber;
		U32 InjectionStart = bLocalSubmission 
			? gpCurrentWorker->Index
			: U32(cor::FetchAddAtomic
					(
						&gScheduler.NextInjectionWorker, 
						I32{1}, 
						std::memory_order_relaxed
					)) % gScheduler.NumWorkerThreads;
		U32 InjectionChunkSize = U32
		(
			(U64(NumJobs) + gScheduler.NumWorkerThreads - 1u) / gScheduler.NumWorkerThreads
		);
		if (InjectionChunkSize > ORDO_MAX_INJECTION_CHUNK_SIZE)
			InjectionChunkSize = ORDO_MAX_INJECTION_CHUNK_SIZE;

		for (U32 Index = 0u; Index < NumJobs; ++Index)
		{
			if (!pJobs[Index].pfnEntryPoint)
				return 0u;
		}

		FAtomicCounterState* pCounterState = nullptr;
		if (pCounter)
		{
			pCounterState = (FAtomicCounterState*)pCounter->pInternal;
			if (!pCounterState || AddAtomicCounterState(pCounterState, NumJobs) != EResult::Success)
				return 0u;
		}

		cor::AddAtomic(&gScheduler.NumOutstandingJobs, I64{NumJobs});
		ResetEvent(gScheduler.hIdleEvent);

		U32 Index = 0u;
		U32 Accepted = 0u;
		U32 ChunkIndex = 0u;
		U32 PriorityIndex = U32(Priority);
		while (Index < NumJobs)
		{
			if (!bLocalSubmission)
			{
				U32 Remaining = NumJobs - Index;
				U32 Requested = Remaining < InjectionChunkSize ? Remaining : InjectionChunkSize;
				U32 WorkerIndex = (InjectionStart + ChunkIndex) % gScheduler.NumWorkerThreads;
				U32 Enqueued = EnqueueMPMCTaskBatch
				(
					&gScheduler.pWorkers[WorkerIndex].InjectionQueues[PriorityIndex], 
					&pJobs[Index], 
					Requested, 
					pCounterState
				);
				++ChunkIndex;
				if (Enqueued != 0u)
				{
					Index += Enqueued;
					Accepted += Enqueued;
					continue;
				}
			}

			FTaskPacket Task;
			Task.Job = pJobs[Index];
			Task.pCompletionCounter = pCounterState;
			if (!SubmitPacket(Task, Priority, (InjectionStart + ChunkIndex) % gScheduler.NumWorkerThreads))
				break;

			++Index;
			++Accepted;
		}

		if (Accepted != NumJobs)
		{
			I64 Missing = I64(NumJobs - Accepted);
			if (pCounterState)
			{
				EResult Result = AddAtomicCounterState(pCounterState, -I32(Missing));
				COR_ASSERT(Result == EResult::Success);
			}
			if (cor::AddAtomic(&gScheduler.NumOutstandingJobs, -Missing) == 0)
			{
				SetEvent(gScheduler.hIdleEvent);
				if (gScheduler.Desc.IdleBehavior == EIdleBehavior::Sleep)
					NotifyWorkers();
			}
		}

		if (Accepted != 0u && gScheduler.Desc.IdleBehavior == EIdleBehavior::Sleep)
			NotifyWorkers();

		return Accepted;
	}

	EResult Initialize
	(
		FSchedulerDesc const* pDesc)
	{
		if (gpCurrentWorker || gpCurrentFiber)
			return EResult::InvalidState;

		FSchedulerDesc Desc = (pDesc) ? *pDesc : DefaultSchedulerDesc();

		if (!Desc.NumWorkerThreads)
			Desc.NumWorkerThreads = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);

		if (!Desc.NumWorkerThreads)
			Desc.NumWorkerThreads = 1u;

		if (Desc.NumFibers < 2u
			|| Desc.NumFibers > U32_MAX / 2u
			|| Desc.FiberStackSizeBytes < 64u * 1024u
			|| U32(Desc.IdleBehavior) > U32(EIdleBehavior::Sleep))
		{
			return EResult::InvalidArg;
		}

		Desc.LocalQueueCapacity = cor::NextPowerOfTwo(Desc.LocalQueueCapacity);
		Desc.InjectionQueueCapacity = cor::NextPowerOfTwo(Desc.InjectionQueueCapacity);
			
		if (Desc.LocalQueueCapacity < 2u || Desc.InjectionQueueCapacity < 2u)
			return EResult::InvalidArg;

		U32 InjectionQueueCapacityPerWorker = cor::NextPowerOfTwo
		(
			U32
			(
				(U64(Desc.InjectionQueueCapacity) + Desc.NumWorkerThreads - 1u) 
				/ Desc.NumWorkerThreads
			)
		);

		if (InjectionQueueCapacityPerWorker < 2u)
			InjectionQueueCapacityPerWorker = 2u;

		::memset(&gScheduler, 0, sizeof(FScheduler));

		gScheduler.Desc = Desc;
		gScheduler.NumWorkerThreads = Desc.NumWorkerThreads;
		gScheduler.NumFibers = Desc.NumFibers;
		gScheduler.pWorkers = cor::AllocateArray<FWorker>
		(
			Desc.pAllocator,
			Desc.NumWorkerThreads,
			COR_CACHE_LINE_SIZE
		);
		gScheduler.pFibers = cor::AllocateArray<FFiber>
		(
			Desc.pAllocator,
			Desc.NumFibers,
			COR_CACHE_LINE_SIZE
		);
		if (!gScheduler.pWorkers || !gScheduler.pFibers)
		{
			DestroySchedulerStorage();
			return EResult::OutOfMemory;
		}

		InitializeSListHead(&gScheduler.FreeFibers);
		gScheduler.hIdleEvent = CreateEvent(nullptr, TRUE, TRUE, nullptr);
		gScheduler.hWorkersReadyEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
		if (!gScheduler.hIdleEvent || !gScheduler.hWorkersReadyEvent)
		{
			DestroySchedulerStorage();
			return EResult::PlatformError;
		}

		bool bInitialized = InitializeMPMCQueue
		(
			&gScheduler.ReadyFibers,
			cor::NextPowerOfTwo(Desc.NumFibers * 2u),
			Desc.pAllocator
		);

		// Worker preinit
		for (U32 Index = 0; Index < Desc.NumWorkerThreads; ++Index)
		{
			FWorker* pWorker = &gScheduler.pWorkers[Index];
			pWorker->Index = Index;
			InitializeSRWLock(&pWorker->PinnedReadyLock);
			for (U32 Priority = 0u; Priority < U32(EPriority::Count); ++Priority)
			{
				if (!InitializeWorkDeque
				(
					&pWorker->LocalQueues[Priority],
					Desc.LocalQueueCapacity,
					Desc.pAllocator
				)) bInitialized = false;

				if (!InitializeMPMCQueue
				(
					&pWorker->InjectionQueues[Priority],
					InjectionQueueCapacityPerWorker,
					Desc.pAllocator
				)) bInitialized = false;
			}
			pWorker->StealState = cor::XORShift32Seed(Index);
		}

		if (!bInitialized)
		{
			DestroySchedulerStorage();
			return EResult::OutOfMemory;
		}

		// Fiber init
		for (U32 Index = 0u; Index < Desc.NumFibers; ++Index)
		{
			FFiber* pFiber = &gScheduler.pFibers[Index];
			pFiber->Index = Index;
			pFiber->PinnedWorkerIndex = ORDO_INVALID_INDEX;
			cor::StoreAtomic
			(
				&pFiber->State,
				I32(EFiberState::Idle),
				std::memory_order_relaxed
			);
			pFiber->pNativeFiber = CreateFiberEx
			(
				0u,
				Desc.FiberStackSizeBytes,
				FIBER_FLAG_FLOAT_SWITCH,
				FiberEntryPoint,
				pFiber
			);
			if (!pFiber->pNativeFiber)
			{
				DestroySchedulerStorage();
				return EResult::PlatformError;
			}
			InterlockedPushEntrySList
			(
				&gScheduler.FreeFibers,
				&pFiber->FreeEntry
			);
			if (Desc.Callbacks.pfnFiberCreated)
			{
				Desc.Callbacks.pfnFiberCreated
				(
					Desc.Callbacks.pContext,
					Index
				);
			}
		}

		cor::InitializeAtomic(&gScheduler.WorkEpoch, I64{0});
		// Capture main thread as worker 0
		if (!AttachCurrentThreadAsWorker(&gScheduler.pWorkers[0]))
		{
			DestroySchedulerStorage();
			return EResult::PlatformError;
		}

		// Init workers
		for (U32 Index = 1u; Index < Desc.NumWorkerThreads; ++Index)
		{
			FWorker* pWorker = &gScheduler.pWorkers[Index];
			pWorker->hThread = CreateThread
			(
				nullptr,
				0u,
				WorkerEntryPoint,
				pWorker,
				0u,
				&pWorker->ThreadId
			);
			if (!pWorker->hThread)
				break;
			++gScheduler.NumCreatedThreads;
		}

		if (gScheduler.NumCreatedThreads != Desc.NumWorkerThreads - 1u)
		{
			StopAndJoinWorkers();
			DetachCurrentThreadFromWorker(&gScheduler.pWorkers[0]);
			DestroySchedulerStorage();
			return EResult::PlatformError;
		}

		WaitForSingleObject(gScheduler.hWorkersReadyEvent, INFINITE);
		if (cor::LoadAtomic(&gScheduler.NumWorkerStartFailures))
		{
			StopAndJoinWorkers();
			DetachCurrentThreadFromWorker(&gScheduler.pWorkers[0]);
			DestroySchedulerStorage();
			return EResult::PlatformError;
		}

		cor::StoreAtomic
		(
			&gScheduler.bAcceptingJobs,
			I32{ 1 },
			std::memory_order_seq_cst
		);

		return EResult::Success;
	}

	EResult WaitForIdle(U32 TimeoutMilliseconds)
	{
		bool bMainThreadDispatcher = IsMainThreadDispatcher();
		if (gpCurrentWorker && !bMainThreadDispatcher)
			return EResult::InvalidState;

		ULONGLONG Start = GetTickCount64();
		while (cor::LoadAtomic(&gScheduler.NumOutstandingJobs) != 0)
		{
			DWORD WaitTime = INFINITE;
			if (TimeoutMilliseconds != ORDO_INFINITE)
			{
				ULONGLONG Elapsed = GetTickCount64() - Start;
				if (Elapsed >= TimeoutMilliseconds)
					return EResult::Timeout;
				WaitTime = DWORD(TimeoutMilliseconds - Elapsed);
			}

			if (bMainThreadDispatcher)
			{
				FWorker* pWorker = gpCurrentWorker;
				if (WorkerDispatchOne(pWorker, false))
					continue;

				FlushWorkerCompletions(pWorker);
				if (cor::LoadAtomic(&gScheduler.NumOutstandingJobs) == 0)
					break;

				if (gScheduler.Desc.IdleBehavior == EIdleBehavior::Spin)
				{
					YieldProcessor();
				}
				else if (gScheduler.Desc.IdleBehavior == EIdleBehavior::Yield)
				{
					SwitchToThread();
				}
				else
				{
					I64 ObservedEpoch = cor::LoadAtomic(&gScheduler.WorkEpoch);
					cor::AtomicThreadFence(std::memory_order_seq_cst);
					if (cor::LoadAtomic(&gScheduler.NumOutstandingJobs) != 0
						&& !WorkerDispatchOne(pWorker, true))
					{
						DWORD WaitResult = WaitForWorkerNotification(ObservedEpoch, WaitTime);
						if (WaitResult == WAIT_TIMEOUT)
							return EResult::Timeout;
						if (WaitResult != WAIT_OBJECT_0)
							return EResult::PlatformError;
					}
				}
				continue;
			}
			DWORD WaitResult = WaitForSingleObject(gScheduler.hIdleEvent, WaitTime);
			if (WaitResult == WAIT_TIMEOUT)
				return EResult::Timeout;
			if (WaitResult != WAIT_OBJECT_0)
				return EResult::PlatformError;
		}
		return EResult::Success;
	}

	EResult Shutdown()
	{
		if (!IsMainThreadDispatcher())
			return EResult::InvalidState;
		if (cor::LoadAtomic(&gScheduler.NumLiveSyncObjects) != 0)
			return EResult::Busy;
		if (cor::CompareExchangeAtomic(&gScheduler.bAcceptingJobs, I32{0}, I32{1}) != 1)
			return EResult::InvalidState;

		EResult Result = WaitForIdle(ORDO_INFINITE);
		if (Result != EResult::Success)
			return Result;

		StopAndJoinWorkers();
		DetachCurrentThreadFromWorker(&gScheduler.pWorkers[0]);

		DestroySchedulerStorage();
		return EResult::Success;
	}
}