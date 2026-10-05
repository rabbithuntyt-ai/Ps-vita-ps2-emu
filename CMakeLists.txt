cmake_minimum_required(VERSION 3.18)

# VitaPS2 - a PlayStation 2 emulator for the PlayStation Vita.
#
# The emulation core (EE/IOP/VU/SPU/HLE BIOS) is Play! (external/Play), built
# unmodified apart from the small, documented patches in patches/. This project
# adds a Vita frontend, a portable software GS renderer and a host test harness.

# Vita builds: pass -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake
# (the toolchain file sets VITA). Without it, a host build is produced.
project(VitaPS2 C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(PLAY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/Play)
if(NOT EXISTS ${PLAY_DIR}/Source/PS2VM.cpp)
	message(FATAL_ERROR "external/Play is missing. Run: git submodule update --init --recursive")
endif()

include(cmake/ApplyPatches.cmake)
vitaps2_apply_patches()

# Play!'s Header.cmake only detects the platform when Play! is the top-level
# project, so do it here.
if(VITA)
	set(TARGET_PLATFORM_VITA TRUE)
elseif(UNIX AND NOT APPLE)
	set(TARGET_PLATFORM_UNIX TRUE)
	if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64.*|AARCH64.*)")
		set(TARGET_PLATFORM_UNIX_AARCH64 TRUE)
	elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^arm")
		set(TARGET_PLATFORM_UNIX_ARM TRUE)
	endif()
	add_definitions(-D_FILE_OFFSET_BITS=64)
elseif(APPLE)
	set(TARGET_PLATFORM_MACOS TRUE)
endif()

# Play! build options: we only need the core library.
set(ENABLE_AMAZON_S3 OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)

add_subdirectory(${PLAY_DIR}/Source ${CMAKE_CURRENT_BINARY_DIR}/PlayCore EXCLUDE_FROM_ALL)

# The Vita's ELF converter (vita-elf-create) rejects PIC relocations such as
# R_ARM_BASE_PREL, but several bundled libraries (zlib, lzma, zstd) force
# POSITION_INDEPENDENT_CODE on. Turn it off for every target.
if(VITA)
	function(vitaps2_disable_pic directory)
		get_property(targets DIRECTORY ${directory} PROPERTY BUILDSYSTEM_TARGETS)
		foreach(target ${targets})
			get_target_property(type ${target} TYPE)
			if(NOT type STREQUAL "INTERFACE_LIBRARY")
				set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE OFF)
			endif()
		endforeach()
		get_property(subdirectories DIRECTORY ${directory} PROPERTY SUBDIRECTORIES)
		foreach(subdirectory ${subdirectories})
			vitaps2_disable_pic(${subdirectory})
		endforeach()
	endfunction()
	vitaps2_disable_pic(${CMAKE_CURRENT_SOURCE_DIR})
endif()

# Portable software GS renderer (no GPU API dependency).
add_library(gsh_software STATIC
	src/gs/GSH_Software.cpp
	src/gs/GSH_Software.h
	src/gs/SoftwareRasterizer.cpp
	src/gs/SoftwareRasterizer.h
	src/gs/ParallelRasterizer.cpp
	src/gs/ParallelRasterizer.h
	src/gs/TextureCache.cpp
	src/gs/TextureCache.h
	src/gs/GsSurface.h
	src/gs/GsMemory.h
	src/gs/GsDriver.h
	src/gs/GsBenchmark.cpp
	src/gs/GsBenchmark.h
)
target_include_directories(gsh_software PUBLIC src/gs ${PLAY_DIR}/Source)
target_link_libraries(gsh_software PUBLIC PlayCore)

# Frontend-independent glue shared by the Vita app and the host harness.
add_library(vitaps2_common STATIC
	src/common/EmuSession.cpp
	src/common/EmuSession.h
	src/common/FrameSink.h
)
target_include_directories(vitaps2_common PUBLIC src/common)
target_link_libraries(vitaps2_common PUBLIC gsh_software PlayCore)

if(VITA)
	include(cmake/VitaApp.cmake)
else()
	add_executable(vitaps2_host
		src/host/main_host.cpp
	)
	target_link_libraries(vitaps2_host PRIVATE vitaps2_common)

	enable_testing()
	add_executable(gs_software_tests tests/gs_software_tests.cpp)
	target_include_directories(gs_software_tests PRIVATE tests)
	target_link_libraries(gs_software_tests PRIVATE vitaps2_common)
	add_test(NAME gs_software_tests COMMAND gs_software_tests)

	add_executable(gs_benchmark tests/gs_benchmark.cpp)
	target_include_directories(gs_benchmark PRIVATE tests)
	target_link_libraries(gs_benchmark PRIVATE vitaps2_common)

	add_executable(gs_diff_tests tests/gs_diff_tests.cpp tests/reference/ReferenceRasterizer.cpp)
	target_include_directories(gs_diff_tests PRIVATE tests)
	target_link_libraries(gs_diff_tests PRIVATE vitaps2_common)
	add_test(NAME gs_diff_tests COMMAND gs_diff_tests)

	add_executable(gs_parallel_tests tests/gs_parallel_tests.cpp)
	target_include_directories(gs_parallel_tests PRIVATE tests)
	target_link_libraries(gs_parallel_tests PRIVATE vitaps2_common)
	add_test(NAME gs_parallel_tests COMMAND gs_parallel_tests)

	add_executable(jit_pool_tests tests/jit_pool_tests.cpp)
	target_include_directories(jit_pool_tests PRIVATE src/common)
	add_test(NAME jit_pool_tests COMMAND jit_pool_tests)

	# End to end: a generated PS2 executable drawing through DMA/GIF/GS.
	find_package(Python3 COMPONENTS Interpreter)
	if(Python3_FOUND)
		add_test(NAME elf_boot_test
			COMMAND ${CMAKE_COMMAND}
				-DPYTHON=${Python3_EXECUTABLE}
				-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
				-DRUNNER=$<TARGET_FILE:vitaps2_host>
				-P ${CMAKE_CURRENT_SOURCE_DIR}/tests/elf_boot_test.cmake)
	endif()
endif()
