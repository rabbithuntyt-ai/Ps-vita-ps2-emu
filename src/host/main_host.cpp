// Headless host runner: boots a PS2 executable or disc image with the same
// emulation core and software GS renderer used on the Vita, runs for a number
// of emulated frames and writes the last displayed frame to a PPM file.
//
//   vitaps2_host <game.elf|game.iso> [--frames N] [--timeout SECONDS] [--out frame.ppm]
//                [--gs-threads N] [--interlaced] [--frameskip N] [--hw] [--mtvu]
//                [--mtspu] [--jit-cache FILE] [--press FRAME:BUTTONS[:HOLD],...]
//                [--mpeg-hle] [--shot-every N] [--shot-dir DIR] [--stats FILE]
//                [--gs-diag LINES]
//
// --hw renders with the GPU renderer on an offscreen Mesa context, pumped
// from the main thread exactly like the Vita frontend does.
//
// Game debugging without a Vita (same core and renderer as the Vita build):
//   --press      scripted controller input by emulated frame (vblank), e.g.
//                "600:start,900:cross,960:down+cross:30" (HOLD frames, default 8);
//                buttons: up down left right start select cross circle square
//                triangle l1 l2 l3 r1 r2 r3
//   --mpeg-hle   native movie decoding (autostart key mpeg_hle=1 on the Vita),
//                log in mpeghle.txt
//   --shot-every write the displayed frame every N emulated frames
//                (DIR/shot_<frame>.ppm)
//   --stats      one CSV row per emulated second (60 frames): fps, emulation
//                thread shares, EE pc, movie pictures, and with --hw the GPU
//                renderer times and the last flipped frame's GS counters
//   --gs-diag    GPU renderer diagnostics (draws, transfers per flip) in gsdiag.txt
//
// Exit code is 0 when at least one frame was presented.

#include "ee/Tier2.h"
#include "ee/VuBasicBlock.h"
#include "ee/VUShared.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>
#include "EmuSession.h"
#include "JitCacheSetup.h"
#include "EmuProfile.h"
#include "ThreadProfiler.h"
#include "PS2VM.h"
#include "PadHandler.h"
#include "ControllerInfo.h"
#include "MpegHle.h"
#include "ee/Vif.h"
#include "Vu1Stats.h"
#include "Vu1Verify.h"
#include "Re4Skin.h"
#include "Re4Vu1Hle.h"
#include <algorithm>
#include <atomic>
#include <map>
#ifdef VITAPS2_HAS_GL
#include <GL/osmesa.h>
#include "GSH_Hardware.h"
#include "gs/GsTransferWrite.h"
#include "MailBox.h"
#endif

namespace
{
	bool WritePpm(const std::string& path, const std::vector<uint32_t>& pixels, uint32_t width, uint32_t height)
	{
		FILE* file = std::fopen(path.c_str(), "wb");
		if(!file) return false;
		std::fprintf(file, "P6\n%u %u\n255\n", width, height);
		std::vector<uint8_t> row(width * 3);
		for(uint32_t y = 0; y < height; y++)
		{
			for(uint32_t x = 0; x < width; x++)
			{
				uint32_t c = pixels[x + y * width];
				row[x * 3 + 0] = c & 0xFF;
				row[x * 3 + 1] = (c >> 8) & 0xFF;
				row[x * 3 + 2] = (c >> 16) & 0xFF;
			}
			std::fwrite(row.data(), 1, row.size(), file);
		}
		std::fclose(file);
		return true;
	}

	// Controller driven by a script of presses keyed by emulated frame (vblank).
	class CPH_Script : public CPadHandler
	{
	public:
		struct PRESS
		{
			uint64_t frame = 0;
			uint32_t hold = 8;
			std::vector<PS2::CControllerInfo::BUTTON> buttons;
			std::vector<std::pair<PS2::CControllerInfo::BUTTON, uint8>> axes; //lup, ldown, lleft, lright: left stick pushed fully
		};

		explicit CPH_Script(std::vector<PRESS> presses)
		    : m_presses(std::move(presses))
		{
		}

		void Update(uint8* ram) override
		{
			uint64_t frame = m_vblanks++;
			bool held[PS2::CControllerInfo::MAX_BUTTONS] = {};
			uint8 axes[PS2::CControllerInfo::MAX_BUTTONS];
			std::fill(std::begin(axes), std::end(axes), 0x7F);
			for(const auto& press : m_presses)
			{
				if((frame < press.frame) || (frame >= press.frame + press.hold)) continue;
				for(auto button : press.buttons) held[button] = true;
				for(const auto& [axis, value] : press.axes) axes[axis] = value;
			}
			for(auto* listener : m_interfaces)
			{
				for(unsigned int i = 0; i < PS2::CControllerInfo::MAX_BUTTONS; i++)
				{
					auto button = static_cast<PS2::CControllerInfo::BUTTON>(i);
					if(PS2::CControllerInfo::IsAxis(button)) listener->SetAxisState(0, button, axes[i], ram);
					else listener->SetButtonState(0, button, held[i], ram);
				}
			}
		}

	private:
		std::vector<PRESS> m_presses;
		std::atomic<uint64_t> m_vblanks{0};
	};

	// "600:start,900:cross,960:down+cross:30"
	bool ParsePresses(const std::string& text, std::vector<CPH_Script::PRESS>& presses)
	{
		using PS2::CControllerInfo;
		static const std::pair<const char*, CControllerInfo::BUTTON> names[] = {
		    {"up", CControllerInfo::DPAD_UP}, {"down", CControllerInfo::DPAD_DOWN}, {"left", CControllerInfo::DPAD_LEFT},
		    {"right", CControllerInfo::DPAD_RIGHT}, {"start", CControllerInfo::START}, {"select", CControllerInfo::SELECT},
		    {"cross", CControllerInfo::CROSS}, {"circle", CControllerInfo::CIRCLE}, {"square", CControllerInfo::SQUARE},
		    {"triangle", CControllerInfo::TRIANGLE}, {"l1", CControllerInfo::L1}, {"l2", CControllerInfo::L2},
		    {"l3", CControllerInfo::L3}, {"r1", CControllerInfo::R1}, {"r2", CControllerInfo::R2}, {"r3", CControllerInfo::R3}};
		size_t start = 0;
		while(start < text.size())
		{
			size_t end = text.find(',', start);
			if(end == std::string::npos) end = text.size();
			std::string item = text.substr(start, end - start);
			start = end + 1;
			size_t colon = item.find(':');
			if(colon == std::string::npos) return false;
			CPH_Script::PRESS press;
			press.frame = std::strtoull(item.c_str(), nullptr, 10);
			std::string rest = item.substr(colon + 1);
			size_t holdColon = rest.find(':');
			if(holdColon != std::string::npos)
			{
				press.hold = std::atoi(rest.c_str() + holdColon + 1);
				rest.resize(holdColon);
			}
			size_t nameStart = 0;
			while(nameStart <= rest.size())
			{
				size_t plus = rest.find('+', nameStart);
				if(plus == std::string::npos) plus = rest.size();
				std::string name = rest.substr(nameStart, plus - nameStart);
				nameStart = plus + 1;
				bool found = false;
				if(name == "lup") press.axes.emplace_back(CControllerInfo::ANALOG_LEFT_Y, 0x00), found = true;
				if(name == "ldown") press.axes.emplace_back(CControllerInfo::ANALOG_LEFT_Y, 0xFF), found = true;
				if(name == "lleft") press.axes.emplace_back(CControllerInfo::ANALOG_LEFT_X, 0x00), found = true;
				if(name == "lright") press.axes.emplace_back(CControllerInfo::ANALOG_LEFT_X, 0xFF), found = true;
				for(const auto& [buttonName, button] : names)
				{
					if(name != buttonName) continue;
					press.buttons.push_back(button);
					found = true;
				}
				if(!found) return false;
			}
			presses.push_back(press);
		}
		return true;
	}
}

