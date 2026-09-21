#pragma once

#include <Cor/BaseTypes.hh>

#include <Ordo/Defines.hh>
#include <Ordo/Api.hh>
#include <Ordo/Enums.hh>

namespace ordo
{
	typedef union USyncStorage
	{
		void* pPointerAlignment;
		U64	IntegerAlignment;
		U8 pBytes[ORDO_SYNC_STORAGE_SIZE];
	} USyncStorage;

	struct HAtomicCounter
	{
		void* pInternal;
		USyncStorage uStorage;
	};

	ORDO_API EResult InitializeAtomicCounter(HAtomicCounter* pCounter);

	ORDO_API EResult ShutdownAtomicCounter(HAtomicCounter* pCounter);

	ORDO_API EResult WaitForAtomicCounter(HAtomicCounter* pCounter, bool bPinToCurrentThread);
}