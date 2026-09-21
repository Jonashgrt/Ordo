cmake_minimum_required(VERSION 3.25)

project(
	Ordo
	VERSION 1.0.0
	LANGUAGES CXX
)

option(ORDO_BUILD_EXAMPLES ON)

add_library(Ordo STATIC
	src/Internal/Defines.hh
	src/Internal/LeanWin32.hh
	src/Internal/Types.hh
	src/Internal/SchedulerInternal.hh
	src/Internal/Queues.hh
	src/Internal/AtomicCounterInternal.hh
	src/Queues.cc
	src/Scheduler.cc 
	src/AtomicCounter.cc
)

if(NOT TARGET Cor)
# Change this path to Cor repo
add_subdirectory(
	"${CMAKE_CURRENT_SOURCE_DIR}/../Cor"
	"${CMAKE_CURRENT_BINARY_DIR}/Cor/"
)
endif()

target_link_libraries(Ordo PUBLIC Cor)

target_include_directories(Ordo PUBLIC 
	$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(Ordo PUBLIC cxx_std_17)
target_compile_definitions(Ordo PUBLIC ORDO_DEBUG=$<IF:$<CONFIG:Debug>,1,0>)

if(WIN32)
	# Target windows version: 11/10
	target_compile_definitions(Ordo PRIVATE
		_WIN32_WINNT=0x0A00
	)
	target_link_libraries(Ordo PRIVATE Synchronization)
else()
	message(FATAL_ERROR "Platform unsupported!")
endif()

if (MSVC)
	target_compile_options(Ordo PRIVATE
		/WX
		/wd4324	
		/GT 
	)
	target_compile_options(Ordo INTERFACE /GT)
else()
	message(FATAL_ERROR "Compiler unsupported!")
endif()

set_target_properties(Ordo PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    UNITY_BUILD ON)

if (ORDO_BUILD_EXAMPLES)
	add_executable(OrdoTests examples/ExampleMain.cc)
	target_compile_options(OrdoTests PRIVATE /WX)
	target_link_libraries(OrdoTests PUBLIC Ordo)
endif()
