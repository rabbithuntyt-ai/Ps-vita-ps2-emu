#include "ParallelRasterizer.h"
#include <string>
#include "ThreadProfiler.h"

CParallelRasterizer::CParallelRasterizer(CSoftwareRasterizer& front, uint8* ram, const uint16* clut, uint32 threadCount)
    : m_front(front)
{
	m_front.GetTextureCache().SetDeferredRelease(true);
	threadCount = std::max<uint32>(threadCount, 1);
	for(uint32 i = 0; i < threadCount; i++)
	{
		auto lane = std::make_unique<CSoftwareRasterizer>();
		lane->SetMemory(ram, clut);
		lane->SetLane(i, threadCount);
		m_lanes.push_back(std::move(lane));
	}
	m_commands.reserve(MAX_BATCH);
	for(uint32 i = 1; i < threadCount; i++)
	{
		m_workers.emplace_back([this, i]() { WorkerProc(i); });
	}
}

CParallelRasterizer::~CParallelRasterizer()
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_quit = true;
	}
	m_startCondition.notify_all();
	for(auto& worker : m_workers)
	{
		worker.join();
	}
	m_front.GetTextureCache().SetDeferredRelease(false);
}

void CParallelRasterizer::OnStateChanged()
{
	m_needSnapshot = true;
}

void CParallelRasterizer::Submit(CSoftwareRasterizer::PRIMITIVE_KIND kind, const CSoftwareRasterizer::VERTEX* vertices)
{
	// A texture that was drawn to earlier in this batch must see those pixels,
	// and row ownership only holds within a single buffer layout.
	uint64 layoutKey = m_front.GetLayoutKey();
	if(!m_commands.empty() && (m_front.PendingWritesOverlapTexture() || (layoutKey != m_layoutKey)))
	{
		Flush();
	}
	if(m_commands.empty())
	{
		m_layoutKey = layoutKey;
		m_serial = m_front.IsLayoutAliased();
	}
	if(!m_front.Prepare(kind, vertices)) return;
	// Feedback: this primitive writes into the texture it samples. Run it on
	// its own so nothing after it reads stale texels.
	bool feedback = m_front.PendingWritesOverlapTexture();

	auto prepared = m_front.GetPreparedState();
	if(m_needSnapshot || m_states.empty() || (m_states.back().texture != prepared.texture) ||
	   (m_states.back().texels != prepared.texels) || (m_states.back().texelsGeneration != prepared.texelsGeneration))
	{
		m_states.push_back(prepared);
		m_needSnapshot = false;
	}

	COMMAND command;
	command.kind = kind;
	command.stateIndex = static_cast<uint32>(m_states.size() - 1);
	uint32 count = (kind == CSoftwareRasterizer::PRIMITIVE_TRIANGLE) ? 3 : (kind == CSoftwareRasterizer::PRIMITIVE_POINT) ? 1 : 2;
	for(uint32 i = 0; i < count; i++)
	{
		command.vertices[i] = vertices[i];
	}
	m_commands.push_back(command);

	if(feedback)
	{
		// Lanes would read texels other lanes are writing.
		m_serial = true;
		Flush();
	}
	else if(m_commands.size() >= MAX_BATCH)
	{
		Flush();
	}
}

void CParallelRasterizer::Flush()
{
	if(m_commands.empty())
	{
		m_front.ClearPendingWrites();
		return;
	}

	m_rowMask = m_front.GetRowMask();
	m_rowValue = m_front.GetRowValue();
	FlushBatch();
	m_serial = false;

	m_commands.clear();
	m_states.clear();
	m_needSnapshot = true;
	m_front.ClearPendingWrites();
	m_front.GetTextureCache().ReleaseRetired();
}

void CParallelRasterizer::FlushBatch()
{
	if(m_serial || m_workers.empty())
	{
		m_lanes[0]->SetLane(0, 1);
		RunLane(0);
		m_lanes[0]->SetLane(0, static_cast<uint32>(m_lanes.size()));
		return;
	}

	if(!m_workers.empty())
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_pendingWorkers = static_cast<uint32>(m_workers.size());
		m_generation++;
	}
	m_startCondition.notify_all();

	RunLane(0);

	if(!m_workers.empty())
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		m_doneCondition.wait(lock, [this]() { return m_pendingWorkers == 0; });
	}
}

void CParallelRasterizer::RunLane(uint32 laneIndex)
{
	auto& lane = *m_lanes[laneIndex];
	lane.SetRowFilter(m_rowMask, m_rowValue);
	uint32 currentState = ~0U;
	for(const auto& command : m_commands)
	{
		if(command.stateIndex != currentState)
		{
			lane.ApplyPrepared(m_states[command.stateIndex]);
			currentState = command.stateIndex;
		}
		switch(command.kind)
		{
		case CSoftwareRasterizer::PRIMITIVE_POINT:
			lane.DrawPoint(command.vertices[0]);
			break;
		case CSoftwareRasterizer::PRIMITIVE_LINE:
			lane.DrawLine(command.vertices[0], command.vertices[1]);
			break;
		case CSoftwareRasterizer::PRIMITIVE_TRIANGLE:
			lane.DrawTriangle(command.vertices[0], command.vertices[1], command.vertices[2]);
			break;
		case CSoftwareRasterizer::PRIMITIVE_SPRITE:
			lane.DrawSprite(command.vertices[0], command.vertices[1]);
			break;
		}
	}
}

void CParallelRasterizer::WorkerProc(uint32 laneIndex)
{
	std::string name = "GS worker " + std::to_string(laneIndex);
	ThreadProfiler::RegisterCurrentThread(name.c_str());
	uint64 seenGeneration = 0;
	while(true)
	{
		{
			std::unique_lock<std::mutex> lock(m_mutex);
			m_startCondition.wait(lock, [&]() { return m_quit || (m_generation != seenGeneration); });
			if(m_quit) return;
			seenGeneration = m_generation;
		}
		RunLane(laneIndex);
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_pendingWorkers--;
		}
		m_doneCondition.notify_one();
	}
}
