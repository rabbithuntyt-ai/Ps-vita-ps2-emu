// Native MPEG-2 decoder test: mpeg2_decoder_tests MOVIE.pss [max pictures] [--yuv OUT] [--ipu]
// Demuxes the video stream of a PSS (MPEG-2 program stream), decodes it and
// writes every 30th picture as MOVIE.pss.NNNN.ppm. Prints pictures, types and
// decoding speed. Without an argument (ctest) there is nothing to decode.
// --yuv writes every picture (output order) as planar 4:2:0 to OUT, to compare
// with a reference decoder (ffmpeg -f rawvideo -pix_fmt yuv420p); --ipu
// selects the emulated IPU's quantisation.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "Mpeg2Decoder.h"

namespace
{
	// Video elementary stream (stream id 0xE0) of an MPEG-2 program stream.
	std::vector<uint8_t> DemuxVideo(const std::vector<uint8_t>& ps)
	{
		std::vector<uint8_t> es;
		size_t position = 0;
		while(position + 4 <= ps.size())
		{
			if((ps[position] != 0) || (ps[position + 1] != 0) || (ps[position + 2] != 1))
			{
				position++;
				continue;
			}
			uint8_t id = ps[position + 3];
			if(id == 0xB9) break; //program end
			if(id == 0xBA) //pack header (MPEG-2: 14 bytes + stuffing)
			{
				if(position + 14 > ps.size()) break;
				position += 14 + (ps[position + 13] & 7);
				continue;
			}
			if(id < 0xBB)
			{
				position++;
				continue;
			}
			if(position + 6 > ps.size()) break;
			size_t length = (ps[position + 4] << 8) | ps[position + 5];
			size_t payload = position + 6;
			size_t end = std::min(payload + length, ps.size());
			if((id & 0xF0) == 0xE0)
			{
				size_t headerLength = (payload + 3 <= end) ? ps[payload + 2] : 0;
				size_t data = payload + 3 + headerLength;
				if(data < end) es.insert(es.end(), ps.begin() + data, ps.begin() + end);
			}
			position = end;
		}
		return es;
	}

	void WritePpm(const char* path, const CMpeg2Decoder::FRAME& frame)
	{
		std::vector<uint32_t> rgb(frame.width * frame.height);
		CMpeg2Decoder::ConvertToRgb32Macroblocks(frame, rgb.data(), ~0u);
		FILE* file = std::fopen(path, "wb");
		if(!file) return;
		std::fprintf(file, "P6\n%u %u\n255\n", frame.width, frame.height);
		uint32_t mbWidth = frame.width / 16;
		for(uint32_t y = 0; y < frame.height; y++)
		{
			for(uint32_t x = 0; x < frame.width; x++)
			{
				uint32_t mb = (y / 16) * mbWidth + (x / 16);
				uint32_t pixel = rgb[mb * 256 + (y % 16) * 16 + (x % 16)];
				uint8_t bytes[3] = {static_cast<uint8_t>(pixel), static_cast<uint8_t>(pixel >> 8), static_cast<uint8_t>(pixel >> 16)};
				std::fwrite(bytes, 1, 3, file);
			}
		}
		std::fclose(file);
	}
}

int main(int argc, char** argv)
{
	if(argc < 2)
	{
		std::printf("usage: mpeg2_decoder_tests MOVIE.pss [max pictures]\n");
		return 0;
	}
	unsigned int maxPictures = ((argc > 2) && (argv[2][0] != '-')) ? static_cast<unsigned int>(std::atoi(argv[2])) : 300;
	FILE* yuv = nullptr;
	bool ipu = false;
	for(int i = 2; i < argc; i++)
	{
		if(!std::strcmp(argv[i], "--yuv") && (i + 1 < argc)) yuv = std::fopen(argv[++i], "wb");
		else if(!std::strcmp(argv[i], "--ipu")) ipu = true;
	}
	std::vector<uint8_t> ps;
	if(FILE* file = std::fopen(argv[1], "rb"))
	{
		std::fseek(file, 0, SEEK_END);
		ps.resize(std::ftell(file));
		std::fseek(file, 0, SEEK_SET);
		size_t read = std::fread(ps.data(), 1, ps.size(), file);
		ps.resize(read);
		std::fclose(file);
	}
	auto es = DemuxVideo(ps);
	std::printf("program stream %zu bytes, video %zu bytes\n", ps.size(), es.size());

	CMpeg2Decoder decoder;
	decoder.SetIpuCompatibleQuantisation(ipu);
	size_t position = 0;
	unsigned int pictures = 0;
	char types[4] = {'?', 'I', 'P', 'B'};
	std::string order;
	auto start = std::chrono::steady_clock::now();
	while(pictures < maxPictures)
	{
		size_t consumed = 0;
		auto result = decoder.Decode(es.data() + position, es.size() - position, true, consumed);
		position += consumed;
		if(result == CMpeg2Decoder::RESULT_PICTURE)
		{
			const auto& picture = decoder.GetPicture();
			if(order.size() < 60) order += types[picture.pictureType & 3];
			if(yuv)
			{
				std::fwrite(picture.y.data(), 1, picture.y.size(), yuv);
				std::fwrite(picture.cb.data(), 1, picture.cb.size(), yuv);
				std::fwrite(picture.cr.data(), 1, picture.cr.size(), yuv);
			}
			if((pictures % 30) == 0)
			{
				char path[512];
				std::snprintf(path, sizeof(path), "%s.%04u.ppm", argv[1], pictures);
				WritePpm(path, picture);
			}
			pictures++;
			continue;
		}
		const char* names[] = {"picture", "need data", "end", "unsupported", "error"};
		std::printf("stopped: %s at byte %zu\n", names[result], position);
		if((result != CMpeg2Decoder::RESULT_END) || (consumed == 0)) break;
	}
	double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	std::printf("%u pictures (%s...) in %.2f s: %.1f pictures/s\n", pictures, order.c_str(), seconds, pictures / seconds);
	if(yuv) std::fclose(yuv);
	return (pictures != 0) ? 0 : 1;
}