int main(int argc, char** argv)
{
	if(argc < 2)
	{
		std::fprintf(stderr, "usage: %s <game.elf|game.iso> [--frames N] [--timeout SECONDS] [--out frame.ppm]\n", argv[0]);
		return 2;
	}

	std::string bootPath = argv[1];
	uint64_t targetFrames = 120;
	int timeoutSeconds = 60;
	std::string outPath = "frame.ppm";
	uint32_t gsThreads = 1;
	bool interlaced = false;
	uint32_t frameSkip = 0;
	bool hardware = false;
	bool threadedVu1 = false;
	bool threadedSpu = false;
	bool benchExit = false; //exit from the emulation thread right at the last frame (deterministic for measurements)
	std::string jitCachePath; //persistent JIT cache file (32-bit ARM only)
	std::vector<CPH_Script::PRESS> presses;
	bool mpegHleEnabled = false;
	bool vitaJit = false; // the Vita app's JIT modes (CEmuSession::SetSafeJit(false)): chained frames, indirect dispatch...
	uint64_t shotEvery = 0;
	std::string shotDir = ".";
	std::string statsPath;
	uint32_t gsDiagLines = 0;
	uint64_t hotFrom = ~0ULL; //sample the EE PC from this frame on (--hot-from)
	std::string ramDumpPath;
	std::string loadStatePath; //--load-state: a save state (the Vita's too), loaded at frame 120
	for(int i = 2; i < argc; i++)
	{
		if(!std::strcmp(argv[i], "--frames") && (i + 1 < argc)) targetFrames = std::strtoull(argv[++i], nullptr, 10);
		else if(!std::strcmp(argv[i], "--timeout") && (i + 1 < argc)) timeoutSeconds = std::atoi(argv[++i]);
		else if(!std::strcmp(argv[i], "--out") && (i + 1 < argc)) outPath = argv[++i];
		else if(!std::strcmp(argv[i], "--gs-threads") && (i + 1 < argc)) gsThreads = std::atoi(argv[++i]);
		else if(!std::strcmp(argv[i], "--interlaced")) interlaced = true;
		else if(!std::strcmp(argv[i], "--frameskip") && (i + 1 < argc)) frameSkip = std::atoi(argv[++i]);
		else if(!std::strcmp(argv[i], "--hw")) hardware = true;
		else if(!std::strcmp(argv[i], "--mtvu")) threadedVu1 = true;
		else if(!std::strcmp(argv[i], "--mtspu")) threadedSpu = true;
		else if(!std::strcmp(argv[i], "--bench-exit")) benchExit = true;
		else if(!std::strcmp(argv[i], "--jit-cache") && (i + 1 < argc)) jitCachePath = argv[++i];
		else if(!std::strcmp(argv[i], "--press") && (i + 1 < argc))
		{
			if(!ParsePresses(argv[++i], presses))
			{
				std::fprintf(stderr, "bad --press script: %s\n", argv[i]);
				return 2;
			}
		}
		else if(!std::strcmp(argv[i], "--mpeg-hle")) mpegHleEnabled = true;
		else if(!std::strcmp(argv[i], "--vita-jit")) vitaJit = true;
		else if(!std::strcmp(argv[i], "--no-indirect")) CEmuSession::SetIndirectDispatch(false);
		else if(!std::strcmp(argv[i], "--no-cheap-exits")) CEmuSession::SetCheapExits(false);
		else if(!std::strcmp(argv[i], "--no-pinned")) CEmuSession::SetPinnedFields(false);
		else if(!std::strcmp(argv[i], "--no-fast-ram")) CEmuSession::SetFastRam(false);
		else if(!std::strcmp(argv[i], "--iop-thread")) CPS2VM::g_parallelIop = true; //the IOP on its own thread
		else if(!std::strcmp(argv[i], "--no-stay-in-slice")) Iop::CSubSystem::g_stayInSlice = false;
		else if(!std::strcmp(argv[i], "--no-ee-stay-in-slice")) Ee::CSubSystem::g_stayInSlice = false;
		else if(!std::strcmp(argv[i], "--regions")) CEmuSession::SetRegions(true);
		else if(!std::strcmp(argv[i], "--tier2") || !std::strcmp(argv[i], "--tier2-interpret"))
		{
			//A tier 2 region at every block start (native code on ARM, else interpreted)
			bool interpret = !std::strcmp(argv[i], "--tier2-interpret");
#if defined(__arm__) && !defined(__aarch64__)
			Tier2::g_settings.mode = interpret ? Tier2::MODE_INTERPRET : Tier2::MODE_NATIVE;
#else
			(void)interpret;
			Tier2::g_settings.mode = Tier2::MODE_INTERPRET;
#endif
			Tier2::g_settings.eager = true;
		}
		else if(!std::strcmp(argv[i], "--shot-every") && (i + 1 < argc)) shotEvery = std::strtoull(argv[++i], nullptr, 10);
		else if(!std::strcmp(argv[i], "--shot-dir") && (i + 1 < argc)) shotDir = argv[++i];
		else if(!std::strcmp(argv[i], "--stats") && (i + 1 < argc)) statsPath = argv[++i];
		else if(!std::strcmp(argv[i], "--gs-diag") && (i + 1 < argc)) gsDiagLines = std::atoi(argv[++i]);
		else if(!std::strcmp(argv[i], "--hot-from") && (i + 1 < argc)) hotFrom = std::strtoull(argv[++i], nullptr, 10);
		else if(!std::strcmp(argv[i], "--dump-ram") && (i + 1 < argc)) ramDumpPath = argv[++i];
		else if(!std::strcmp(argv[i], "--load-state") && (i + 1 < argc)) loadStatePath = argv[++i];
		else if(!std::strcmp(argv[i], "--vu1-stats")) EmuProfile::g_vu1Programs.enabled = true; //vu1prog.txt
		else if(!std::strcmp(argv[i], "--vu-old-checks"))
		{
			//VU blocks: per slot stall checks and full flag pipeline walks (A/B of the faster ones)
			CVuBasicBlock::g_combinedStallCheck = false;
			VUShared::g_flagTracking = false;
		}
		else if(!std::strcmp(argv[i], "--vu1-verify")) CVpu::g_programHook = Vu1Verify::Hook; //vu1verify.txt
		else if(!std::strcmp(argv[i], "--vu1-hle")) Re4Vu1Hle::Enable(true); //RE4 world program without VU1
		else if(!std::strcmp(argv[i], "--gs-frames-ahead") && (i + 1 < argc)) CGSHandler::g_framesAhead = std::atoi(argv[++i]); //gs_frames_ahead
		else if(!std::strcmp(argv[i], "--no-block-writes")) GsTransferWrite::g_blockWrites = false; //transfers a pixel at a time (A/B)
		else if(!std::strcmp(argv[i], "--no-target-blocks")) CGSH_Hardware::g_targetPixelBlocks = false; //target_pixel_blocks=0
		else if(!std::strcmp(argv[i], "--no-world-merge")) CGSH_Hardware::g_retainedMerge = false; //gpu_world_merge=0
		else if(!std::strcmp(argv[i], "--no-vu1-vblank-sync")) Ee::CSubSystem::g_vblankVu1Sync = false; //vu1_vblank_sync=0
		else if(!std::strcmp(argv[i], "--no-fast-packed")) CGIF::g_fastPacked = false; //gif_fast_packed=0
		else if(!std::strcmp(argv[i], "--no-bulk-unpack")) CVif::g_bulkUnpack = false; //bulk_unpack=0
		else if(!std::strcmp(argv[i], "--no-feed-compare")) GsTransferWrite::g_compare = false; //transfer writes store without comparing
		else if(!std::strcmp(argv[i], "--no-clip-reject")) CGSH_Hardware::g_clipReject = false; //every clipped world triangle through the CPU clipper
		else if(!std::strcmp(argv[i], "--clip-reject-check")) CGSH_Hardware::g_clipRejectCheck = true; //rejected triangles clipped anyway: any drawn is a mismatch
		else if(!std::strcmp(argv[i], "--no-world-share-slots")) CGSH_Hardware::g_worldShareSlots = false; //one draw per world slot batch
		else if(!std::strcmp(argv[i], "--world-slots") && (i + 1 < argc)) CGSH_Hardware::g_gpuWorldSlots = std::atoi(argv[++i]); //batches per world draw (Vita: 1)
		else if(!std::strcmp(argv[i], "--state-breaks")) CGSH_Hardware::g_stateBreaks = true; //fixed function draws ended by state changes, by changed fields
		else if(!std::strcmp(argv[i], "--no-strip-merge")) CGSH_Hardware::g_stripMerge = false; //each scissor column's sprite its own draw
		else if(!std::strcmp(argv[i], "--all-world-uniforms")) CGSH_Hardware::g_worldUniformsUsed = false; //world draws upload all 16 uniform slots
		else if(!std::strcmp(argv[i], "--no-target-pool-lru")) CGSH_Hardware::g_targetPoolLru = false; //full pool deletes the released target; no ageing
		else if(!std::strcmp(argv[i], "--target-pool-min-age") && (i + 1 < argc)) CGSH_Hardware::g_targetPoolMinAge = std::atoi(argv[++i]);
		else if(!std::strcmp(argv[i], "--no-strip-merge-opaque")) CGSH_Hardware::g_stripMergeOpaque = false; //opaque strip sprites each their own draw
		else if(!std::strcmp(argv[i], "--no-gpu-upload")) CGSH_Hardware::g_gpuUpload = false; //target uploads with glTexSubImage2D
		else if(!std::strcmp(argv[i], "--no-texture-index")) CTextureCache::g_indexedLookup = false; //texture cache lookups compare every entry
		else if(!std::strcmp(argv[i], "--readback-liveness")) CGSH_Hardware::g_readbackLiveness = true; //read-back blocks later read or not
		else if(!std::strcmp(argv[i], "--block-coherency")) CGSH_Hardware::g_blockCoherency = true; //per-block stale/clean target tracking
		else if(!std::strcmp(argv[i], "--no-resize-on-gpu")) CGSH_Hardware::g_resizeOnGpu = false; //shrinks and trims with glCopyTexSubImage2D
		else if(!std::strcmp(argv[i], "--no-fast-skin-loop")) CGSH_Hardware::g_fastSkinLoop = false; //skinning batches through the generic world loop
		else if(!std::strcmp(argv[i], "--no-texture-page-hash")) CTextureCache::g_exactPageHash = false; //page hashes over whole page rows
		else if(!std::strcmp(argv[i], "--no-alpha-clut-reuse")) CGSH_Hardware::g_alphaClutReuse = false; //every alpha CLUT texture bind draws the pass
		else if(!std::strcmp(argv[i], "--no-solid-readback")) CGSH_Hardware::g_solidReadback = false; //read-backs of solid-filled targets read the GPU
		else if(!std::strcmp(argv[i], "--no-strip-merge-marks")) CGSH_Hardware::g_stripMergeMarks = false; //merged sprite columns do not mark their pixels dirty
		else if(!std::strcmp(argv[i], "--no-solid-upload")) CGSH_Hardware::g_solidUpload = false; //uploads of solid GS memory read and upload every pixel
		else if(!std::strcmp(argv[i], "--no-batch-pool")) Re4Vu1::g_batchPool = false; //world batches allocated and freed each time
		else if(!std::strcmp(argv[i], "--no-clut-hash-on-load")) CGSH_Hardware::g_clutHashOnLoad = false; //the CLUT hashed after every TEX0 write
		else if(!std::strcmp(argv[i], "--no-end-on-worker")) Re4Vu1::g_endOnWorker = false; //world batch ends projected on the GS thread
		else if(!std::strcmp(argv[i], "--solid-readback-check")) CGSH_Hardware::g_solidReadbackCheck = true; //solid read-backs also read the GPU and count differences
		else if(!std::strcmp(argv[i], "--no-mailbox-batch")) CMailBox::g_batched = false; //one lock and one notify per mailbox call
		else if(!std::strcmp(argv[i], "--feed-neon")) GsTransferWrite::g_columns = true; //PSMCT32 transfer blocks written by columns
		else if(!std::strcmp(argv[i], "--image-no-copy")) CGSHandler::g_imageNoCopy = true; //DIRECT image data to the GS thread without a copy
		else if(!std::strcmp(argv[i], "--vif-ring-kb") && (i + 1 < argc)) CVpu::g_asyncRingSize = static_cast<uint32>(std::atoi(argv[++i])) * 1024; //VIF1 ring size (power of 2, >= 128)
		else if(!std::strcmp(argv[i], "--vif-merge")) CVpu::g_asyncMerge = true; //DMA tag records joined with their data records in the VIF1 ring
		else if(!std::strcmp(argv[i], "--fine-timers")) CGSH_Hardware::g_fineTimers = true; //per batch timers: retained world segments printed at the end
		else if(!std::strcmp(argv[i], "--feed-shapes")) CGSH_Hardware::g_feedShapes = true; //host to local transfers by format, size and alignment
		else if(!std::strcmp(argv[i], "--io-counts")) Ee::CSubSystem::g_ioCounts = true; //EE I/O accesses per register
		else if(!std::strcmp(argv[i], "--reuse-scan")) CVpu::g_reuseScan = CGSHandler::g_shadowRegs = true; //relayed GIF data repeated across frames
		else if(!std::strcmp(argv[i], "--no-merged-submit")) CGSHandler::g_mergedSubmit = false; //gs_merged_submit=0
		else if(!std::strcmp(argv[i], "--no-fast-registers")) CGSH_Primitives::g_fastRegisters = false; //gs_fast_registers=0
		else if(!std::strcmp(argv[i], "--re4-skin")) CVpu::g_nativeProgram = Re4Skin::Run; //RE4 skinning programs natively (re4_skin)
		else if(!std::strcmp(argv[i], "--skin-verify")) CVpu::g_programHook = Re4Skin::VerifyHook; //skinverify.txt (inline VU1)
		else if(!std::strcmp(argv[i], "--skin-model")) Re4Skin::g_modelCheck = true; //native skinning against a plain float model
		else if(!std::strcmp(argv[i], "--skin-sparse-state")) Re4Skin::g_sparseState = true; //experiment: skinning leaves only cheap registers
		else if(!std::strcmp(argv[i], "--gpu-skin")) Re4Skin::g_gpuSkin = true; //skinning batches through the GPU world path (gpu_skin)
		else if(!std::strcmp(argv[i], "--no-group-submit")) Re4Vu1Hle::g_groupSubmit = false; //one GS call per world batch (group_submit=0)
		else if(!std::strcmp(argv[i], "--vu-scalar-opmul")) VUShared::g_vectorOuterProduct = false; //old OPMULA/OPMSUB
		else if(!std::strcmp(argv[i], "--vu-no-loop-variant")) CVuBasicBlock::g_loopVariant = false;      //no loop copy of self-looping VU blocks
		else if(!std::strcmp(argv[i], "--vu-no-clamp")) CVuBasicBlock::g_extraCompileHints |= VUShared::COMPILEHINT_NO_CLAMPING; //vu_no_clamp
		else if(!std::strcmp(argv[i], "--vu1-async")) CVpu::g_asyncVif1 = true;               //VIF1 work on the VU1 thread (with --mtvu)
		else if(!std::strcmp(argv[i], "--vu1-async-no-p3sync")) CVpu::g_asyncPath3Sync = false; //PATH3 does not wait for queued VIF1 work
#ifdef VITAPS2_HAS_GL
		else if(!std::strcmp(argv[i], "--re4-skip-dof") && (i + 1 < argc)) CGSH_Hardware::g_re4SkipDof = std::strtoul(argv[++i], nullptr, 0);
		else if(!std::strcmp(argv[i], "--gs-shaders")) CGSH_Hardware::g_shaderBatches = true; //batch program (gs_shaders, the default)
		else if(!std::strcmp(argv[i], "--no-gs-shaders")) CGSH_Hardware::g_shaderBatches = false; //fixed-function batches (gs_shaders=0)
		else if(!std::strcmp(argv[i], "--gpu-world-debug") && (i + 1 < argc)) CGSH_Hardware::g_gpuWorldDebug = std::atoi(argv[++i]);
		else if(!std::strcmp(argv[i], "--no-gpu-world")) CGSH_Hardware::g_gpuWorld = false;
		else if(!std::strcmp(argv[i], "--gpu-world")) CGSH_Hardware::g_gpuWorld = true;
		else if(!std::strcmp(argv[i], "--gpu-world-guard") && (i + 1 < argc)) CGSH_Hardware::g_gpuWorldGuard = std::atoi(argv[++i]);
		else if(!std::strcmp(argv[i], "--gs-ceiling")) CGSH_Primitives::g_gsCeiling = true;
		else if(!std::strcmp(argv[i], "--no-partial-relayout")) CGSH_Hardware::g_partialRelayout = false;
		else if(!std::strcmp(argv[i], "--no-target-pool")) CGSH_Hardware::g_targetPool = false;
		else if(!std::strcmp(argv[i], "--no-sprite-new-target")) CGSH_Hardware::g_spriteCreatesTarget = false;
		else if(!std::strcmp(argv[i], "--no-strip-cover")) CGSH_Hardware::g_stripCover = false;
		else if(!std::strcmp(argv[i], "--gpu-world-retained")) Re4Vu1Hle::g_hashBatches = CGSH_Hardware::g_gpuWorldRetained = true;
#endif
	}

	const char* home = std::getenv("HOME");
	std::string dataPath = std::string(home ? home : ".") + "/.local/share/VitaPS2";

	CEmuSession::CONFIG config;
	config.dataPath = dataPath;
	config.resourcesPath = ".";
	config.limitFrameRate = false;
	config.rasterizerThreads = gsThreads;
	config.interlacedRendering = interlaced;
	config.frameSkip = frameSkip;
	config.padFactory = [presses]() { return new CPH_Script(presses); };
	if(benchExit)
	{
		config.frameHook = [targetFrames](uint64_t frame, uint64_t eeExecuted) {
			if(frame < targetFrames) return;
			std::printf("EE executed %llu idle 0\n", (unsigned long long)eeExecuted);
			std::fflush(stdout);
			std::_Exit(0);
		};
	}

	// The GPU renderer runs on this thread.
	CEmuSession* sessionPtr = nullptr;
	std::function<void(uint32_t)> pump = [](uint32_t timeoutMs) { std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs)); };
