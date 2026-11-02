# The EE must get its full clock: 294.912 MHz / 60 = 4915200 instructions per
# frame for code that never idles (an overflow once gave it 1/8 of that).
execute_process(COMMAND ${PYTHON} ${SOURCE_DIR}/tools/make_workload_elf.py ee_clock.elf int RESULT_VARIABLE r)
if(NOT r EQUAL 0)
	message(FATAL_ERROR "make_workload_elf.py failed")
endif()
execute_process(COMMAND ${EMULATOR} ${RUNNER} ee_clock.elf --frames 30 --timeout 100 OUTPUT_VARIABLE out ERROR_VARIABLE err)
string(REGEX MATCH "EE executed ([0-9]+)" m "${out}")
set(executed ${CMAKE_MATCH_1})
string(REGEX MATCH "vm frames: ([0-9]+)" m "${out}")
set(frames ${CMAKE_MATCH_1})
if(NOT executed OR NOT frames)
	message(FATAL_ERROR "no counters in output:\n${out}\n${err}")
endif()
math(EXPR perFrame "${executed} / ${frames}")
if(perFrame LESS 4500000 OR perFrame GREATER 5500000) # the count includes the unfinished frame at pause
	message(FATAL_ERROR "EE ran ${perFrame} instructions per frame (expected ~4915200)\n${out}")
endif()
