# IPU end to end: a generated ELF decodes an intra picture with IDEC through
# DMA4/DMA3 and polls IPU_CTRL, as MPEG players do; checks the first picture.
execute_process(COMMAND ${PYTHON} ${SOURCE_DIR}/tools/make_ipu_elf.py ipu_decode.elf 240 RESULT_VARIABLE r)
if(NOT r EQUAL 0)
	message(FATAL_ERROR "make_ipu_elf.py failed")
endif()
execute_process(COMMAND ${EMULATOR} ${RUNNER} ipu_decode.elf --frames 10 --timeout 100 OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT out MATCHES "EE s6 [0-9A-F]+ s7 ${EXPECTED}")
	message(FATAL_ERROR "wrong result:\n${out}\n${err}")
endif()
if(out MATCHES "EE s6 00000000")
	message(FATAL_ERROR "no picture decoded:\n${out}\n${err}")
endif()
