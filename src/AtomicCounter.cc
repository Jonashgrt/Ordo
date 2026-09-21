#include <Ordo/AtomicCounter.hh>
#include "Internal/AtomicCounterInternal.hh"

#include <new>

#include <Cor/Atomic.hh>

#include "Internal/Defines.hh"
#include "Internal/SchedulerInternal.hh"


namespace ordo
{
	EResult InitializeAtomicCounter(HAtomicCounter* pCounter)
	{
		if (!pCounter
			|| pCounter->pInternal
			|| !cor::LoadAtomic(&gScheduler.bAcceptingJobs))
		{
			return EResult::InvalidArg;
		}

		::memset(&pCounter->uStorage, 0, sizeof(pCounter->uStorage));

		FAtomicCounterState* pState = ::new((void*)pCounter->uStorage.pBytes) FAtomicCounterState{ };
		InitializeSRWLock(&pState->WaitLock);
		InitializeConditionVariable(&pState->ExternalCondition);
		pCounter->pInternal = pState;
		cor::IncrementAtomic(&gScheduler.NumLiveSyncObjects);
		return EResult::Success;
	}

	EResult ShutdownAtomicCounter(HAtomicCounter* pCounter)
	{
		if (!pCounter)
			return EResult::InvalidArg;
		FAtomicCounterState* pState = (FAtomicCounterState*)pCounter->pInternal;
		if (!pState)
			return EResult::InvalidArg;

		AcquireSRWLockExclusive(&pState->WaitLock);
		if (cor::LoadAtomic(&pState->Value) != 0 || cor::LoadAtomic(&pState->ActiveWaiters) != 0 || pState->pWaitHead)
		{
			ReleaseSRWLockExclusive(&pState->WaitLock);
			return EResult::Busy;
		}
		ReleaseSRWLockExclusive(&pState->WaitLock);

		cor::DecrementAtomic(&gScheduler.NumLiveSyncObjects);
		pCounter->pInternal = nullptr;
		pState->~FAtomicCounterState();
		::memset(&pCounter->uStorage, 0, sizeof(pCounter->uStorage));
		return EResult::Success;
	}	

	EResult AddAtomicCounterState(FAtomicCounterState* pState, I32 Delta)
	{
		if (Delta == 0)
			return EResult::Success;

		if (Delta > 0)
		{
			AcquireSRWLockExclusive(&pState->WaitLock);
			if (cor::LoadAtomic(&pState->ActiveWaiters) != 0)
			{
				ReleaseSRWLockExclusive(&pState->WaitLock);
				return EResult::Busy;
			}

			for (;;)
			{
				I32 OldValue = cor::LoadAtomic(&pState->Value);
				I64 NewValue = I64(OldValue) + I64(Delta);
				if (NewValue > I32_MAX)
				{
					ReleaseSRWLockExclusive(&pState->WaitLock);
					return EResult::InvalidArg;
				}
				if (cor::CompareExchangeAtomic(&pState->Value, I32(NewValue), OldValue) == OldValue)
					break;
				YieldProcessor();
			}
			ReleaseSRWLockExclusive(&pState->WaitLock);
			return EResult::Success;
		}

		for (;;)
		{
			I32 OldValue = cor::LoadAtomic(&pState->Value);
			I64 NewValue = I64(OldValue) + I64(Delta);
			if (NewValue < 0)
				return EResult::InvalidState;
			if (NewValue == 0)
			{
				AcquireSRWLockExclusive(&pState->WaitLock);
				OldValue = cor::LoadAtomic(&pState->Value);
				NewValue = I64(OldValue) + I64(Delta);
				if (NewValue < 0)
				{
					ReleaseSRWLockExclusive(&pState->WaitLock);
					return EResult::InvalidState;
				}
				if (NewValue != 0)
				{
					ReleaseSRWLockExclusive(&pState->WaitLock);
					YieldProcessor();
					continue;
				}
				if (cor::CompareExchangeAtomic(&pState->Value, I32{ 0 }, OldValue) == OldValue)
				{
					ReleaseAtomicCounterWaitersLocked(pState);
					ReleaseSRWLockExclusive(&pState->WaitLock);
					return EResult::Success;
				}
				ReleaseSRWLockExclusive(&pState->WaitLock);
				YieldProcessor();
				continue;
			}
			if (cor::CompareExchangeAtomic(&pState->Value, I32(NewValue), OldValue) == OldValue)
				return EResult::Success;
			YieldProcessor();
		}
	}

