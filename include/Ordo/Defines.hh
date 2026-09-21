#pragma once

#include <Ordo/Enums.hh>

#define ORDO_SUCCEEDED(Result) (::ordo::EResult::Success == Result)
#define ORDO_FAILED(Result) (!ORDO_SUCCEEDED(Result))

#define ORDO_SYNC_STORAGE_SIZE 64u