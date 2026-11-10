# PlayStation Vita application: eboot + VPK packaging.

include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "VitaPS2")      # VPK file name (scripts and CI artifacts)
set(VITA_APP_TITLE "VitaStation2") # name on the home screen and LiveArea
set(VITA_TITLEID  "VTPS20000")
# A second install next to the main one (another session testing on the same Vita):
# -DVITAPS2_TITLEID=VTPS20001 -DVITAPS2_DATA_DIR=ux0:data/VitaPS2gp "-DVITAPS2_TITLE=VitaStation2 GP"
if(VITAPS2_TITLEID)
	set(VITA_TITLEID "${VITAPS2_TITLEID}")
endif()
if(VITAPS2_TITLE)
	set(VITA_APP_TITLE "${VITAPS2_TITLE}")
endif()
if(NOT VITAPS2_DATA_DIR)
	set(VITAPS2_DATA_DIR "ux0:data/VitaPS2")
endif()
set(VITA_VERSION  "00.10")

# ATTRIBUTE2=12 unlocks the extended memory budget the emulator needs.
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")

add_executable(vitaps2
	src/vita/main_vita.cpp
	src/vita/CpuScreen.cpp
	src/vita/CpuScreen.h
	src/vita/FastNew.cpp
	src/vita/Theme.cpp
	src/vita/JitMemory.cpp
	src/vita/JitMemory.h
	src/vita/PH_Vita.cpp
	src/vita/PH_Vita.h
	src/vita/SH_Vita.cpp
	src/vita/SH_Vita.h
)
target_include_directories(vitaps2 PRIVATE src/vita src/common)
target_compile_options(vitaps2 PRIVATE -Wall)
target_compile_definitions(vitaps2 PRIVATE VITAPS2_DATA_DIR="${VITAPS2_DATA_DIR}")
# vita-elf-create appends the SCE module info and import tables (a few KB,
# growing with the number of imports) right after the code segment and fails
# if the data segment starts too close. The SDK linker script skips
# __sce_headroom bytes before aligning the data segment; without it the gap
# depends on the code size. Costs address space only.
target_link_options(vitaps2 PRIVATE -Wl,--defsym,__sce_headroom=0x8000)
# Build identification shown in the UI, so photos and logs name the build.
# CI passes -DVITAPS2_COMMIT (git may refuse to run inside the SDK container).
if(NOT VITAPS2_COMMIT)
	execute_process(COMMAND git rev-parse --short HEAD WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
		OUTPUT_VARIABLE VITAPS2_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
if(NOT VITAPS2_COMMIT)
	set(VITAPS2_COMMIT "unknown")
endif()
target_compile_definitions(vitaps2 PRIVATE VITAPS2_BUILD="${VITA_VERSION}-${VITAPS2_COMMIT}")

target_link_libraries(vitaps2
	vitaps2_common
	vitaps2_ui
	gsh_hardware
	# vitaGL and what it needs (runtime shader compiler, NEON math...)
	vitaGL
	vitashark
	SceShaccCgExt
	mathneon
	taihen_stub
	SceShaccCg_stub
	z
	m
	# pthread comes from Threads::Threads (PlayCore); linking it again duplicates
	# the whole-archive pthread objects.
	SceAppMgr_stub
	SceAppUtil_stub
	SceAudio_stub
	SceCommonDialog_stub
	SceCtrl_stub
	SceDisplay_stub
	SceGxm_stub
	SceKernelDmacMgr_stub
	SceLibKernel_stub
	ScePower_stub
	SceSysmodule_stub
	SceTouch_stub
)

# UNSAFE: the recompiler needs sceKernelAllocMemBlockForVM.
vita_create_self(eboot.bin vitaps2 UNSAFE)
vita_create_vpk(${VITA_APP_NAME}.vpk ${VITA_TITLEID} eboot.bin
	VERSION ${VITA_VERSION}
	NAME ${VITA_APP_TITLE}
	FILE sce_sys/icon0.png sce_sys/icon0.png
	FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
	FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
	FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
	FILE ${PLAY_DIR}/GameConfig.xml GameConfig.xml
	FILE assets/gs_test.elf gs_test.elf
	FILE assets/game_profiles.ini game_profiles.ini
	FILE assets/brand/wordmark.a8 brand/wordmark.a8
	FILE assets/brand/digit.a8 brand/digit.a8
	FILE assets/brand/glow.a8 brand/glow.a8
	FILE assets/brand/buttons.a8 brand/buttons.a8
)