#ifdef VITAPS2_HAS_GL
	OSMesaContext glContext = nullptr;
	std::vector<uint32_t> glWindow(16 * 16);
	if(hardware)
	{
		const int attribs[] = {OSMESA_FORMAT, OSMESA_RGBA, OSMESA_DEPTH_BITS, 24, OSMESA_PROFILE, OSMESA_COMPAT_PROFILE, 0};
		glContext = OSMesaCreateContextAttribs(attribs, nullptr);
		if(!glContext || !OSMesaMakeCurrent(glContext, glWindow.data(), GL_UNSIGNED_BYTE, 16, 16))
		{
			std::fprintf(stderr, "could not create an OSMesa context\n");
			return 1;
		}
		CGSH_Hardware::OPTIONS options;
		options.readbackFrames = true;
		options.frameSink = [&sessionPtr](std::vector<uint32>& pixels, uint32 width, uint32 height) {
			if(sessionPtr) sessionPtr->GetFrames().Publish(pixels, width, height);
		};
		config.gsFactory = CGSH_Hardware::GetFactoryFunction(options);
		auto hwGs = [&sessionPtr]() -> CGSH_Hardware* {
			return sessionPtr ? static_cast<CGSH_Hardware*>(sessionPtr->GetVm()->GetGSHandler()) : nullptr;
		};
		pump = [hwGs](uint32_t timeoutMs) {
			if(auto gs = hwGs()) gs->Pump(timeoutMs);
			else std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
		};
		config.gsPump = [pump]() { pump(2); };
		config.gsShutdown = [hwGs]() {
			if(auto gs = hwGs()) gs->ReleaseGpu();
		};
	}
