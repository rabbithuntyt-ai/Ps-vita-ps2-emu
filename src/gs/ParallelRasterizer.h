#pragma once

// Splits GS rasterization across several threads.
//
// Primitives are recorded into a batch by the GS thread, which performs all
// shared bookkeeping (CSoftwareRasterizer::Prepare). When the batch is flushed
// every thread rasterizes the whole batch, but only the scanlines it owns
// (y % threadCount == index). A pixel is therefore always written by the same
// thread, in primitive order, so the result is identical to single-threaded
// rendering. The GS thread itself acts as lane 0.
//
// The batch must be flushed before anything else reads or writes GS memory:
// transfers, CLUT loads, display readout, and textures whose pages were drawn
// to earlier in the batch (detected automatically).

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "SoftwareRasterizer.h"

class CParallelRasterizer
{
public:
	CParallelRasterizer(CSoftwareRasterizer& front, uint8* ram, const uint16* clut, uint32 threadCount);
	~CParallelRasterizer();

	CParallelRasterizer(const CParallelRasterizer&) = delete;
	CParallelRasterizer& operator=(const CParallelRasterizer&) = delete;

	// Call after changing the front rasterizer's state.
	void OnStateChanged();
	void Submit(CSoftwareRasterizer::PRIMITIVE_KIND, const CSoftwareRasterizer::VERTEX*);
	void Flush();

	uint32 GetThreadCount() const
	{
		return static_cast<uint32>(m_lanes.size());
	}

private:
	enum
	{
		MAX_BATCH = 4096,
	};

	struct COMMAND
	{
		CSoftwareRasterizer::PRIMITIVE_KIND kind;
		uint32 stateIndex;
		CSoftwareRasterizer::VERTEX vertices[3];
	};

	void RunLane(uint32 laneIndex);
	void FlushBatch();
	void WorkerProc(uint32 laneIndex);

	CSoftwareRasterizer& m_front;
	std::vector<std::unique_ptr<CSoftwareRasterizer>> m_lanes;
	std::vector<CSoftwareRasterizer::PREPARED_STATE> m_states;
	std::vector<COMMAND> m_commands;
	bool m_needSnapshot = true;
	uint64 m_layoutKey = 0;
	bool m_serial = false; //run the batch on one thread (memory aliasing)
	uint32 m_rowMask = 0;
	uint32 m_rowValue = 0;

	std::vector<std::thread> m_workers;
	std::mutex m_mutex;
	std::condition_variable m_startCondition;
	std::condition_variable m_doneCondition;
	uint64 m_generation = 0;
	uint32 m_pendingWorkers = 0;
	bool m_quit = false;
};
