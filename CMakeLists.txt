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
	src/gs/Re4Vu1Hle.cpp
	src/gs/Re4Vu1Hle.h
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
		src/gs/hw/GsHardwareBenchmark.cpp
		src/gs/hw/GsHardwareBenchmark.h
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

	# The VitaStation2 game list drawn on the host (tools/make_branding.py art).
	if(OSMESA_LIBRARY)
		add_executable(theme_preview tests/theme_preview.cpp src/vita/Theme.cpp)
		target_include_directories(theme_preview PRIVATE src/vita)
		target_link_libraries(theme_preview PRIVATE vitaps2_ui)
	endif()
endif()

# Frontend-independent glue shared by the Vita app and the host harness.
add_library(vitaps2_common STATIC
	src/common/EmuSession.cpp
	src/common/JitCacheSetup.cpp
	src/common/JitCacheSetup.h
	src/common/EmuSession.h
	src/common/FrameSink.h
	src/common/AudioStretcher.cpp
	src/common/AudioStretcher.h
	src/common/GameProfiles.cpp
	src/common/GameProfiles.h
	src/common/Mpeg2Decoder.cpp
	src/common/Mpeg2Decoder.h
	src/common/MpegHle.cpp
	src/common/MpegHle.h
	src/common/MpegShadow.cpp
	src/common/MpegShadow.h
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

	add_executable(spu_mixer_tests tests/spu_mixer_tests.cpp)
	target_link_libraries(spu_mixer_tests PRIVATE vitaps2_common)
	target_include_directories(spu_mixer_tests PRIVATE ${PLAY_DIR}/Source)
	add_test(NAME spu_mixer_tests COMMAND spu_mixer_tests)

	add_executable(spu_benchmark tests/spu_benchmark.cpp)
	target_link_libraries(spu_benchmark PRIVATE vitaps2_common)
	target_include_directories(spu_benchmark PRIVATE ${PLAY_DIR}/Source)

	add_executable(gs_benchmark tests/gs_benchmark.cpp)
	target_include_directories(gs_benchmark PRIVATE tests)
	target_link_libraries(gs_benchmark PRIVATE vitaps2_common)
	if(VITAPS2_HAS_GL)
		target_compile_definitions(gs_benchmark PRIVATE VITAPS2_HAS_GL)
		target_link_libraries(gs_benchmark PRIVATE gsh_hardware)
	endif()

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
	target_include_directories(ipu_tests PRIVATE src/common)
	add_test(NAME ipu_tests COMMAND ipu_tests)

	# Native MPEG-2 decoder (movie HLE): decodes a .pss given as argument
	# (skipped without one) and writes PPM frames next to it.
	add_executable(mpeg2_decoder_tests tests/mpeg2_decoder_tests.cpp)
	target_link_libraries(mpeg2_decoder_tests PRIVATE vitaps2_common)

	# Row-run GS transfer writes (patch 0061) against Play!'s per pixel ones.
	add_executable(gs_transfer_tests tests/gs_transfer_tests.cpp)
	target_link_libraries(gs_transfer_tests PRIVATE vitaps2_common)
	add_test(NAME gs_transfer_tests COMMAND gs_transfer_tests)

	# NEON kernels against the scalar ones (build for ARM to test NEON).
	add_executable(mpeg2_simd_tests tests/mpeg2_simd_tests.cpp)
	target_include_directories(mpeg2_simd_tests PRIVATE src/common)
	add_test(NAME mpeg2_simd_tests COMMAND mpeg2_simd_tests)

	add_executable(game_profiles_tests tests/game_profiles_tests.cpp)
	target_link_libraries(game_profiles_tests PRIVATE vitaps2_common)

	# Persistent JIT cache: file format and relocation helpers (any host).
	# The Vita JIT's ARM code for EE blocks from a RAM dump, on any host (tools/jit_arm_report.py)
	add_executable(jit_arm_dump tools/jit_arm_dump.cpp)
	add_executable(vu_arm_dump tools/vu_arm_dump.cpp)
	target_link_libraries(vu_arm_dump PRIVATE PlayCore)
	target_link_libraries(jit_arm_dump PRIVATE PlayCore)

	add_executable(jit_cache_tests tests/jit_cache_tests.cpp)
	target_link_libraries(jit_cache_tests PRIVATE PlayCore)
	add_test(NAME jit_cache_tests COMMAND jit_cache_tests)

	add_executable(auto_tuner_tests tests/auto_tuner_tests.cpp)
	target_include_directories(auto_tuner_tests PRIVATE src/common)
	add_test(NAME auto_tuner_tests COMMAND auto_tuner_tests)
	add_test(NAME game_profiles_tests COMMAND game_profiles_tests ${CMAKE_CURRENT_SOURCE_DIR}/assets/game_profiles.ini)

	add_executable(audio_stretch_tests tests/audio_stretch_tests.cpp)
	target_link_libraries(audio_stretch_tests PRIVATE vitaps2_common)
	add_test(NAME audio_stretch_tests COMMAND audio_stretch_tests)

	# Play!'s recompiler test suites: CodeGen (JIT backend, including the
	# AArch32 backend under qemu on ARM cross builds) and VU microcode.
	if(TARGET CodeGenTestSuite)
		set_target_properties(CodeGenTestSuite PROPERTIES EXCLUDE_FROM_ALL FALSE)
		add_test(NAME codegen_tests COMMAND CodeGenTestSuite)
	endif()
	add_subdirectory(${PLAY_DIR}/tools/VuTest ${CMAKE_CURRENT_BINARY_DIR}/VuTest)
	target_include_directories(VuTest PRIVATE ${PLAY_DIR}/Source/app_shared)
	if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
		# The "Safe JIT" mode (no NEON/VFP register allocation) must stay correct.
		if(TARGET CodeGenTestSuite)
			add_test(NAME codegen_tests_safe_jit COMMAND CodeGenTestSuite)
			set_tests_properties(codegen_tests_safe_jit PROPERTIES ENVIRONMENT CODEGEN_AARCH32_CONSERVATIVE=1)
		endif()
		add_test(NAME VuTest_safe_jit COMMAND VuTest)
		set_tests_properties(VuTest_safe_jit PROPERTIES ENVIRONMENT CODEGEN_AARCH32_CONSERVATIVE=1)
		# Chained frames (default in the Vita app): shared frame across linked blocks.
		if(TARGET CodeGenTestSuite)
			add_test(NAME codegen_tests_chained COMMAND CodeGenTestSuite)
			set_tests_properties(codegen_tests_chained PROPERTIES ENVIRONMENT CODEGEN_AARCH32_CHAINED=1)
		endif()
		add_test(NAME VuTest_chained COMMAND VuTest)
		set_tests_properties(VuTest_chained PROPERTIES ENVIRONMENT CODEGEN_AARCH32_CHAINED=1)
	endif()

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
		# Same picture drawn by a VU1 microprogram (VIF1, VU recompiler, PATH1)
		add_test(NAME elf_boot_test_vu1
			COMMAND ${CMAKE_COMMAND}
				-DPYTHON=${Python3_EXECUTABLE}
				-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
				-DRUNNER=$<TARGET_FILE:vitaps2_host>
				"-DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
				-DELF_ARGS=--vu1
				-DSUFFIX=_vu1
				-P ${CMAKE_CURRENT_SOURCE_DIR}/tests/elf_boot_test.cmake)
		# ...with VU1 on its own thread
		add_test(NAME elf_boot_test_mtvu
			COMMAND ${CMAKE_COMMAND}
				-DPYTHON=${Python3_EXECUTABLE}
				-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
				-DRUNNER=$<TARGET_FILE:vitaps2_host>
				"-DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
				-DELF_ARGS=--vu1
				-DRUNNER_ARGS=--mtvu
				-DSUFFIX=_mtvu
				-P ${CMAKE_CURRENT_SOURCE_DIR}/tests/elf_boot_test.cmake)
		# VU1 program spinning until the EE feeds it after a vblank (must not deadlock)
		add_test(NAME elf_boot_test_vu1_wait
			COMMAND ${CMAKE_COMMAND}
				-DPYTHON=${Python3_EXECUTABLE}
				-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
				-DRUNNER=$<TARGET_FILE:vitaps2_host>
				"-DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
				-DELF_ARGS=--vu1-wait
				-DSUFFIX=_vu1_wait
				-P ${CMAKE_CURRENT_SOURCE_DIR}/tests/elf_boot_test.cmake)
		add_test(NAME elf_boot_test_vu1_wait_mtvu
			COMMAND ${CMAKE_COMMAND}
				-DPYTHON=${Python3_EXECUTABLE}
				-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
				-DRUNNER=$<TARGET_FILE:vitaps2_host>
				"-DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
				-DELF_ARGS=--vu1-wait
				-DRUNNER_ARGS=--mtvu
				-DSUFFIX=_vu1_wait_mtvu
				-P ${CMAKE_CURRENT_SOURCE_DIR}/tests/elf_boot_test.cmake)
		# ...with SPU2 mixing (and VU1) on worker threads
		add_test(NAME elf_boot_test_mtspu
			COMMAND ${CMAKE_COMMAND}
				-DPYTHON=${Python3_EXECUTABLE}
				-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
				-DRUNNER=$<TARGET_FILE:vitaps2_host>
				"-DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
				-DELF_ARGS=--vu1-wait
				"-DRUNNER_ARGS=--mtspu;--mtvu"
				-DSUFFIX=_mtspu
				-P ${CMAKE_CURRENT_SOURCE_DIR}/tests/elf_boot_test.cmake)
		set_tests_properties(elf_boot_test_vu1_wait elf_boot_test_vu1_wait_mtvu elf_boot_test_mtspu PROPERTIES TIMEOUT 180)
		# EE loads/stores of every size, mapped and through the handlers
		set(EE_MEMORY_TESTS ee_memory_test)
		if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
			list(APPEND EE_MEMORY_TESTS ee_memory_test_chained ee_memory_test_safe_jit)
		endif()
		foreach(EE_MEMORY_TEST ${EE_MEMORY_TESTS})
			add_test(NAME ${EE_MEMORY_TEST}
				COMMAND ${CMAKE_COMMAND}
					-DPYTHON=${Python3_EXECUTABLE}
					-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
					-DRUNNER=$<TARGET_FILE:vitaps2_host>
					"-DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
					-DSUFFIX=_${EE_MEMORY_TEST}
					-P ${CMAKE_CURRENT_SOURCE_DIR}/tests/ee_memory_test.cmake)
		endforeach()
		add_test(NAME ee_clock_test
			COMMAND ${CMAKE_COMMAND}
				-DPYTHON=${Python3_EXECUTABLE}
				-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
				-DRUNNER=$<TARGET_FILE:vitaps2_host>
				"-DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
				-P ${CMAKE_CURRENT_SOURCE_DIR}/tests/ee_clock_test.cmake)
		# EE FPU with operand clamping (expected checksum per architecture)
		if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
			set(EE_FLOAT_EXPECTED 7D9211C6)
			set(EE_FLOAT_MODES default chained safe)
		else()
			set(EE_FLOAT_EXPECTED DC5121AE)
			set(EE_FLOAT_MODES default)
		endif()
		foreach(EE_FLOAT_MODE ${EE_FLOAT_MODES})
			add_test(NAME ee_float_test_${EE_FLOAT_MODE}
				COMMAND ${CMAKE_COMMAND}
					-DPYTHON=${Python3_EXECUTABLE}
					-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
					-DRUNNER=$<TARGET_FILE:vitaps2_host>
					"-DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
					-DEXPECTED=${EE_FLOAT_EXPECTED}
					-DSUFFIX=_${EE_FLOAT_MODE}
					-P ${CMAKE_CURRENT_SOURCE_DIR}/tests/ee_float_test.cmake)
			if(EE_FLOAT_MODE STREQUAL "chained")
				set_tests_properties(ee_float_test_${EE_FLOAT_MODE} PROPERTIES ENVIRONMENT CODEGEN_AARCH32_CHAINED=1)
			elseif(EE_FLOAT_MODE STREQUAL "safe")
				set_tests_properties(ee_float_test_${EE_FLOAT_MODE} PROPERTIES ENVIRONMENT "CODEGEN_AARCH32_CONSERVATIVE=1;CODEGEN_AARCH32_NO_GUESTMEMOPS=1;PLAY_NO_SIGNEXT_TRACKING=1")
			endif()
		endforeach()
		add_test(NAME ipu_decode_test
			COMMAND ${CMAKE_COMMAND}
				-DPYTHON=${Python3_EXECUTABLE}
				-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
				-DRUNNER=$<TARGET_FILE:vitaps2_host>
				"-DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
				-DEXPECTED=268B3623
				-P ${CMAKE_CURRENT_SOURCE_DIR}/tests/ipu_decode_test.cmake)
		# Random EE integer code checked against reference checksums
		# (seed, expected, generator option: --loops adds counted loops)
		# Tier 2 (src: Play ee/Tier2.cpp, --tier2: a region at every block start, interpreted
		# on x86): the same programs plus calls (inlining, return checks) and scratchpad accesses
		foreach(EE_FUZZ_CASE "1;19501935;-" "3;8006134B;-" "100;D477E7D3;--loops" "300;91E0B378;--calls" "320;D672F8AA;--calls --loops --likely" "330;FE6D9BC7;--spr --calls")
			list(GET EE_FUZZ_CASE 0 EE_FUZZ_SEED)
			list(GET EE_FUZZ_CASE 1 EE_FUZZ_EXPECTED)
			list(GET EE_FUZZ_CASE 2 EE_FUZZ_ARGS)
			if(EE_FUZZ_ARGS STREQUAL "-")
				set(EE_FUZZ_ARGS "")
			endif()
			add_test(NAME ee_fuzz_test_${EE_FUZZ_SEED}_tier2
				COMMAND ${CMAKE_COMMAND}
					-DPYTHON=${Python3_EXECUTABLE}
					-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
					-DRUNNER=$<TARGET_FILE:vitaps2_host>
					"-DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
					-DSEED=${EE_FUZZ_SEED}
					-DEXPECTED=${EE_FUZZ_EXPECTED}
					"-DFUZZ_ARGS=${EE_FUZZ_ARGS}"
					-DRUNNER_ARGS=--tier2
					-DSUFFIX=_${EE_FUZZ_SEED}_tier2
					-P ${CMAKE_CURRENT_SOURCE_DIR}/tests/ee_fuzz_test.cmake)
		endforeach()
		foreach(EE_FUZZ_CASE "1;19501935;-" "2;DE6B896F;-" "3;8006134B;-" "100;D477E7D3;--loops" "101;54F8303C;--loops")
			list(GET EE_FUZZ_CASE 0 EE_FUZZ_SEED)
			list(GET EE_FUZZ_CASE 1 EE_FUZZ_EXPECTED)
			list(GET EE_FUZZ_CASE 2 EE_FUZZ_ARGS)
			if(EE_FUZZ_ARGS STREQUAL "-")
				set(EE_FUZZ_ARGS "")
			endif()
			set(EE_FUZZ_MODES default)
			if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
				list(APPEND EE_FUZZ_MODES chained safe)
			endif()
			foreach(EE_FUZZ_MODE ${EE_FUZZ_MODES})
				set(EE_FUZZ_NAME ee_fuzz_test_${EE_FUZZ_SEED}_${EE_FUZZ_MODE})
				add_test(NAME ${EE_FUZZ_NAME}
					COMMAND ${CMAKE_COMMAND}
						-DPYTHON=${Python3_EXECUTABLE}
						-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
						-DRUNNER=$<TARGET_FILE:vitaps2_host>
						"-DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
						-DSEED=${EE_FUZZ_SEED}
						-DEXPECTED=${EE_FUZZ_EXPECTED}
						-DFUZZ_ARGS=${EE_FUZZ_ARGS}
						-DSUFFIX=_${EE_FUZZ_SEED}_${EE_FUZZ_MODE}
						-P ${CMAKE_CURRENT_SOURCE_DIR}/tests/ee_fuzz_test.cmake)
				if(EE_FUZZ_MODE STREQUAL "chained")
					set_tests_properties(${EE_FUZZ_NAME} PROPERTIES ENVIRONMENT CODEGEN_AARCH32_CHAINED=1)
				elseif(EE_FUZZ_MODE STREQUAL "safe")
					set_tests_properties(${EE_FUZZ_NAME} PROPERTIES ENVIRONMENT "CODEGEN_AARCH32_CONSERVATIVE=1;CODEGEN_AARCH32_NO_GUESTMEMOPS=1;PLAY_NO_SIGNEXT_TRACKING=1")
				endif()
			endforeach()
		endforeach()
		if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
			# Persistent JIT cache with real generated code: store, then load
			# (default and chained frames, whose links use thunks).
			# ...with VU1 microprograms, single threaded and with VU1 on its thread
			foreach(JIT_CACHE_VU_MODE single mtvu)
				if(JIT_CACHE_VU_MODE STREQUAL "mtvu")
					set(JIT_CACHE_VU_ARGS --mtvu)
				else()
					set(JIT_CACHE_VU_ARGS "")
				endif()
				add_test(NAME jit_cache_vu_test_${JIT_CACHE_VU_MODE}
					COMMAND ${CMAKE_COMMAND}
						-DPYTHON=${Python3_EXECUTABLE}
						-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
						-DRUNNER=$<TARGET_FILE:vitaps2_host>
						"-DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
						-DRUNNER_ARGS=${JIT_CACHE_VU_ARGS}
						-DSUFFIX=_jitvu_${JIT_CACHE_VU_MODE}
						-P ${CMAKE_CURRENT_SOURCE_DIR}/tests/jit_cache_vu_test.cmake)
			endforeach()
			foreach(JIT_CACHE_CASE "1;19501935;-;default" "100;D477E7D3;--loops;default" "100;D477E7D3;--loops;chained")
				list(GET JIT_CACHE_CASE 0 JIT_CACHE_SEED)
				list(GET JIT_CACHE_CASE 1 JIT_CACHE_EXPECTED)
				list(GET JIT_CACHE_CASE 2 JIT_CACHE_ARGS)
				list(GET JIT_CACHE_CASE 3 JIT_CACHE_MODE)
				if(JIT_CACHE_ARGS STREQUAL "-")
					set(JIT_CACHE_ARGS "")
				endif()
				set(JIT_CACHE_NAME jit_cache_ee_test_${JIT_CACHE_SEED}_${JIT_CACHE_MODE})
				add_test(NAME ${JIT_CACHE_NAME}
					COMMAND ${CMAKE_COMMAND}
						-DPYTHON=${Python3_EXECUTABLE}
						-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
						-DRUNNER=$<TARGET_FILE:vitaps2_host>
						"-DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
						-DSEED=${JIT_CACHE_SEED}
						-DEXPECTED=${JIT_CACHE_EXPECTED}
						-DFUZZ_ARGS=${JIT_CACHE_ARGS}
						-DSUFFIX=_${JIT_CACHE_SEED}_${JIT_CACHE_MODE}
						-P ${CMAKE_CURRENT_SOURCE_DIR}/tests/jit_cache_test.cmake)
				if(JIT_CACHE_MODE STREQUAL "chained")
					set_tests_properties(${JIT_CACHE_NAME} PROPERTIES ENVIRONMENT CODEGEN_AARCH32_CHAINED=1)
				endif()
			endforeach()
		endif()
		if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
			set_tests_properties(ee_memory_test_chained PROPERTIES ENVIRONMENT CODEGEN_AARCH32_CHAINED=1)
			set_tests_properties(ee_memory_test_safe_jit PROPERTIES ENVIRONMENT "CODEGEN_AARCH32_CONSERVATIVE=1;CODEGEN_AARCH32_NO_GUESTMEMOPS=1")
		endif()
		if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
			# Everything the app enables by default: threaded VU1 and SPU, chained frames
			add_test(NAME elf_boot_test_vita_defaults
				COMMAND ${CMAKE_COMMAND}
					-DPYTHON=${Python3_EXECUTABLE}
					-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
					-DRUNNER=$<TARGET_FILE:vitaps2_host>
					"-DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
					-DELF_ARGS=--vu1-wait
					"-DRUNNER_ARGS=--mtspu;--mtvu"
					-DSUFFIX=_vita_defaults
					-P ${CMAKE_CURRENT_SOURCE_DIR}/tests/elf_boot_test.cmake)
			set_tests_properties(elf_boot_test_vita_defaults PROPERTIES TIMEOUT 180 ENVIRONMENT CODEGEN_AARCH32_CHAINED=1)
		endif()
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
