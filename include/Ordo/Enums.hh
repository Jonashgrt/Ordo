#pragma once

#include <Cor/BaseTypes.hh>

namespace ordo
{
	enum class EResult : U32
	{
		Success = 0,
		InvalidArg,
		InvalidState,
		OutOfMemory,
		PlatformError,
		Busy,
		Timeout,
		QueueFull
	};

	enum class EIdleBehavior : U32
	{
		Spin = 0,
		Yield,
		Sleep
	};

	enum class EPriority : U32
	{
		High = 0,
		Normal = 1,
		Count
	};

	enum class ESuspendReason : U32
	{
		AtomicCounter = 0,
		Mutex
	};
}