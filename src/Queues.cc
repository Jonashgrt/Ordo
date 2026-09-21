
/*
 * This file contains original MIT-licensed work and an adaptation of
 * Dmitry Vyukov's bounded MPMC queue, licensed under BSD-2-Clause.
 *
 * Copyright (c) 2010-2011, Dmitry Vyukov. All rights reserved.
 *
 * The complete third-party copyright notice, license conditions, disclaimer,
 * and algorithm references are preserved in NOTICE.md at the
 * repository root. That file must accompany source and binary distributions.
 */


#include "Internal/Queues.hh"

#include <Cor/BaseTypes.hh>
#include <Cor/Allocator.hh>
#include <Cor/Atomic.hh>
#include <Cor/Bit.hh>

namespace ordo
{
	// ref: Dmitry Vyukov's bounded MPMC queue
	// ref: https://www.1024cores.net/home/lock-free-algorithms/queues/bounded-mpmc-queue
	bool InitializeMPMCQueue
	(
		FMPMCQueue* pQueue,
		U32 Capacity,
		cor::FAllocator* pAllocator)
	{
		if (!pQueue || !cor::IsPowerOfTwo(Capacity) || Capacity < 2u)
			return false;

		pAllocator = cor::ResolveAllocator(pAllocator);
		pQueue->pCells = cor::AllocateArray<FMPMCCell>
		(
			pAllocator,
			Capacity,
			COR_CACHE_LINE_SIZE
		);
		if (!pQueue->pCells)
			return false;

		pQueue->pAllocator = pAllocator;
		pQueue->Mask = U64(Capacity) - 1u;
		for (U32 Index = 0u; Index < Capacity; ++Index)
		{
			cor::StoreAtomic
			(
				&pQueue->pCells[Index].Sequence,
				I64(Index),
				std::memory_order_relaxed
			);
		}
		cor::InitializeAtomic(&pQueue->EnqueuePosition, I64{0});
		cor::InitializeAtomic(&pQueue->DequeuePosition, I64{0});
		return true;
	}

	bool EnqueueMPMC(FMPMCQueue* pQueue, UQueueValue uValue)
	{
		I64 Position = cor::LoadAtomic
		(
			&pQueue->EnqueuePosition,
			std::memory_order_relaxed
		);

		for (;;)
		{
			FMPMCCell* pCell = &pQueue->pCells[U64(Position) & pQueue->Mask];
			I64 Sequence = cor::LoadAtomic
			(
				&pCell->Sequence,
				std::memory_order_acquire
			);
			I64 Difference = Sequence - Position;

			if (Difference == 0)
			{
				if (cor::CompareExchangeAtomicWeak
					(
						&pQueue->EnqueuePosition,
						&Position,
						Position + 1,
						std::memory_order_relaxed,
						std::memory_order_relaxed
					))
				{
					pCell->Value = uValue;
					cor::StoreAtomic
					(
						&pCell->Sequence,
						Position + 1,
						std::memory_order_release
					);
					return true;
				}
			}
			else if (Difference < 0)
			{
				return false;
			}
			else
			{
				Position = cor::LoadAtomic
				(
					&pQueue->EnqueuePosition,
					std::memory_order_relaxed
				);
			}
			YieldProcessor();
		}
	}

