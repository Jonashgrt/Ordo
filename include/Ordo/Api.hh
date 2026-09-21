#pragma once

#include <Cor/Defines.hh>

#if defined(COR_PLATFORM_WINDOWS) && defined(ORDO_SHARED)
#if defined(ORDO_EXPORTS)
#define ORDO_API __declspec(dllexport)
#else
#define ORDO_API __declspec(dllimport)
#endif
#else
#define ORDO_API
#endif