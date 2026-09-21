#pragma once

#include <Ordo/Fwd.hh>

#include "Types.hh"

namespace ordo
{
	// ref: Dmitry Vyukov's bounded MPMC queue
	// ref: https://www.1024cores.net/home/lock-free-algorithms/queues/bounded-mpmc-queue
	bool InitializeMPMCQueue
	(
		FMPMCQueue* pQueue,
		U32 Capacity,
		cor::FAllocator* pAllocator
	);
	bool EnqueueMPMC(FMPMCQueue* pQueue, UQueueValue uValue);
	U32 EnqueueMPMCTaskBatch
	(
		FMPMCQueue* pQueue, 
		FJob const* pJobs, 
		U32 NumJobs, 
		FAtomicCounterState* pCounterState
	);
	bool DequeueMPMC(FMPMCQueue* pQueue, UQueueValue* pValue);
	void ShutdownMPMCQueue(FMPMCQueue* pQueue);

	// ref: Chase and Lev's deque
	// ref: https://www.cs.wm.edu/~dcschmidt/PDF/work-stealing-dequeue.pdf
	// ref: Memory model by Lê, et al.
	// ref: https://doi.org/10.1145/2442516.2442524
	bool InitializeWorkDeque
	(
		FWorkDeque* pDeque,
		U32 Capacity,
		cor::FAllocator* pAllocator);
	bool PushWorkDeque
	(
		FWorkDeque* pDeque,
		FTaskPacket Task
	);
	bool PopWorkDeque
	(
		FWorkDeque* pDeque, 
		FTaskPacket* pTask
	);
	bool StealWorkDeque
	(
		FWorkDeque* pDeque,
		FTaskPacket* pTask
	);
	void ShutdownWorkDeque(FWorkDeque* pDeque);

}