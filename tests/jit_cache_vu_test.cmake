# Persistent JIT cache with VU1 microprograms (32-bit ARM): the VU1 test scene
# runs twice with the same cache file. Both frames must pass the pixel check;
# the second run must take blocks (EE, IOP and VU) from the cache.
# RUNNER_ARGS: extra runner flags (--mtvu: VU1 blocks compiled on their thread).
execute_process(COMMAND ${PYTHON} ${SOURCE_DIR}/tools/make_test_elf.py gs_test${SUFFIX}.elf --vu1 RESULT_VARIABLE r)
if(NOT r EQUAL 0)
	message(FATAL_ERROR "make_test_elf.py failed")
endif()
set(CACHE_FILE jit_cache${SUFFIX}.jit)
file(REMOVE ${CACHE_FILE})

foreach(RUN 1 2)
	execute_process(COMMAND ${EMULATOR} ${RUNNER} gs_test${SUFFIX}.elf --frames 30 --timeout 60 --out gs_test${SUFFIX}_${RUN}.ppm
		--jit-cache ${CACHE_FILE} ${RUNNER_ARGS} OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE r)
	if(NOT r EQUAL 0)
		message(FATAL_ERROR "run ${RUN}: emulator run failed:\n${out}\n${err}")
	endif()
	execute_process(COMMAND ${PYTHON} ${SOURCE_DIR}/tools/check_frame.py gs_test${SUFFIX}_${RUN}.ppm RESULT_VARIABLE r)
	if(NOT r EQUAL 0)
		message(FATAL_ERROR "run ${RUN}: frame check failed")
	endif()
	if(NOT out MATCHES "jit cache: ([0-9]+) loaded, ([0-9]+) hits, ([0-9]+) stored, ([0-9]+) uncacheable, ([0-9]+) failures")
		message(FATAL_ERROR "run ${RUN}: no jit cache statistics:\n${out}\n${err}")
	endif()
	message(STATUS "run ${RUN}: ${CMAKE_MATCH_1} loaded, ${CMAKE_MATCH_2} hits, ${CMAKE_MATCH_3} stored, ${CMAKE_MATCH_4} uncacheable, ${CMAKE_MATCH_5} failures")
	if(NOT CMAKE_MATCH_5 EQUAL 0)
		message(FATAL_ERROR "run ${RUN}: cache entries could not be applied")
	endif()
	if(RUN EQUAL 2 AND CMAKE_MATCH_2 EQUAL 0)
		message(FATAL_ERROR "run 2: expected blocks loaded from the cache")
	endif()
endforeach()
