# Generates the test ELF, runs it headless and checks a few output pixels.
# ELF_ARGS: generator flags (e.g. --vu1), as a ;-list.
execute_process(COMMAND ${PYTHON} ${SOURCE_DIR}/tools/make_test_elf.py gs_test${SUFFIX}.elf ${ELF_ARGS} RESULT_VARIABLE r)
if(NOT r EQUAL 0)
	message(FATAL_ERROR "make_test_elf.py failed")
endif()
# RUNNER_ARGS: extra runner flags (e.g. --hw), as a ;-list.
# EMULATOR: runs the runner on cross builds (qemu-arm ...), as a ;-list.
execute_process(COMMAND ${EMULATOR} ${RUNNER} gs_test${SUFFIX}.elf --frames 30 --timeout 60 --out gs_test${SUFFIX}.ppm ${RUNNER_ARGS} RESULT_VARIABLE r)
if(NOT r EQUAL 0)
	message(FATAL_ERROR "emulator run failed")
endif()
execute_process(COMMAND ${PYTHON} ${SOURCE_DIR}/tools/check_frame.py gs_test${SUFFIX}.ppm RESULT_VARIABLE r)
if(NOT r EQUAL 0)
	message(FATAL_ERROR "frame check failed")
endif()