	EResult WaitForAtomicCounter(HAtomicCounter* pCounter, bool bPinToCurrentThread)
	{
		if (!pCounter)
			return EResult::InvalidArg;
		FAtomicCounterState* pState = (FAtomicCounterState*)pCounter->pInternal;
		if (!pState)
			return EResult::InvalidArg;

		FFiber* pFiber = gpCurrentFiber;
		if (!pFiber && gpCurrentWorker && !IsMainThreadDispatcher())
			return EResult::InvalidState;

		if (gpCurrentWorker)
			FlushWorkerCounterCompletions(gpCurrentWorker);

		AcquireSRWLockExclusive(&pState->WaitLock);
		if (cor::LoadAtomic(&pState->Value) == 0)
		{
			ReleaseSRWLockExclusive(&pState->WaitLock);
			return EResult::Success;
		}

		cor::IncrementAtomic(&pState->ActiveWaiters);
		if (pFiber)
		{
			pFiber->PinnedWorkerIndex = bPinToCurrentThread ? gpCurrentWorker->Index : ORDO_INVALID_INDEX;
			pFiber->pWaitNext = nullptr;

			cor::ExchangeAtomic(&pFiber->State, I32(EFiberState::Parking));
			if (pState->pWaitTail)
				pState->pWaitTail->pWaitNext = pFiber;
			else
				pState->pWaitHead = pFiber;
			pState->pWaitTail = pFiber;
			ReleaseSRWLockExclusive(&pState->WaitLock);

			ParkCurrentFiber(ESuspendReason::AtomicCounter);

			AcquireSRWLockExclusive(&pState->WaitLock);
			cor::DecrementAtomic(&pState->ActiveWaiters);
			ReleaseSRWLockExclusive(&pState->WaitLock);
			return EResult::Success;
		}

		if (IsMainThreadDispatcher())
		{
			FWorker* pWorker = gpCurrentWorker;
			cor::IncrementAtomic(&pState->NumMainThreadWaiters);
			ReleaseSRWLockExclusive(&pState->WaitLock);

			EResult Result = EResult::Success;
			while (cor::LoadAtomic(&pState->Value) != 0)
			{
				if (WorkerDispatchOne(pWorker, false))
					continue;

				FlushWorkerCompletions(pWorker);
				if (cor::LoadAtomic(&pState->Value) == 0)
					break;

				if (gScheduler.Desc.IdleBehavior == EIdleBehavior::Spin)
					YieldProcessor();
				else if (gScheduler.Desc.IdleBehavior == EIdleBehavior::Yield)
					SwitchToThread();
				else
				{
					I64 ObservedEpoch = cor::LoadAtomic(&gScheduler.WorkEpoch);
					cor::AtomicThreadFence(std::memory_order_seq_cst);
					if (cor::LoadAtomic(&pState->Value) != 0 && !WorkerDispatchOne(pWorker, true))
					{
						DWORD WaitResult = WaitForWorkerNotification(ObservedEpoch, INFINITE);
						if (WaitResult != WAIT_OBJECT_0)
						{
							Result = EResult::PlatformError;
							break;
						}
					}
				}
			}
			FlushWorkerCompletions(pWorker);

			AcquireSRWLockExclusive(&pState->WaitLock);
			cor::DecrementAtomic(&pState->NumMainThreadWaiters);
			cor::DecrementAtomic(&pState->ActiveWaiters);
			ReleaseSRWLockExclusive(&pState->WaitLock);
			return Result;
		}

		while (cor::LoadAtomic(&pState->Value) != 0)
		{
			if (!SleepConditionVariableSRW(&pState->ExternalCondition, &pState->WaitLock, INFINITE, 0u))
			{
				cor::DecrementAtomic(&pState->ActiveWaiters);
				ReleaseSRWLockExclusive(&pState->WaitLock);
				return EResult::PlatformError;
			}
		}
		cor::DecrementAtomic(&pState->ActiveWaiters);
		ReleaseSRWLockExclusive(&pState->WaitLock);
		return EResult::Success;
	}
}