#else
	if(hardware)
	{
		std::fprintf(stderr, "built without the GPU renderer\n");
		return 2;
	}
#endif

	if(vitaJit) CEmuSession::SetSafeJit(false);
	try
	{
		CEmuSession session(config);
		sessionPtr = &session;
		if(threadedVu1 || threadedSpu)
		{
			auto hacks = session.GetSpeedHacks();
			hacks.threadedVu1 = threadedVu1;
			hacks.threadedSpu = threadedSpu;
			session.SetSpeedHacks(hacks);
		}
		if(!jitCachePath.empty())
		{
			bool opened = JitCacheSetup::Open(jitCachePath, "/proc/self/exe");
			std::printf("jit cache %s: %s\n", jitCachePath.c_str(), opened ? "open" : "not available here");
		}
		session.Boot(bootPath);
		CMpegHle mpegHle;
		if(mpegHleEnabled) mpegHle.Enable(session.GetVm()->m_ee->m_ram, "mpeghle.txt", nullptr);
#ifdef VITAPS2_HAS_GL
		if(hardware && gsDiagLines)
		{
			static_cast<CGSH_Hardware*>(session.GetVm()->GetGSHandler())->SetDiagnosticsFile("gsdiag.txt", gsDiagLines);
		}
#endif

		std::FILE* statsFile = statsPath.empty() ? nullptr : std::fopen(statsPath.c_str(), "w");
		if(statsFile)
		{
			std::fprintf(statsFile, "frame,real_s,vm_fps,presented,ee_pc");
			for(unsigned int i = 0; i < EmuProfile::SECTION_COUNT; i++) std::fprintf(statsFile, ",ps2_%s", EmuProfile::GetSectionName(i));
			std::fprintf(statsFile, ",mpeg_pictures,mpeg_waits");
#ifdef VITAPS2_HAS_GL
			if(hardware)
			{
				std::fprintf(statsFile, ",gpu_pump_ms,gpu_gl_ms,gpu_tex_ms,gpu_xfer_ms,gpu_disp_ms,up_gl_ms,up_kpix,dl_gl_ms,dl_kpix"
				                        ",draws,tex_up,rt_up,rt_dl,up_px,dl_px,targets_new,xfer_in,xfer_out,xfer_local,rt_deferred,rt_skip_rows,xfer_same");
			}
#endif
			std::fprintf(statsFile, "\n");
		}

		ThreadProfiler::Sample();
		auto start = std::chrono::steady_clock::now();
		uint64_t serial = 0;
		std::vector<uint32_t> pixels;
		uint32_t width = 0, height = 0;
		uint64_t presented = 0;
		uint64_t nextShot = shotEvery, nextStats = 60, statsPresented = 0;
		auto statsTime = start;
#ifdef VITAPS2_HAS_GL
		CGSH_Hardware::GPU_TIMES statsGpu = CGSH_Hardware::GetTimes();
#endif
		CMpegHle::STATS statsMpeg;
		std::map<uint32, uint32> hotPcs;
		while(session.GetVmFrameCount() < targetFrames)
		{
			if(session.GetFrames().Fetch(serial, pixels, width, height)) presented++;
			if(std::chrono::steady_clock::now() - start > std::chrono::seconds(timeoutSeconds)) break;
			pump(5);
			if(!loadStatePath.empty() && (session.GetVmFrameCount() >= 120))
			{
				// The VM loads on its own thread; the GS keeps being pumped meanwhile.
				auto loading = session.GetVm()->LoadState(fs::path(loadStatePath));
				while(loading.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) pump(2);
				bool loaded = loading.get();
				std::printf("state %s: %s\n", loadStatePath.c_str(), loaded ? "loaded" : "FAILED");
				std::fflush(stdout);
#ifdef VITAPS2_HAS_GL
				if(loaded && hardware) static_cast<CGSH_Hardware*>(session.GetVm()->GetGSHandler())->DiscardGpuCopies();
#endif
				loadStatePath.clear();
			}
			uint64_t frame = session.GetVmFrameCount();
			if(frame >= hotFrom) hotPcs[session.GetDebugState().eePc]++;
			if(shotEvery && (frame >= nextShot))
			{
				nextShot = frame - (frame % shotEvery) + shotEvery;
				if(width) WritePpm(shotDir + "/shot_" + std::to_string(frame) + ".ppm", pixels, width, height);
			}
			if(statsFile && (frame >= nextStats))
			{
				// One row per emulated second.
				nextStats = frame - (frame % 60) + 60;
				auto now = std::chrono::steady_clock::now();
				double seconds = std::chrono::duration<double>(now - statsTime).count();
				statsTime = now;
				auto profile = session.TakeProfile();
				auto debug = session.GetDebugState();
				std::fprintf(statsFile, "%llu,%.2f,%.1f,%llu,%08X", (unsigned long long)frame, seconds, 60.0 / seconds,
				             (unsigned long long)(presented - statsPresented), debug.eePc);
				statsPresented = presented;
				for(unsigned int i = 0; i < EmuProfile::SECTION_COUNT; i++) std::fprintf(statsFile, ",%.3f", profile.share[i]);
				auto mpegStats = mpegHle.GetStats();
				std::fprintf(statsFile, ",%u,%u", mpegStats.pictures - statsMpeg.pictures, mpegStats.waits - statsMpeg.waits);
				statsMpeg = mpegStats;
#ifdef VITAPS2_HAS_GL
				if(hardware)
				{
					auto times = CGSH_Hardware::GetTimes();
					auto f = static_cast<CGSH_Hardware*>(session.GetVm()->GetGSHandler())->GetLastFrameStats();
					std::fprintf(statsFile, ",%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%u,%.1f,%u", (times.pumpUs - statsGpu.pumpUs) / 1000.0,
					             (times.stageUs[0] - statsGpu.stageUs[0]) / 1000.0, (times.stageUs[1] - statsGpu.stageUs[1]) / 1000.0,
					             (times.stageUs[2] - statsGpu.stageUs[2]) / 1000.0, (times.stageUs[3] - statsGpu.stageUs[3]) / 1000.0,
					             (times.uploadGlUs - statsGpu.uploadGlUs) / 1000.0, static_cast<unsigned int>((times.uploadPixels - statsGpu.uploadPixels) / 1000),
					             (times.downloadGlUs - statsGpu.downloadGlUs) / 1000.0, static_cast<unsigned int>((times.downloadPixels - statsGpu.downloadPixels) / 1000));
					std::fprintf(statsFile, ",%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u", f.drawCalls, f.textureUploads, f.targetUploads, f.targetDownloads,
					             f.uploadedPixels, f.downloadedPixels, f.targetsCreated, f.transfersIn, f.transfersOut, f.transfersLocal,
					             f.deferredUploads, f.skippedUploadRows, f.unchangedTransfers);
					statsGpu = times;
				}
#endif
				std::fprintf(statsFile, "\n");
				std::fflush(statsFile);
			}
		}
		if(statsFile) std::fclose(statsFile);
		mpegHle.Disable();
		if(Re4Skin::g_stats.verified != 0) Re4Skin::WriteReport("skinverify.txt");
		else if(CVpu::g_programHook) Vu1Verify::Write("vu1verify.txt");
		if(CVpu::g_nativeProgram) std::printf("re4 skin: %llu native runs, %llu refused\n", (unsigned long long)Re4Skin::g_stats.runs, (unsigned long long)Re4Skin::g_stats.refused);
		if(EmuProfile::g_vu1Programs.enabled)
		{
			if(std::FILE* file = std::fopen("vu1prog.txt", "w"))
			{
				WriteVu1ProgramStats(file);
				std::fclose(file);
			}
		}
		if(!hotPcs.empty())
		{
			// EE PC samples (one per runner loop) by address, most frequent first.
			std::vector<std::pair<uint32, uint32>> hot(hotPcs.begin(), hotPcs.end());
			std::sort(hot.begin(), hot.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
			uint32 total = 0;
			for(const auto& entry : hot) total += entry.second;
			std::printf("EE hot (%u samples):", total);
			for(size_t i = 0; i < std::min<size_t>(hot.size(), 24); i++) std::printf(" %08X:%u", hot[i].first, hot[i].second);
			std::printf("\n");
		}
		auto threadUsage = ThreadProfiler::Sample();
		auto profile = session.TakeProfile();
		session.Pause();
		std::printf("emulation thread (%u samples):", profile.samples);
		for(unsigned int i = 0; i < EmuProfile::SECTION_COUNT; i++)
			if(profile.share[i] >= 0.005f) std::printf(" %s %.0f%%", EmuProfile::GetSectionName(i), profile.share[i] * 100.0f);
		std::printf("\n");
		auto debug = session.GetDebugState();
		std::printf("EE pc %08X ra %08X  IOP pc %08X ra %08X thread %d  INTC %X/%X  DMAC %08X  VU1 %04X pc %04X %ums\n", debug.eePc,
		            debug.eeRa, debug.iopPc, debug.iopRa, debug.iopThread, debug.intcStat, debug.intcMask, debug.dmacStat,
		            debug.vu1Start, debug.vu1Pc, debug.vu1RunMs);
		std::printf("EE s6 %08X s7 %08X\n", debug.eeGpr[22], debug.eeGpr[23]);
		{
			// DMA and VIF1 state (hangs waiting on a transfer).
			auto& ee = *session.GetVm()->m_ee;
			auto dmac = [&](uint32 address) { return ee.m_dmac.GetRegister(address); };
			std::printf("DMAC CTRL %08X STAT %08X PCR %08X STADR %08X ENABLER %08X\n", dmac(0x1000E000), dmac(0x1000E010), dmac(0x1000E020),
			            dmac(0x1000E040), dmac(0x1000F520));
			const std::pair<const char*, uint32> channels[] = {{"VIF0", 0x10008000}, {"VIF1", 0x10009000}, {"GIF", 0x1000A000},
			                                                   {"fromIPU", 0x1000B000}, {"toIPU", 0x1000B400}};
			for(const auto& [name, base] : channels)
				std::printf("D %-7s CHCR %08X MADR %08X QWC %04X TADR %08X\n", name, dmac(base), dmac(base + 0x10), dmac(base + 0x20), dmac(base + 0x30));
			auto& vif1 = ee.m_vpu1->GetVif();
			std::printf("VIF1 STAT %08X CODE %08X NUM %u TOP %X ITOP %X\n", vif1.GetRegister(0x10003C00), vif1.GetRegister(0x10003C80),
			            vif1.GetRegister(0x10003C60), vif1.GetRegister(0x10003CE0), vif1.GetRegister(0x10003CD0));
			std::printf("GIF STAT %08X active path %u\n", ee.m_gif.GetRegister(0x10003020), ee.m_gif.GetActivePath());
			std::printf("VU1 ready %d running %d threaded %d start %04X pc %04X\n", ee.m_vpu1->IsVuReady() ? 1 : 0, ee.m_vpu1->IsVuRunning() ? 1 : 0,
			            ee.m_vpu1->IsThreaded() ? 1 : 0, ee.m_vpu1->GetProgramStart(), ee.m_vpu1->GetCurrentPc());
			if(!ramDumpPath.empty())
			{
				if(std::FILE* file = std::fopen(ramDumpPath.c_str(), "wb"))
				{
					std::fwrite(ee.m_ram, 1, 0x02000000, file); //retail RAM (32 MB)
					std::fclose(file);
				}
			}
			if(EmuProfile::g_vu1Programs.enabled)
			{
				// VU1 micro memory and data memory, and the microcode disassembled (upper | lower).
				if(std::FILE* file = std::fopen("vu1micro.bin", "wb"))
				{
					std::fwrite(ee.m_microMem1, 1, PS2::MICROMEM1SIZE, file);
					std::fclose(file);
				}
				if(std::FILE* file = std::fopen("vu1mem.bin", "wb"))
				{
					std::fwrite(ee.m_vuMem1, 1, PS2::VUMEM1SIZE, file);
					std::fclose(file);
				}
				if(std::FILE* file = std::fopen("vu1micro.txt", "w"))
				{
					auto& vu1 = ee.m_VU1;
					for(uint32 address = 0; address < PS2::MICROMEM1SIZE; address += 8)
					{
						uint32 lower = *reinterpret_cast<const uint32*>(ee.m_microMem1 + address);
						uint32 upper = *reinterpret_cast<const uint32*>(ee.m_microMem1 + address + 4);
						char upperMnemonic[64] = {}, upperOperands[64] = {}, lowerMnemonic[64] = {}, lowerOperands[64] = {};
						vu1.m_pArch->GetInstructionMnemonic(&vu1, address + 4, upper, upperMnemonic, sizeof(upperMnemonic));
						vu1.m_pArch->GetInstructionOperands(&vu1, address + 4, upper, upperOperands, sizeof(upperOperands));
						vu1.m_pArch->GetInstructionMnemonic(&vu1, address, lower, lowerMnemonic, sizeof(lowerMnemonic));
						vu1.m_pArch->GetInstructionOperands(&vu1, address, lower, lowerOperands, sizeof(lowerOperands));
						std::fprintf(file, "%04X %08X %08X  %-10s %-24s | %-10s %s\n", address, upper, lower, upperMnemonic, upperOperands, lowerMnemonic, lowerOperands);
					}
					std::fclose(file);
				}
			}
		}
		if(!jitCachePath.empty())
		{
			std::printf("%s\n", JitCacheSetup::Describe().c_str());
			JitCacheSetup::Close();
		}
		std::printf("EE executed %llu idle %llu\n", (unsigned long long)session.GetEeExecutedTicks(), (unsigned long long)session.GetEeIdleTicks());
		{
			// Returns to the C++ dispatch loop per 1000 EE instructions (indirect jumps, unlinked blocks).
			double executed = static_cast<double>(std::max<uint64_t>(session.GetEeExecutedTicks(), 1));
			std::printf("dispatch: EE %llu (%.1f per 1000 instructions, %llu slices), IOP %llu (%llu slices)\n",
			            (unsigned long long)EmuProfile::g_dispatches[0], EmuProfile::g_dispatches[0] * 1000.0 / executed,
			            (unsigned long long)EmuProfile::g_executeCalls[0], (unsigned long long)EmuProfile::g_dispatches[1],
			            (unsigned long long)EmuProfile::g_executeCalls[1]);
			std::printf("regions: %u (%llu EE instructions)\n", EmuProfile::g_regions, (unsigned long long)EmuProfile::g_regionInstructions);
			std::printf("tier2: %u regions, %u not built, %llu EE instructions, %u inlined calls, %llu code bytes, %llu us\n", Tier2::g_stats.regions,
			            Tier2::g_stats.failed, (unsigned long long)Tier2::g_stats.guestInstructions, Tier2::g_stats.inlinedCalls,
			            (unsigned long long)Tier2::g_stats.codeBytes, (unsigned long long)Tier2::g_stats.compileUs);
			std::printf("tier2 runs: %llu\n", (unsigned long long)Tier2::g_stats.interpretedRuns);
		}
		session.GetFrames().Fetch(serial, pixels, width, height);
		for(const auto& usage : threadUsage)
		{
			std::printf("cpu %-18s %5.1f%%\n", usage.name.c_str(), usage.cpuPercent);
		}

		double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		uint64_t vmFrames = session.GetVmFrameCount();
		std::printf("vm frames: %llu, presented frames: %llu, %.2fs (%.1f fps)\n",
		            (unsigned long long)vmFrames, (unsigned long long)presented, seconds, vmFrames / seconds);
#ifdef VITAPS2_HAS_GL
		if(hardware)
			std::printf("game frames (GPU renderer): %u, primitives drawn: %u\n", CGSH_Hardware::g_gameFrames,
			            static_cast<CGSH_Hardware*>(session.GetVm()->GetGSHandler())->GetPrimitiveCount());
#endif
#ifdef VITAPS2_HAS_GL
		if(CGSH_Hardware::g_shaderBatches)
			std::printf("batch program: %llu draws from the vertex ring (%.1f MB), %llu from client memory (ring full)\n", (unsigned long long)CGSH_Hardware::g_ringDraws,
			            CGSH_Hardware::g_ringBytes / 1048576.0, (unsigned long long)CGSH_Hardware::g_ringOverflows);
		if(CGSH_Hardware::g_shaderBatches)
			std::printf("strip triangles sharing vertices: %llu of %llu (%llu continuing)\n", (unsigned long long)CGSH_Hardware::g_stripShared,
			            (unsigned long long)CGSH_Hardware::g_stripCalls, (unsigned long long)CGSH_Hardware::g_stripContinues);
		if(CGSH_Hardware::g_shaderBatches)
			std::printf("strips broken by: new PRIM %llu, state change %llu, kick without drawing %llu\n", (unsigned long long)CGSH_Primitives::g_stripBreaks[0],
			            (unsigned long long)CGSH_Primitives::g_stripBreaks[1], (unsigned long long)CGSH_Primitives::g_stripBreaks[2]);
		if(CGSH_Hardware::g_shaderBatches)
		{
			std::printf("state changed by register:");
			for(unsigned int r = 0; r < 256; r++)
				if(CGSH_Primitives::g_dirtyBy[r] > 1000) std::printf(" %02X:%llu", r, (unsigned long long)CGSH_Primitives::g_dirtyBy[r]);
			std::printf("\n");
		}
		if(CGSH_Hardware::g_re4SkipDof) std::printf("draws skipped (re4_skip_dof): %llu\n", (unsigned long long)CGSH_Hardware::g_skippedDraws);
#endif
		if(CVpu::g_asyncVif1)
		{
			auto& a = CVpu::g_asyncStats;
			std::printf("async VIF1: %llu packets, %.1f MB, %llu stalls, %llu quiet waits (%.0f ms), %llu PATH3 syncs\n", (unsigned long long)a.packets.load(),
			            a.bytes.load() / 1048576.0, (unsigned long long)a.stalls.load(), (unsigned long long)a.quietWaits.load(), a.quietWaitUs.load() / 1000.0, (unsigned long long)a.path3Syncs.load());
			std::printf("VIF1 ring: %u KB, most queued at once %llu KB, %llu records waited for room, %llu tags merged\n", CVpu::g_asyncRingSize / 1024, (unsigned long long)(a.ringPeak.load() / 1024), (unsigned long long)a.ringWaits.load(), (unsigned long long)a.merged.load());
			std::printf("image data to the GS: %.1f MB by reference, %.1f MB copied\n", CGSHandler::g_imageNoCopyBytes / 1048576.0, CGSHandler::g_imageCopyBytes / 1048576.0);
			std::printf("relay: %llu kicks (%.1f MB), %llu DIRECT (%.1f MB), %llu replaced; %llu register writes\n",(unsigned long long)a.kicks.load(), a.kickBytes.load() / 1048576.0,
			            (unsigned long long)a.directs.load(), a.directBytes.load() / 1048576.0, (unsigned long long)a.customs.load(), (unsigned long long)CGSHandler::g_writeBufferTotal);
			std::printf("unpacked by runs: %llu values\n", (unsigned long long)CVif::g_bulkValues);
			if(CVpu::g_reuseScan)
				for(int k = 0; k < 2; k++)
				{
					const auto& c = CVpu::g_reuse[k];
					std::printf("reuse %s: %llu packets (%.1f MB), %llu repeated within 6 vblanks (%.1f%%, %.1f%% of bytes)\n", k ? "DIRECT" : "XGKICK",
					            (unsigned long long)c[0], c[1] / 1048576.0, (unsigned long long)c[2], c[0] ? 100.0 * c[2] / c[0] : 0.0, c[1] ? 100.0 * c[3] / c[1] : 0.0);
				}
			if(CVpu::g_reuseScan)
				for(int m = 0; m < 4; m++)
					std::printf("reuse DIRECT mode %d: %.1f MB, %.1f MB repeated\n", m, CVpu::g_reuseDirectMode[m][0] / 1048576.0, CVpu::g_reuseDirectMode[m][1] / 1048576.0);
			if(Re4Skin::g_modelCheck) Re4Skin::PrintModelStats();
			std::printf("host to local transfers: %llu, changing no byte of GS memory: %llu\n", (unsigned long long)CGSH_Hardware::g_transfersInTotal, (unsigned long long)CGSH_Hardware::g_unchangedTransfersTotal);
			std::printf("alpha CLUT passes: %llu, results reused: %llu\n", (unsigned long long)CGSH_Hardware::g_alphaClutPassesTotal, (unsigned long long)CGSH_Hardware::g_alphaClutReuses);
			std::printf("solid read-backs: %llu (%llu pixels), colours learned %llu, pixels differing from the GPU %llu\n", (unsigned long long)CGSH_Hardware::g_solidReadbacks, (unsigned long long)CGSH_Hardware::g_solidReadbackPixels, (unsigned long long)CGSH_Hardware::g_solidLearned, (unsigned long long)CGSH_Hardware::g_solidReadbackMismatches);
			std::printf("solid colours not learned (read-back not one colour): %llu; first difference at %u,%u: GPU %08X, written %08X\n", (unsigned long long)CGSH_Hardware::g_solidLearnFailures, CGSH_Hardware::g_solidFirstMismatch[0], CGSH_Hardware::g_solidFirstMismatch[1], CGSH_Hardware::g_solidFirstMismatch[2], CGSH_Hardware::g_solidFirstMismatch[3]);
			std::printf("solid uploads: %llu (%llu pixels), pixels differing from memory %llu\n", (unsigned long long)CGSH_Hardware::g_solidUploads, (unsigned long long)CGSH_Hardware::g_solidUploadPixels, (unsigned long long)CGSH_Hardware::g_solidUploadMismatches);
			std::printf("world batches taken from the pool: %llu, allocated: %llu\n", (unsigned long long)Re4Vu1::g_batchesPooled, (unsigned long long)Re4Vu1::g_batchesAllocated);
			if(CGSH_Hardware::g_fineTimers) std::printf("retained world ms: matrix %.0f, hash %.0f, box %.0f, flush/setup %.0f, uniforms %.0f, draw %.0f; slot path %.0f\n", CGSH_Hardware::g_retainedSegmentUs[0] / 1000.0, CGSH_Hardware::g_retainedSegmentUs[1] / 1000.0, CGSH_Hardware::g_retainedSegmentUs[2] / 1000.0, CGSH_Hardware::g_retainedSegmentUs[3] / 1000.0, CGSH_Hardware::g_retainedSegmentUs[4] / 1000.0, CGSH_Hardware::g_retainedSegmentUs[5] / 1000.0, CGSH_Hardware::g_worldSlotUs / 1000.0);
			for(int psm = 0; psm < 64; psm++)
			{
				if(CGSH_Hardware::g_feedCallsByPsm[psm] == 0) continue;
				std::printf("image data into psm %02X: %llu calls, %llu KB\n", psm, (unsigned long long)CGSH_Hardware::g_feedCallsByPsm[psm], (unsigned long long)(CGSH_Hardware::g_feedBytesByPsm[psm] / 1024));
			}
			{
				const char* kinds[4] = {"batch program", "fixed function", "world slot", "retained world"};
				for(int k = 0; k < 4; k++)
					std::printf("GL draws, %s: %llu, vertices or indices %llu\n", kinds[k], (unsigned long long)CGSH_Hardware::g_drawsByKind[k], (unsigned long long)CGSH_Hardware::g_drawElementsByKind[k]);
				std::printf("world batches that joined the open slot: %llu\n", (unsigned long long)CGSH_Hardware::g_worldSlotShares);
				if(CGSH_Hardware::g_blockCoherency)
					std::printf("block mapping check: %u failures\n", static_cast<CGSH_Hardware*>(session.GetVm()->GetGSHandler())->CheckBlockMapping());
				if(CGSH_Hardware::g_blockCoherency)
					std::printf("block coherency: %llu transfers by blocks, %llu blocks read back, %llu stale runs uploaded, %llu targets flattened\n", (unsigned long long)CGSH_Hardware::g_blockTransfers,
					            (unsigned long long)CGSH_Hardware::g_blockReadBacks, (unsigned long long)CGSH_Hardware::g_blockResolves, (unsigned long long)CGSH_Hardware::g_blockFlattens);
				if(CGSH_Hardware::g_readbackLiveness)
				{
					for(int cause = 0; cause < 16; cause++)
					{
						if(CGSH_Hardware::g_liveDownloaded[cause] == 0) continue;
						std::printf("read-back blocks, cause %d: %llu written to GS memory, %llu read afterwards, %llu replaced by a later read-back unread, %llu overwritten by a transfer unread\n", cause,
						            (unsigned long long)CGSH_Hardware::g_liveDownloaded[cause], (unsigned long long)CGSH_Hardware::g_liveNeeded[cause], (unsigned long long)CGSH_Hardware::g_liveSuperseded[cause],
						            (unsigned long long)CGSH_Hardware::g_liveOverwritten[cause]);
						std::printf("  read by:");
						for(int r = 0; r < 16; r++) if(CGSH_Hardware::g_liveNeededBy[cause][r]) std::printf(" %d:%llu", r, (unsigned long long)CGSH_Hardware::g_liveNeededBy[cause][r]);
						std::printf("\n");
					}
				}
				std::printf("sprites that joined the previous column's draw: %llu (opaque: %llu)\n", (unsigned long long)CGSH_Hardware::g_stripMerges, (unsigned long long)CGSH_Hardware::g_stripMergesOpaque);
				if(CGSH_Hardware::g_stateBreaks)
				{
					std::vector<std::pair<uint64, std::string>> breaks;
					uint64 total = 0;
					for(const auto& [fields, count] : CGSH_Hardware::g_stateBreakCounts)
					{
						breaks.emplace_back(count.first, fields + " (" + std::to_string(count.second) + " vertices)");
						total += count.first;
					}
					std::sort(breaks.rbegin(), breaks.rend());
					std::printf("fixed function draws ended by a state change: %llu\n", (unsigned long long)total);
					std::vector<std::pair<uint64, std::string>> misses;
					for(const auto& [key, count] : CGSH_Hardware::g_targetMisses) misses.emplace_back(count, key);
					std::sort(misses.rbegin(), misses.rend());
					std::printf("render targets created (not from the pool): %llu, pool evictions %llu\n", (unsigned long long)CGSH_Hardware::g_targetAllocs, (unsigned long long)CGSH_Hardware::g_targetPoolEvictions);
					std::vector<std::pair<uint64, std::string>> downloads;
					for(const auto& [key, count] : CGSH_Hardware::g_downloadCauses) downloads.emplace_back(count.second, key + " : " + std::to_string(count.first) + " read-backs");
					std::sort(downloads.rbegin(), downloads.rend());
					for(size_t i = 0; i < std::min<size_t>(downloads.size(), 16); i++) std::printf("  read-back %llu px: %s\n", (unsigned long long)downloads[i].first, downloads[i].second.c_str());
					for(size_t i = 0; i < std::min<size_t>(misses.size(), 15); i++) std::printf("  target miss %llu: %s\n", (unsigned long long)misses[i].first, misses[i].second.c_str());
					for(size_t i = 0; i < std::min<size_t>(breaks.size(), 20); i++) std::printf("  state break %llu:%s\n", (unsigned long long)breaks[i].first, breaks[i].second.c_str());
				}
				std::printf("fixed function draws with CPU-clipped world triangles: %llu (%llu vertices)\n", (unsigned long long)CGSH_Hardware::g_drawsWithClippedWorld, (unsigned long long)CGSH_Hardware::g_drawsWithClippedWorldVertices);
				std::printf("world triangles to the CPU clipper: %llu, rejected before clipping: %llu, of those the clipper would draw: %llu\n", (unsigned long long)CGSH_Hardware::g_worldClipCalls,
				            (unsigned long long)CGSH_Hardware::g_worldClipRejected, (unsigned long long)CGSH_Hardware::g_clipRejectMismatches);
				const char* worldKinds[4] = {"retained", "slot", "skinning slot", "other"};
				for(int k = 0; k < 4; k++)
					std::printf("world batches, %s: %llu, vertices %llu\n", worldKinds[k], (unsigned long long)CGSH_Hardware::g_worldBatchesByKind[k], (unsigned long long)CGSH_Hardware::g_worldVerticesByKind[k]);
			}
			if(CGSH_Hardware::g_feedShapes)
			{
				std::printf("transfer writes: %llu calls (%llu starting inside a block row), units by block rows %llu, one at a time %llu\n", (unsigned long long)GsTransferWrite::g_calls,
				            (unsigned long long)GsTransferWrite::g_unalignedStarts, (unsigned long long)GsTransferWrite::g_blockUnits, (unsigned long long)GsTransferWrite::g_singleUnits);
				std::vector<std::pair<uint64, std::string>> shapes;
				for(const auto& [shape, count] : CGSH_Hardware::g_feedShapeCounts) shapes.emplace_back(count.second, shape + " : " + std::to_string(count.first) + " transfers");
				std::sort(shapes.rbegin(), shapes.rend());
				for(size_t i = 0; i < std::min<size_t>(shapes.size(), 25); i++) std::printf("feed shape %s, %llu KB\n", shapes[i].second.c_str(), (unsigned long long)(shapes[i].first / 1024));
			}
			if(Re4Skin::g_gpuSkin) std::printf("gpu skin: %llu batches (%llu vertices) on the GPU, %llu native\n", (unsigned long long)Re4Skin::g_gpuBatches, (unsigned long long)Re4Skin::g_gpuVertices, (unsigned long long)Re4Skin::g_nativeBatches);
			if(CVpu::g_reuseScan)
			{
				std::printf("reuse DIRECT with the same GS register state: %llu packets, %.1f MB\n", (unsigned long long)CVpu::g_reuseWithState[0], CVpu::g_reuseWithState[1] / 1048576.0);
				std::printf("XGKICK bytes by program:");
				for(const auto& [program, counts] : CVpu::g_kicksByProgram)
					if(counts.second > 1048576) std::printf(" %04X:%llu packets/%.1f MB", program, (unsigned long long)counts.first, counts.second / 1048576.0);
				std::printf("\n");
				std::printf("register writes by path: other %llu, PATH1 %llu, PATH2 %llu, PATH3 %llu\n", (unsigned long long)CGSHandler::g_writesByPath[0], (unsigned long long)CGSHandler::g_writesByPath[1],
				            (unsigned long long)CGSHandler::g_writesByPath[2], (unsigned long long)CGSHandler::g_writesByPath[3]);
			}
			if(Ee::CSubSystem::g_ioCounts)
			{
				//The 20 busiest EE I/O register slots
				std::vector<std::pair<uint64, uint32>> slots;
				for(uint32 i = 0; i < 4098; i++)
					if(Ee::CSubSystem::g_ioCount[0][i] + Ee::CSubSystem::g_ioCount[1][i]) slots.emplace_back(Ee::CSubSystem::g_ioCount[0][i] + Ee::CSubSystem::g_ioCount[1][i], i);
				std::sort(slots.rbegin(), slots.rend());
				std::printf("EE I/O by register (reads/writes):");
				for(size_t n = 0; n < std::min<size_t>(slots.size(), 20); n++)
				{
					uint32 i = slots[n].second;
					uint32 address = (i < 4096) ? (0x10000000 + i * 16) : (i == 4096) ? 0x12000000 : 0;
					std::printf(" %08X:%llu/%llu(%.0f/%.0fns)", address, (unsigned long long)Ee::CSubSystem::g_ioCount[0][i], (unsigned long long)Ee::CSubSystem::g_ioCount[1][i], Ee::CSubSystem::g_ioNs[0][i] / std::max(1.0, double(Ee::CSubSystem::g_ioCount[0][i])), Ee::CSubSystem::g_ioNs[1][i] / std::max(1.0, double(Ee::CSubSystem::g_ioCount[1][i])));
				}
				std::printf("\n");
			}
		}
#ifdef VITAPS2_HAS_GL
		if(Re4Vu1Hle::IsEnabled())
			std::printf("world batches on the CPU: %llu (%llu vertices, %llu clipped triangles), on the GPU: %llu batches %llu draws %llu fallbacks\n", (unsigned long long)CGSH_Primitives::g_worldBatches,
			            (unsigned long long)CGSH_Primitives::g_worldVertices, (unsigned long long)CGSH_Primitives::g_worldClipped, (unsigned long long)CGSH_Hardware::g_gpuWorldBatches, (unsigned long long)CGSH_Hardware::g_gpuWorldDraws, (unsigned long long)CGSH_Hardware::g_gpuWorldFallbacks);
		if(Re4Vu1Hle::IsEnabled()) std::printf("most world vertices in one draw: %llu, guard band triangles: %llu\n", (unsigned long long)CGSH_Hardware::g_gpuWorldMaxVertices, (unsigned long long)CGSH_Hardware::g_gpuWorldGuarded);
		if(CGSH_Hardware::g_gpuWorldRetained) std::printf("retained world: %llu inside, %llu outside, %llu edge; %llu hits, %llu misses, %llu evictions, %.1f MB\n", (unsigned long long)CGSH_Hardware::g_retainedInside, (unsigned long long)CGSH_Hardware::g_retainedOutside, (unsigned long long)CGSH_Hardware::g_retainedEdge, (unsigned long long)CGSH_Hardware::g_retainedHits, (unsigned long long)CGSH_Hardware::g_retainedMisses, (unsigned long long)CGSH_Hardware::g_retainedEvictions, CGSH_Hardware::g_retainedMegabytes);
		for(int d = 0; d < 2; d++)
		{
			std::printf(d ? "uploaded pixels by cause:" : "read back pixels by cause:");
			for(int c = 0; c < 16; c++)
				if(CGSH_Hardware::g_roundTripPixels[d][c]) std::printf(" %d:%llu", c, (unsigned long long)CGSH_Hardware::g_roundTripPixels[d][c]);
			std::printf("\n");
		}
		if(CGSH_Hardware::g_gpuWorldRetained) std::printf("retained draws: %llu draw calls, %llu batches merged into the one before\n", (unsigned long long)CGSH_Hardware::g_retainedDrawCalls, (unsigned long long)CGSH_Hardware::g_retainedMerged);
#endif

		if(width == 0)
		{
			std::fprintf(stderr, "no frame was presented\n");
			return 1;
		}
		WritePpm(outPath, pixels, width, height);
		std::printf("wrote %ux%u frame to %s\n", width, height, outPath.c_str());
		sessionPtr = nullptr;
	}
	catch(const std::exception& e)
	{
		std::fprintf(stderr, "error: %s\n", e.what());
		return 1;
	}
	return 0;
}
