#pragma once

#include <Cor/Defines.hh>

#include <Ordo/Enums.hh>

#include "Types.hh"


namespace ordo
{
	extern COR_THREAD_LOCAL FWorker* gpCurrentWorker;
	extern COR_THREAD_LOCAL FFiber* gpCurrentFiber;
	extern FScheduler gScheduler;

	bool IsMainThreadDispatcher();
	void FlushWorkerCounterCompletions(FWorker* pWorker);
	void FlushWorkerCompletions(FWorker* pWorker);
	void ParkCurrentFiber(ESuspendReason Reason);
	bool WorkerDispatchOne(FWorker* pWorker, bool bExhaustive);
	DWORD WaitForWorkerNotification(I64 ObservedEpoch, DWORD TimeoutInMilliseconds);
	void ReleaseAtomicCounterWaitersLocked(FAtomicCounterState* pCounter);
}