	U32 EnqueueMPMCTaskBatch
	(
		FMPMCQueue* pQueue,
		FJob const* pJobs,
		U32 NumJobs,
		FAtomicCounterState* pCounterState)
	{
		U32 Capacity = U32(pQueue->Mask + 1u);
		U32 MaxCount = NumJobs < Capacity ? NumJobs : Capacity;
		I64 Position = cor::LoadAtomic(&pQueue->EnqueuePosition, std::memory_order_relaxed);

		for (;;)
		{
			U32 Count = MaxCount;
			for (U32 Index = 0u; Index < Count; ++Index)
			{
				I64 CellPosition = Position + I64(Index);
				FMPMCCell* pCell = &pQueue->pCells[U64(CellPosition) & pQueue->Mask];
				I64 Sequence = cor::LoadAtomic(&pCell->Sequence, std::memory_order_acquire);
				I64 Difference = Sequence - CellPosition;
				if (Difference < 0)
				{
					Count = Index;
					break;
				}
				if (Difference > 0)
				{
					Position = cor::LoadAtomic(&pQueue->EnqueuePosition, std::memory_order_relaxed);
					Count = U32_MAX;
					break;

				}
			}

			if (Count == U32_MAX)
				continue;
			if (Count == 0u)
				return 0u;

			I64 Expected = Position;
			if (!cor::CompareExchangeAtomicWeak
				(
					&pQueue->EnqueuePosition,
					&Expected,
					Position + I64(Count),
					std::memory_order_relaxed,
					std::memory_order_relaxed
				))
			{
				Position = Expected;
				YieldProcessor();
				continue;
			}

			for (U32 Index = 0u; Index < Count; ++Index)
			{
				I64 CellPosition = Position + I64(Index);
				FMPMCCell* pCell = &pQueue->pCells[U64(CellPosition) & pQueue->Mask];
				pCell->Value.Task.Job = pJobs[Index];
				pCell->Value.Task.pCompletionCounter = pCounterState;
			}
			for (U32 Index = 0u; Index < Count; ++Index)
			{
				I64 CellPosition = Position + I64(Index);
				FMPMCCell* pCell = &pQueue->pCells[U64(CellPosition) & pQueue->Mask];
				cor::StoreAtomic(&pCell->Sequence, CellPosition + 1, std::memory_order_release);
			}
			return Count;
		}
	}

	bool DequeueMPMC(FMPMCQueue* pQueue, UQueueValue* pValue)
	{
		I64 Position = cor::LoadAtomic
		(
			&pQueue->DequeuePosition,
			std::memory_order_relaxed
		);

		for (;;)
		{
			FMPMCCell* pCell = &pQueue->pCells[U64(Position) & pQueue->Mask];
			I64 Sequence = cor::LoadAtomic
			(
				&pCell->Sequence,
				std::memory_order_acquire
			);
			I64 Difference = Sequence - (Position + 1);

			if (Difference == 0)
			{
				if (cor::CompareExchangeAtomicWeak
					(
						&pQueue->DequeuePosition,
						&Position,
						Position + 1,
						std::memory_order_relaxed,
						std::memory_order_relaxed
					))
				{
					*pValue = pCell->Value;
					cor::StoreAtomic
					(
						&pCell->Sequence,
						Position + I64(pQueue->Mask) + 1,
						std::memory_order_release
					);
					return true;
				}
			}
			else if (Difference < 0)
			{
				return false;
			}
			else
			{
				Position = cor::LoadAtomic
				(
					&pQueue->DequeuePosition,
					std::memory_order_relaxed
				);
			}
			YieldProcessor();

		}
	}

	void ShutdownMPMCQueue(FMPMCQueue* pQueue)
	{
		if (!pQueue)
			return;

		U64 Capacity = (pQueue->pCells) ? U64(pQueue->Mask + 1u) : 0u;
		cor::DeallocateArray
		(
			pQueue->pAllocator,
			pQueue->pCells,
			Capacity
		);
		pQueue->pCells = nullptr;
		pQueue->pAllocator = nullptr;
		pQueue->Mask = 0u;
		cor::InitializeAtomic(&pQueue->EnqueuePosition, I64{0});
		cor::InitializeAtomic(&pQueue->DequeuePosition, I64{0});
	}

	// ref: Chase and Lev's deque,
	// ref: https://www.cs.wm.edu/~dcschmidt/PDF/work-stealing-dequeue.pdf
	// ref: Memory model by Lê, et al.
	// ref: https://doi.org/10.1145/2442516.2442524
	bool InitializeWorkDeque
	(
		FWorkDeque* pDeque,
		U32 Capacity,
		cor::FAllocator* pAllocator)
	{
		if (!pDeque || !cor::IsPowerOfTwo(Capacity) || Capacity < 2u)
			return false;

		pAllocator = cor::ResolveAllocator(pAllocator);
		pDeque->pItems = cor::AllocateArray<FTaskPacket>
		(
			pAllocator,
			Capacity,
			COR_CACHE_LINE_SIZE
		);
		if (!pDeque->pItems)
			return false;

		pDeque->pAllocator = pAllocator;
		pDeque->Mask = U64(Capacity) - 1u;
		cor::InitializeAtomic(&pDeque->Top, I64{0});
		cor::InitializeAtomic(&pDeque->Bottom, I64{0});
		return true;
	}

