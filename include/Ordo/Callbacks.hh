#pragma once

#include <Cor/BaseTypes.hh>

#include <Ordo/Fwd.hh>
#include <Ordo/Enums.hh>

namespace ordo
{
	struct FCallbacks
	{
		void* pContext;

		void (*pfnWorkerStarted)
		(
			void* pContext, 
			U32 WorkerIndex
		);
		void (*pfnWorkerStopped)
		(
			void* pContext, 
			U32 WorkerIndex
		);

		void (*pfnFiberCreated)
		(
			void* pContext, 
			U32 FiberIndex
		);
		void (*pfnFiberSuspended)
		(
			void* pContext,
			U32 FiberIndex,
			U32 WorkerIndex,
			ESuspendReason SuspendReason
		);
		void (*pfnFiberResumed)
		(
			void* pContext,
			U32 FiberIndex,
			U32 WorkerIndex
		);

		void (*pfnJobStarted)
		(
			void* pContext,
			FJob const* pJob,
			U32 FiberIndex,
			U32 WorkerIndex
		);

		void (*pfnJobCompleted)
		(
			void* pContext,
			FJob const* pJob,
			U32 FiberIndex,
			U32 WorkerIndex
		);
	};
}