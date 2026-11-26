#pragma once

// Statistical profile of the GS (GPU) thread: the thread stores the section it is in (one
// relaxed store on entry and one on exit, no clock reads), the session's 1 ms profiler thread
// counts samples per section. Sections nest: an inner scope wins and the outer one comes back
// when it ends. perf.csv gs_sections (one count per section per second, in this order).
#include <atomic>
#include <cstdint>

namespace GsProfile
{
	enum SECTION : uint8_t
	{
		SECTION_IDLE,      //waiting for mailbox calls
		SECTION_OTHER,     //mailbox calls outside the sections below (image data writes into GS memory...)
		SECTION_REGISTERS, //register writes (dispatch, state decoding)
		SECTION_VERTEX,    //vertex kicks: the primitive assembly and vertex conversion
		SECTION_PRIMITIVE, //primitives into batches (OnDraw, strip triangles)
		SECTION_ADD_VERTEX, //of which: GL vertex conversion
		SECTION_STATE,     //state application (ApplyState, GL state)
		SECTION_TEXTURE,   //texture binding (cache lookups, decoding, uploads)
		SECTION_TARGET,    //render targets: finding, creating, relayouts, trims
		SECTION_FLUSH,     //batches handed to GL (draw calls)
		SECTION_UPLOAD,    //target rows uploaded from GS memory
		SECTION_DOWNLOAD,  //targets read back into GS memory
		SECTION_TRANSFER,  //host to local transfers handled by the renderer
		SECTION_WORLD,     //RE4 world batches
		SECTION_DISPLAY,   //flips
		SECTION_WORLD_RETAINED, //of world: retained batches (lookup, uniforms, merge)
		SECTION_WORLD_SKIN,     //of world: projected skinning batches (vertices into the slot)
		SECTION_FEED_WRITE,     //host to local image data written into GS memory (TransferWrite)
		SECTION_GL_DRAW,        //inside the GL draw calls of generic batches (glDrawArrays/glDrawElements: vitaGL's copies and command building)
		SECTION_GL_DRAW_WORLD,  //inside the GL draw calls of world slot draws (world and skinning batches)
		SECTION_GL_DRAW_RETAINED, //inside the GL draw calls of retained world draws
		SECTION_WORLD_SETUP,    //of world: the batch's strip start, GS state applied, program check
		SECTION_WORLD_VERTS,    //of world: slot path vertices, clip flags and indices (not skinning)
		SECTION_WORLD_END,      //of world: the batch's last vertices projected, strip end, target marked
		SECTION_WORLD_CLIP,     //of world: triangles touching the clip volume clipped on the CPU (drawing them is primitive/addvertex)
		SECTION_TARGET_RELAYOUT, //of target: old targets copied into a new layout on the GPU
		SECTION_TARGET_ALLOC,   //of target: GL objects of a target (pool or new)
		SECTION_TARGET_REMOVE,  //of target: targets in the way removed (read-backs themselves are download)
		SECTION_TARGET_RESIZE,  //of target: shrinks and front trims
		SECTION_TEX_HASH,       //of texture: page hashes of rewritten or new texture cache entries
		SECTION_TEX_CREATE,     //of texture: new texture cache entries (texel storage, CLUT snapshot)
		SECTION_ALPHA_CLUT,     //of texture: alpha index textures looked up on the GPU
		SECTION_SKIN_LOOP,      //of worldSkin: the projected skinning vertices and indices into the slot (fast_skin_loop)
		SECTION_BATCH_FREE,     //world and skinning batches released after their draw (their memory freed)
		SECTION_END_KICK,       //of worldEnd: the batch's last two vertices kicked into the GS registers
		SECTION_COUNT
	};

	inline const char* GetSectionName(unsigned int section)
	{
		static const char* names[SECTION_COUNT] = {"idle", "other", "registers", "vertex", "primitive", "addvertex", "state", "texture", "target", "flush", "upload", "download", "transfer", "world", "display", "worldRet", "worldSkin", "feedWrite", "glDraw", "glDrawWorld", "glDrawRet", "worldSetup", "worldVerts", "worldEnd", "worldClip", "tgtRelayout", "tgtAlloc", "tgtRemove", "tgtResize", "texHash", "texCreate", "alphaClut", "skinLoop", "batchFree", "endKick"};
		return (section < SECTION_COUNT) ? names[section] : "?";
	}

	inline std::atomic<uint8_t> g_section{SECTION_IDLE};
	inline std::atomic<uint64_t> g_samples[SECTION_COUNT] = {};
	// gs_wait_split (Vita): with each sample the profiler thread also reads the GS thread's
	// scheduler state (0 running on a core, 1 ready but not running, 2 blocked: waiting in the
	// kernel, e.g. inside GXM/vitaGL), so each section splits into CPU-active and blocked samples.
	// 'g_threadState' is set by the platform; null = off (no cost).
	inline int (*g_threadState)() = nullptr;
	inline std::atomic<uint64_t> g_readySamples[SECTION_COUNT] = {};
	inline std::atomic<uint64_t> g_blockedSamples[SECTION_COUNT] = {};

	class CScope
	{
	public:
		explicit CScope(SECTION section)
		    : m_previous(g_section.load(std::memory_order_relaxed))
		{
			g_section.store(section, std::memory_order_relaxed);
		}
		~CScope()
		{
			g_section.store(m_previous, std::memory_order_relaxed);
		}
		CScope(const CScope&) = delete;
		CScope& operator=(const CScope&) = delete;

	private:
		uint8_t m_previous;
	};

	//The profiler thread, every millisecond
	inline void Sample()
	{
		unsigned int section = g_section.load(std::memory_order_relaxed);
		if(section >= SECTION_COUNT) return;
		g_samples[section].fetch_add(1, std::memory_order_relaxed);
		if(g_threadState)
		{
			int state = g_threadState();
			if(state == 1) g_readySamples[section].fetch_add(1, std::memory_order_relaxed);
			else if(state == 2) g_blockedSamples[section].fetch_add(1, std::memory_order_relaxed);
		}
	}
}
