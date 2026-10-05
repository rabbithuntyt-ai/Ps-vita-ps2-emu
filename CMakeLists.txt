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
	src/gs/GSH_Primitives.cpp
	src/gs/GSH_Primitives.h
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
	src/common/ThreadProfiler.cpp
	src/common/ThreadProfiler.h
)
target_include_directories(gsh_software PUBLIC src/gs src/common ${PLAY_DIR}/Source)
target_link_libraries(gsh_software PUBLIC PlayCore)

# GPU GS renderer (OpenGL fixed-function subset: vitaGL on the Vita, Mesa on
# the host for tests).
if(VITA)
	set(VITAPS2_HAS_GL ON)
else()
	find_library(OSMESA_LIBRARY OSMesa)
	if(OSMESA_LIBRARY)
		set(VITAPS2_HAS_GL ON)
	endif()
endif()
if(VITAPS2_HAS_GL)
	add_library(gsh_hardware STATIC
		src/gs/hw/GSH_Hardware.cpp
		src/gs/hw/GSH_Hardware.h
		src/gs/hw/GpuGl.h
	)
	target_include_directories(gsh_hardware PUBLIC src/gs/hw)
	target_link_libraries(gsh_hardware PUBLIC gsh_software)
	if(NOT VITA)
		target_link_libraries(gsh_hardware PUBLIC ${OSMESA_LIBRARY})
	endif()

	# 2D drawing for menus and overlays, on the same GL context.
	add_library(vitaps2_ui STATIC
		src/ui/Gfx.cpp
		src/ui/Gfx.h
		src/ui/FontData.h
	)
	target_include_directories(vitaps2_ui PUBLIC src/ui)
	target_link_libraries(vitaps2_ui PUBLIC gsh_hardware)
endif()

# Frontend-independent glue shared by the Vita app and the host harness.
add_library(vitaps2_common STATIC
	src/common/EmuSession.cpp
	src/common/EmuSession.h
	src/common/FrameSink.h
	src/common/AudioStretcher.cpp
	src/common/AudioStretcher.h
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
	if(TARGET gsh_hardware)
		target_link_libraries(vitaps2_host PRIVATE gsh_hardware)
		target_compile_definitions(vitaps2_host PRIVATE VITAPS2_HAS_GL)
	endif()

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

	if(TARGET gsh_hardware)
		add_executable(gs_hardware_tests tests/gs_hardware_tests.cpp)
		target_include_directories(gs_hardware_tests PRIVATE tests)
		target_link_libraries(gs_hardware_tests PRIVATE vitaps2_common gsh_hardware)
		add_test(NAME gs_hardware_tests COMMAND gs_hardware_tests)

		add_executable(ui_render_test tests/ui_render_test.cpp)
		target_link_libraries(ui_render_test PRIVATE vitaps2_ui)
		add_test(NAME ui_render_test COMMAND ui_render_test)
	endif()

	add_executable(idct_tests tests/idct_tests.cpp)
	target_link_libraries(idct_tests PRIVATE PlayCore)
	add_test(NAME idct_tests COMMAND idct_tests)

	add_executable(ipu_tests tests/ipu_tests.cpp)
	target_link_libraries(ipu_tests PRIVATE PlayCore)
	add_test(NAME ipu_tests COMMAND ipu_tests)

	add_executable(audio_stretch_tests tests/audio_stretch_tests.cpp)
	target_link_libraries(audio_stretch_tests PRIVATE vitaps2_common)
	add_test(NAME audio_stretch_tests COMMAND audio_stretch_tests)

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
				"-DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
				-P ${CMAKE_CURRENT_SOURCE_DIR}/tests/elf_boot_test.cmake)
		if(TARGET gsh_hardware)
			add_test(NAME elf_boot_test_hw
				COMMAND ${CMAKE_COMMAND}
					-DPYTHON=${Python3_EXECUTABLE}
					-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
					-DRUNNER=$<TARGET_FILE:vitaps2_host>
					-DRUNNER_ARGS=--hw
					-DSUFFIX=_hw
					-P ${CMAKE_CURRENT_SOURCE_DIR}/tests/elf_boot_test.cmake)
		endif()
	endif()
endif()
