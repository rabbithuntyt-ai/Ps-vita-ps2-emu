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
		SECTION_COUNT
	};

	inline const char* GetSectionName(unsigned int section)
	{
		static const char* names[SECTION_COUNT] = {"idle", "other", "registers", "vertex", "primitive", "addvertex", "state", "texture", "target", "flush", "upload", "download", "transfer", "world", "display"};
		return (section < SECTION_COUNT) ? names[section] : "?";
	}

	inline std::atomic<uint8_t> g_section{SECTION_IDLE};
	inline std::atomic<uint64_t> g_samples[SECTION_COUNT] = {};

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
		if(section < SECTION_COUNT) g_samples[section].fetch_add(1, std::memory_order_relaxed);
	}
}
