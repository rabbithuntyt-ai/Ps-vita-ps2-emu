# PlayStation Vita application: eboot + VPK packaging.

include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "VitaPS2")
set(VITA_TITLEID  "VTPS20000")
set(VITA_VERSION  "00.10")

# ATTRIBUTE2=12 unlocks the extended memory budget the emulator needs.
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")

add_executable(vitaps2
	src/vita/main_vita.cpp
	src/vita/JitMemory.cpp
	src/vita/JitMemory.h
	src/vita/PH_Vita.cpp
	src/vita/PH_Vita.h
	src/vita/SH_Vita.cpp
	src/vita/SH_Vita.h
)
target_include_directories(vitaps2 PRIVATE src/vita src/common)
target_compile_options(vitaps2 PRIVATE -Wall)

target_link_libraries(vitaps2
	vitaps2_common
	vita2d
	freetype
	png
	jpeg
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
	ScePgf_stub
	ScePower_stub
	SceSysmodule_stub
	SceTouch_stub
)

# UNSAFE: the recompiler needs sceKernelAllocMemBlockForVM.
vita_create_self(eboot.bin vitaps2 UNSAFE)
vita_create_vpk(${VITA_APP_NAME}.vpk ${VITA_TITLEID} eboot.bin
	VERSION ${VITA_VERSION}
	NAME ${VITA_APP_NAME}
	FILE sce_sys/icon0.png sce_sys/icon0.png
	FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
	FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
	FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
	FILE ${PLAY_DIR}/GameConfig.xml GameConfig.xml
)
