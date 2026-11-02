# EE FPU code (multiply/add with clamped operands) on random bit patterns:
# NaN/Inf/huge values exercise the operand clamping. Checks the checksum of
# the results. x86 and ARM differ (default NaN encodings), so the expected
# value is per architecture.
execute_process(COMMAND ${PYTHON} ${SOURCE_DIR}/tools/make_workload_elf.py ee_float${SUFFIX}.elf float --check RESULT_VARIABLE r)
if(NOT r EQUAL 0)
	message(FATAL_ERROR "make_workload_elf.py failed")
endif()
execute_process(COMMAND ${EMULATOR} ${RUNNER} ee_float${SUFFIX}.elf --frames 3 --timeout 100 OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT out MATCHES "EE s6 00000D0E s7 ${EXPECTED}")
	message(FATAL_ERROR "wrong result (expected ${EXPECTED}):\n${out}\n${err}")
endif()
