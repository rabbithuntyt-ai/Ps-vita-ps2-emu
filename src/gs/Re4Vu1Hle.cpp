#include "Re4Vu1Hle.h"
#include <cstring>
#include <memory>
#include "GSH_Primitives.h"
#include "Re4Vu1Batch.h"
#include "ee/GIF.h"
#include "ee/Vif.h"
#include "ee/Vpu.h"

namespace
{
	bool g_enabled = false;

	// The world program: B 0x1040 at 0x1408 and the start of its vertex loop (MULA at 0x12B8).
	bool IsWorldProgram(const uint8* micro)
	{
		uint32 at1408 = *reinterpret_cast<const uint32*>(micro + 0x1408);
		uint32 at12B8 = *reinterpret_cast<const uint32*>(micro + 0x12B8 + 4);
		uint32 at13F0 = *reinterpret_cast<const uint32*>(micro + 0x13F0);
		return (at1408 == 0x40000786) && (at12B8 == 0x01FE69BC) && (at13F0 == 0x800016FC);
	}

	std::shared_ptr<void> Replace(CVpu& vpu, uint32 address)
	{
		if(address != 0x1408) return nullptr;
		const uint8* micro = vpu.GetMicroMemory();
		if(!IsWorldProgram(micro)) return nullptr;
		const uint8* mem = vpu.GetVuMemory();
		auto& state = vpu.GetContext().m_State;
		uint32 top = vpu.GetVif().GetTOP() & 0x3FF;
		uint32 vi5 = state.nCOP2VI[5] & 0x3FF;
		auto qword = [mem](uint32 index) { return mem + ((index & 0x3FF) * 16); };
		uint32 header[4];
		std::memcpy(header, qword(top + 0x20), 16);
		uint64 tag = static_cast<uint64>(header[0]) | (static_cast<uint64>(header[1]) << 32);
		uint32 nreg = static_cast<uint32>(tag >> 60) & 0xF;
		uint32 flg = static_cast<uint32>(tag >> 58) & 3;
		bool eop = ((tag >> 15) & 1) != 0;
		// PACKED, 3 registers: ST, RGBAQ, XYZ2 (what the program's output holds)
		if((nreg != 3) || (flg != 0) || !eop || ((header[2] & 0xFFF) != 0x512)) return nullptr;
		auto batch = std::make_shared<Re4Vu1::WORLD_BATCH>();
		batch->tagLow = tag;
		batch->count = header[0] & 0x7FFF;
		std::memcpy(&batch->scale, &header[3], 4);
		for(uint32 r = 0; r < 4; r++)
		{
			std::memcpy(&batch->projection[r], qword(vi5 + r), 16);
			std::memcpy(&batch->clip[r], qword(vi5 + 4 + r), 16);
		}
		std::memcpy(&batch->colorOffset, qword(vi5 - 2), 16);
		std::memcpy(&batch->colorScale, qword(vi5 - 1), 16);
		std::memcpy(&batch->vf31, &state.nCOP2[31], 16);
		batch->raw.resize(batch->count * 12);
		for(uint32 i = 0; i < batch->count * 3; i++)
		{
			std::memcpy(batch->raw.data() + i * 4, qword(top + 0x21 + i), 16);
		}
		// Where the program stops (E bit at 0x13F8): MSCNT continues there.
		state.nPC = 0x1408;
		return batch;
	}

	void Submit(CGIF& gif, const std::shared_ptr<void>& data)
	{
		auto batch = std::static_pointer_cast<Re4Vu1::WORLD_BATCH>(data);
		auto gs = gif.GetGsHandler();
		// The tag's PRIM (PRE), then everything written before goes to the GS thread first.
		if((batch->tagLow >> 46) & 1)
		{
			gs->WriteRegister(CGSHandler::RegisterWrite(static_cast<uint8>(GS_REG_PRIM), (batch->tagLow >> 47) & 0x7FF));
		}
		gs->ProcessWriteBuffer(nullptr);
		gs->SubmitWriteBuffer();
		auto primitives = static_cast<CGSH_Primitives*>(gs);
		gs->SendGSCall([primitives, batch]() { primitives->DrawWorldBatch(*batch); });
	}
}

void Re4Vu1Hle::Enable(bool enabled)
{
	g_enabled = enabled;
	CVpu::g_programReplacement = enabled ? std::function<std::shared_ptr<void>(CVpu&, uint32)>(Replace) : nullptr;
	CVpu::g_customKickSubmit = enabled ? std::function<void(CGIF&, const std::shared_ptr<void>&)>(Submit) : nullptr;
}

bool Re4Vu1Hle::IsEnabled()
{
	return g_enabled;
}
