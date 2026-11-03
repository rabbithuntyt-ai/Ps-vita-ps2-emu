#include "MpegShadow.h"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include "ee/IPU.h"

namespace
{
	uint32_t Read32(const uint8_t* ram, uint32_t address)
	{
		uint32_t value;
		std::memcpy(&value, ram + (address & 0x01FFFFFF), 4);
		return value;
	}

	int64_t Read64(const uint8_t* ram, uint32_t address)
	{
		int64_t value;
		std::memcpy(&value, ram + (address & 0x01FFFFFF), 8);
		return value;
	}

	// Pictures decoded ahead of libmpeg are kept for comparison; beyond this
	// the shadow fell out of step.
	constexpr size_t MAX_QUEUED = 8;

	// Pixel i of 16 spread over the picture (for the color conversion samples).
	void SamplePosition(unsigned int i, uint32_t width, uint32_t height, uint32_t& x, uint32_t& y)
	{
		x = (width / 5) * (1 + (i % 4)) + 3;
		y = (height / 5) * (1 + (i / 4)) + 5;
	}
}

CMpegShadow::~CMpegShadow()
{
	Disable();
}

void CMpegShadow::Enable(const uint8_t* eeRam, uint32_t mpAddress, const char* logPath)
{
	Disable();
	m_ram = eeRam;
	m_mpAddress = mpAddress;
	m_log = std::fopen(logPath, "w");
	m_lastFrameCount = Read32(m_ram, m_mpAddress + 8);
	m_decoder.SetIpuCompatibleQuantisation(true);
	CIPU::SetInputObserver(&CMpegShadow::OnInput, this);
}

void CMpegShadow::Disable()
{
	CIPU::SetInputObserver(nullptr, nullptr);
	if(m_log) std::fclose(m_log);
	m_log = nullptr;
	m_ram = nullptr;
}

void CMpegShadow::OnInput(void* context, const uint8_t* data, uint32_t size)
{
	auto shadow = static_cast<CMpegShadow*>(context);
	std::lock_guard<std::mutex> lock(shadow->m_mutex);
	// sceIpuStopDMA/RestartDMA rewind DMA4 by what was left in the IPU's
	// input FIFO and send it again: skip data delivered already. (A wrap to
	// the start of the ring is a big jump back, not an overlap.)
	const uint8_t* end = data + size;
	if(shadow->m_lastInputEnd && (data < shadow->m_lastInputEnd) && (shadow->m_lastInputEnd - data <= 0x200))
	{
		data = std::min(shadow->m_lastInputEnd, end);
	}
	if(end > shadow->m_lastInputEnd || (shadow->m_lastInputEnd - end > 0x200)) shadow->m_lastInputEnd = end;
	shadow->m_incoming.insert(shadow->m_incoming.end(), data, end);
}

void CMpegShadow::Poll()
{
	if(!m_ram) return;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if(!m_incoming.empty())
		{
			m_stream.insert(m_stream.end(), m_incoming.begin(), m_incoming.end());
			m_incoming.clear();
		}
	}

	// Decode every complete picture available.
	while(m_pictures.size() < MAX_QUEUED)
	{
		size_t consumed = 0;
		auto start = std::chrono::steady_clock::now();
		auto result = m_decoder.Decode(m_stream.data(), m_stream.size(), false, consumed);
		double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		m_stream.erase(m_stream.begin(), m_stream.begin() + consumed);
		if(result != CMpeg2Decoder::RESULT_PICTURE)
		{
			if(result == CMpeg2Decoder::RESULT_NEED_DATA) break;
			if(m_log)
			{
				std::fprintf(m_log, "native decoder: result %d at frame %u, skipping to the next start code\n", result, m_lastFrameCount);
				std::fflush(m_log);
			}
			// Skip the bad picture's start code; the next Decode resyncs.
			m_stream.erase(m_stream.begin(), m_stream.begin() + std::min<size_t>(4, m_stream.size()));
			continue;
		}
		const auto& frame = m_decoder.GetPicture();
		PICTURE picture;
		picture.type = frame.pictureType;
		picture.temporalReference = frame.temporalReference;
		picture.decodeMs = ms;
		picture.rgb.resize((frame.width / 16) * (frame.height / 16) * 256);
		CMpeg2Decoder::ConvertToRgb32Macroblocks(frame, picture.rgb.data(), ~0u);
		for(unsigned int i = 0; i < 16; i++)
		{
			uint32_t x, y;
			SamplePosition(i, frame.width, frame.height, x, y);
			picture.samples[i][0] = frame.y[y * frame.width + x];
			picture.samples[i][1] = frame.cb[(y / 2) * (frame.width / 2) + x / 2];
			picture.samples[i][2] = frame.cr[(y / 2) * (frame.width / 2) + x / 2];
		}
		m_width = frame.width;
		m_height = frame.height;
		{
			uint32_t mbWidth = frame.width / 16, mbHeight = frame.height / 16;
			picture.raw8.resize(mbWidth * mbHeight * 0x180);
			uint8_t* out = picture.raw8.data();
			for(uint32_t mby = 0; mby < mbHeight; mby++)
			{
				for(uint32_t mbx = 0; mbx < mbWidth; mbx++, out += 0x180)
				{
					for(unsigned int j = 0; j < 16; j++) std::memcpy(out + j * 16, &frame.y[(mby * 16 + j) * frame.width + mbx * 16], 16);
					for(unsigned int j = 0; j < 8; j++)
					{
						std::memcpy(out + 0x100 + j * 8, &frame.cb[(mby * 8 + j) * (frame.width / 2) + mbx * 8], 8);
						std::memcpy(out + 0x140 + j * 8, &frame.cr[(mby * 8 + j) * (frame.width / 2) + mbx * 8], 8);
					}
				}
			}
		}
		m_pictures.push_back(std::move(picture));
		m_decodeMsTotal += ms;
		m_decoded++;
	}

	uint32_t frameCount = Read32(m_ram, m_mpAddress + 8);
	while((frameCount != m_lastFrameCount) && !m_pictures.empty())
	{
		m_lastFrameCount++;
		Compare(m_pictures.front(), m_lastFrameCount);
		m_pictures.pop_front();
	}
	m_lastFrameCount = frameCount;
}

