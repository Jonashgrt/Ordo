#pragma once

#include <Ordo/Fwd.hh>
#include <Ordo/Defines.hh>

namespace ordo
{
	typedef void (*PFNJob)(void* pArg);
#define ORDO_DECLARE_JOB(JobName) void JobName(void* pArg)

	struct FJob
	{
		PFNJob pfnEntryPoint;
		void* pArg;
	};
}