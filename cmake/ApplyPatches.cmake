# Applies patches/*.patch to external/Play (and its nested submodules) at
# configure time. Each patch is applied once; already-applied patches are
# detected with `git apply --reverse --check` so re-configuring is harmless.

function(vitaps2_apply_patches)
	find_package(Git REQUIRED)
	file(GLOB PATCHES ${CMAKE_SOURCE_DIR}/patches/*.patch)
	list(SORT PATCHES)
	foreach(PATCH ${PATCHES})
		# Patch file names are <order>-<repo>-<description>.patch where <repo> is
		# "Play", "CodeGen" or "Framework".
		get_filename_component(NAME ${PATCH} NAME)
		if(NAME MATCHES "^[0-9]+-CodeGen-")
			set(TARGET_DIR ${PLAY_DIR}/deps/CodeGen)
		elseif(NAME MATCHES "^[0-9]+-Framework-")
			set(TARGET_DIR ${PLAY_DIR}/deps/Framework)
		else()
			set(TARGET_DIR ${PLAY_DIR})
		endif()
		execute_process(
			COMMAND ${GIT_EXECUTABLE} apply --reverse --check ${PATCH}
			WORKING_DIRECTORY ${TARGET_DIR}
			RESULT_VARIABLE ALREADY_APPLIED OUTPUT_QUIET ERROR_QUIET)
		if(ALREADY_APPLIED EQUAL 0)
			continue()
		endif()
		execute_process(
			COMMAND ${GIT_EXECUTABLE} apply --whitespace=nowarn ${PATCH}
			WORKING_DIRECTORY ${TARGET_DIR}
			RESULT_VARIABLE RESULT)
		if(NOT RESULT EQUAL 0)
			message(FATAL_ERROR "Failed to apply ${NAME} in ${TARGET_DIR}")
		endif()
		message(STATUS "Applied ${NAME}")
	endforeach()
endfunction()
