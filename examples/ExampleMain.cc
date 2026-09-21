#include <Ordo/Api.hh>

#include <malloc.h>

#include <stdio.h>

#include <Ordo/Scheduler.hh>

#include <Cor/Atomic.hh>

ORDO_DECLARE_JOB(InternalMain)
{
	
}

I32 main(void)
{
	if (ORDO_FAILED(ordo::Initialize(nullptr)))
		return -1;

	ordo::HAtomicCounter hCounter{ };
	if (ORDO_FAILED(ordo::InitializeAtomicCounter(&hCounter)))
		return -1;

	ordo::FJob MainJob{ };
	MainJob.pfnEntryPoint = InternalMain;
	MainJob.pArg = nullptr;
	if (ORDO_FAILED(ordo::Submit(MainJob, ordo::EPriority::High, &hCounter)))
		return -1;

	if (ORDO_FAILED(ordo::WaitForAtomicCounter(&hCounter, true)))
		return -1;

	if (ORDO_FAILED(ordo::ShutdownAtomicCounter(&hCounter)))
		return -1;

	if (ORDO_FAILED(ordo::WaitForIdle(U32_MAX)))
		return -1;

	if (ORDO_FAILED(ordo::Shutdown()))
		return -1;

	return 0;
}