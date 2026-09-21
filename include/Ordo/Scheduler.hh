#pragma once

#include <Cor/BaseTypes.hh>
#include <Cor/Allocator.hh>

#include <Ordo/Api.hh>
#include <Ordo/Enums.hh>
#include <Ordo/Callbacks.hh>
#include <Ordo/Job.hh>
#include <Ordo/AtomicCounter.hh>


namespace ordo
{
	struct FSchedulerDesc
	{
		U32 NumWorkerThreads;
		U32 NumFibers;
		U32 FiberStackSizeBytes;
		U32 LocalQueueCapacity;
		U32 InjectionQueueCapacity;
		EIdleBehavior IdleBehavior;
		bool bPinWorkersToLogicalProcessors;
		FCallbacks Callbacks;
		::cor::FAllocator* pAllocator;
	};

	ORDO_API FSchedulerDesc DefaultSchedulerDesc();

	ORDO_API EResult Initialize
	(
		FSchedulerDesc const* pDesc
	);

	ORDO_API EResult WaitForIdle(U32 TimeoutMilliseconds);

	ORDO_API EResult Shutdown();
	
	ORDO_API EResult Submit(FJob Job, EPriority Priority, HAtomicCounter* pCounter);

	ORDO_API U32 SubmitBatch(FJob const* pJobs, U32 NumJobs, EPriority Priority, HAtomicCounter* pCounter);
}