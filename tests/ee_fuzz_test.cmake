# Random EE integer code (tools/make_ee_fuzz_elf.py): the final checksum of all
# registers must match the value recorded from the reference (x86, no tracking).
execute_process(COMMAND ${PYTHON} ${SOURCE_DIR}/tools/make_ee_fuzz_elf.py ee_fuzz${SUFFIX}.elf ${SEED} RESULT_VARIABLE r)
if(NOT r EQUAL 0)
	message(FATAL_ERROR "make_ee_fuzz_elf.py failed")
endif()
execute_process(COMMAND ${EMULATOR} ${RUNNER} ee_fuzz${SUFFIX}.elf --frames 60 --timeout 100 OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT out MATCHES "EE s6 00000D0E s7 ${EXPECTED}")
	message(FATAL_ERROR "wrong result (expected ${EXPECTED}):\n${out}\n${err}")
endif()