void CMpegShadow::Compare(const PICTURE& picture, uint32_t frameCount)
{
	if(!m_log) return;
	uint32_t sys = Read32(m_ram, m_mpAddress + 0x40);
	uint32_t buffer = Read32(m_ram, sys + 0xD8) & 0x01FFFFFF;
	uint32_t capacity = Read32(m_ram, sys + 0xE4);
	size_t pixels = std::min<size_t>(picture.rgb.size(), static_cast<size_t>(capacity) * 256);
	uint64_t absDiff = 0, exact = 0;
	for(size_t i = 0; i < pixels; i++)
	{
		uint32_t emulated;
		std::memcpy(&emulated, m_ram + ((buffer + i * 4) & 0x01FFFFFF), 4);
		uint32_t native = picture.rgb[i];
		if((emulated & 0x00FFFFFF) == (native & 0x00FFFFFF)) exact++;
		for(int c = 0; c < 3; c++) absDiff += std::abs(static_cast<int>((emulated >> (c * 8)) & 0xFF) - static_cast<int>((native >> (c * 8)) & 0xFF));
	}
	const char types[] = "?IPB";
	std::fprintf(m_log, "frame %u pts %lld dts %lld flags %llx | native %c tref %u %.1f ms | mean diff %.2f exact %.1f%% | queued %u, avg %.1f ms\n",
	             frameCount, static_cast<long long>(Read64(m_ram, m_mpAddress + 0x10)), static_cast<long long>(Read64(m_ram, m_mpAddress + 0x18)),
	             static_cast<unsigned long long>(Read64(m_ram, m_mpAddress + 0x20)), types[picture.type & 3], picture.temporalReference,
	             picture.decodeMs, pixels ? static_cast<double>(absDiff) / (pixels * 3) : 0.0, pixels ? exact * 100.0 / pixels : 0.0,
	             static_cast<unsigned int>(m_pictures.size()), m_decoded ? m_decodeMsTotal / m_decoded : 0.0);
	// The native picture against libmpeg's four RAW8 frame buffers (decoding is
	// bit exact: one of them should match exactly).
	{
		static const uint32_t frameOffsets[4] = {0xFC, 0x100, 0x104, 0x110};
		int best = -1;
		double bestDiff = 1e9;
		uint64_t bestExact = 0;
		for(int k = 0; k < 4; k++)
		{
			uint32_t frameBuffer = Read32(m_ram, sys + frameOffsets[k]) & 0x01FFFFFF;
			if(frameBuffer == 0) continue;
			uint64_t diff = 0, same = 0;
			for(size_t i = 0; i < picture.raw8.size(); i++)
			{
				int emulated = m_ram[(frameBuffer + i) & 0x01FFFFFF];
				int native = picture.raw8[i];
				diff += std::abs(emulated - native);
				same += (emulated == native);
			}
			double mean = picture.raw8.empty() ? 0.0 : static_cast<double>(diff) / picture.raw8.size();
			if(mean < bestDiff)
			{
				bestDiff = mean;
				best = k;
				bestExact = same;
			}
		}
		std::fprintf(m_log, "  RAW8 best match frame buffer %d: mean diff %.3f, %.2f%% bytes equal\n", best, bestDiff,
		             picture.raw8.empty() ? 0.0 : bestExact * 100.0 / picture.raw8.size());
		// Both versions of the first I picture, for analysis on a PC.
		if(!m_dumpedIntra && (picture.type == 1) && (best >= 0))
		{
			m_dumpedIntra = true;
			uint32_t frameBuffer = Read32(m_ram, sys + frameOffsets[best]) & 0x01FFFFFF;
			if(FILE* file = std::fopen("ux0:data/VitaPS2/mpeg_native.raw8", "wb"))
			{
				std::fwrite(picture.raw8.data(), 1, picture.raw8.size(), file);
				std::fclose(file);
			}
			if(FILE* file = std::fopen("ux0:data/VitaPS2/mpeg_libmpeg.raw8", "wb"))
			{
				std::fwrite(m_ram + frameBuffer, 1, picture.raw8.size(), file);
				std::fclose(file);
			}
		}
	}
	// Color conversion samples: native Y Cb Cr -> libmpeg's R G B (first pictures).
	if((frameCount <= 2) && (m_width != 0))
	{
		uint32_t mbWidth = m_width / 16;
		for(unsigned int i = 0; i < 16; i++)
		{
			uint32_t x, y;
			SamplePosition(i, m_width, m_height, x, y);
			uint32_t index = ((y / 16) * mbWidth + (x / 16)) * 256 + (y % 16) * 16 + (x % 16);
			uint32_t emulated;
			std::memcpy(&emulated, m_ram + ((buffer + index * 4) & 0x01FFFFFF), 4);
			std::fprintf(m_log, "  sample %u,%u ycbcr %u %u %u -> rgb %u %u %u a %u\n", x, y, picture.samples[i][0], picture.samples[i][1],
			             picture.samples[i][2], emulated & 0xFF, (emulated >> 8) & 0xFF, (emulated >> 16) & 0xFF, emulated >> 24);
		}
	}
	std::fflush(m_log);
}
