# EE loads and stores of every size through the page table and through the
# memory handlers: runs a generated ELF and checks its final checksum.
execute_process(COMMAND ${PYTHON} ${SOURCE_DIR}/tools/make_membench_elf.py ee_memory${SUFFIX}.elf 20 RESULT_VARIABLE r)
if(NOT r EQUAL 0)
	message(FATAL_ERROR "make_membench_elf.py failed")
endif()
# The program draws nothing: the runner's exit code is not checked.
execute_process(COMMAND ${EMULATOR} ${RUNNER} ee_memory${SUFFIX}.elf --frames 120 --timeout 100 OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT out MATCHES "EE s6 00000D0E s7 3460654F")
	message(FATAL_ERROR "wrong result:\n${out}\n${err}")
endif()