	bool PushWorkDeque
	(
		FWorkDeque* pDeque,
		FTaskPacket Task)
	{
		I64 Bottom = cor::LoadAtomic(&pDeque->Bottom, std::memory_order_relaxed);
		I64 Top = cor::LoadAtomic(&pDeque->Top, std::memory_order_acquire);
		if (U64(Bottom - Top) > pDeque->Mask)
			return false;

		pDeque->pItems[U64(Bottom) & pDeque->Mask] = Task;
		cor::AtomicThreadFence(std::memory_order_release);
		cor::StoreAtomic(&pDeque->Bottom, Bottom + 1, std::memory_order_relaxed);
		return true;
	}

	bool PopWorkDeque
	(
		FWorkDeque* pDeque,
		FTaskPacket* pTask)
	{
		I64 PublishedBottom = cor::LoadAtomic
		(
			&pDeque->Bottom,
			std::memory_order_relaxed
		);

		if (cor::LoadAtomic(&pDeque->Top, std::memory_order_acquire) >= PublishedBottom)
			return false;

		I64 Bottom = PublishedBottom - 1;
		cor::StoreAtomic(&pDeque->Bottom, Bottom, std::memory_order_relaxed);
		cor::AtomicThreadFence(std::memory_order_seq_cst);
		I64 Top = cor::LoadAtomic
		(
			&pDeque->Top,
			std::memory_order_relaxed
		);

		if (Top <= Bottom)
		{
			*pTask = pDeque->pItems[U64(Bottom) & pDeque->Mask];
			if (Top == Bottom)
			{
				if (cor::CompareExchangeAtomic
					(
						&pDeque->Top,
						Top + 1,
						Top
					) != Top)
				{
					cor::StoreAtomic
					(
						&pDeque->Bottom,
						PublishedBottom,
						std::memory_order_relaxed
					);
					return false;
				}
				cor::StoreAtomic
				(
					&pDeque->Bottom,
					PublishedBottom,
					std::memory_order_relaxed
				);
			}
			return true;
		}

		cor::StoreAtomic(&pDeque->Bottom, Top, std::memory_order_relaxed);
		return false;
	}

	bool StealWorkDeque
	(
		FWorkDeque* pDeque,
		FTaskPacket* pTask)
	{
		I64 Top = cor::LoadAtomic(&pDeque->Top, std::memory_order_acquire);
		I64 Bottom = cor::LoadAtomic(&pDeque->Bottom, std::memory_order_acquire);

		if (Top >= Bottom)
			return false;

		Top = cor::LoadAtomic(&pDeque->Top, std::memory_order_acquire);
		cor::AtomicThreadFence(std::memory_order_seq_cst);
		Bottom = cor::LoadAtomic(&pDeque->Bottom, std::memory_order_acquire);
		if (Top >= Bottom)
			return false;

		*pTask = pDeque->pItems[U64(Top) & pDeque->Mask];
		return cor::CompareExchangeAtomicStrong
		(
			&pDeque->Top,
			&Top,
			Top + 1,
			std::memory_order_seq_cst,
			std::memory_order_relaxed
		);
	}

	void ShutdownWorkDeque(FWorkDeque* pDeque)
	{
		if (!pDeque)
			return;

		U64 Capacity = (pDeque->pItems) ? U64(pDeque->Mask + 1u) : 0u;
		cor::DeallocateArray
		(
			pDeque->pAllocator,
			pDeque->pItems,
			Capacity
		);
		pDeque->pItems = nullptr;
		pDeque->pAllocator = nullptr;
		pDeque->Mask = 0u;
		cor::InitializeAtomic(&pDeque->Top, I64{ 0 });
		cor::InitializeAtomic(&pDeque->Bottom, I64{ 0 });
	}
}