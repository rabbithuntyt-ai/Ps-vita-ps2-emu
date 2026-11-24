// Render target pixels <-> GS memory (src/gs/hw/TargetPixels.h): the block versions against
// the per pixel ones, for PSMCT32, PSMCT24, PSMCT16 and PSMCT16S, on random rectangles (block
// aligned and not), buffer widths, positions and memory. Pixels read and memory written must
// match exactly.
#include <cstdio>
#include <random>
#include <vector>
#include "TargetPixels.h"

int main()
{
	GS_SURFACE::InitTables();
	std::mt19937 rng(4321);
	const uint32 psms[] = {CGSHandler::PSMCT32, CGSHandler::PSMCT24, CGSHandler::PSMCT16, CGSHandler::PSMCT16S};
	const char* names[] = {"PSMCT32", "PSMCT24", "PSMCT16", "PSMCT16S"};
	int failures = 0;
	for(uint32 p = 0; p < 4; p++)
	{
		uint32 psm = psms[p];
		if(!TargetPixels::GetBlockLayout(psm).valid)
		{
			std::printf("  FAIL %s: no block layout\n", names[p]);
			failures++;
			continue;
		}
		int readFailures = 0, writeFailures = 0;
		for(int trial = 0; trial < 300; trial++)
		{
			uint32 fbw = 1 + rng() % 16;
			uint32 fbp = (rng() % 400) * 8192;
			uint32 x0 = rng() % (fbw * 64);
			uint32 y0 = rng() % 512;
			uint32 width = 1 + rng() % std::max<uint32>(fbw * 64 - x0, 1);
			uint32 height = 1 + rng() % 300;
			if(trial % 3 == 0)
			{
				//block aligned
				x0 &= ~15u;
				y0 &= ~7u;
				width = std::max<uint32>(width & ~15u, 16);
				height = std::max<uint32>(height & ~7u, 8);
				if(x0 + width > fbw * 64) x0 = 0;
			}
			std::vector<uint8> ram(CGSHandler::RAMSIZE);
			for(auto& b : ram) b = static_cast<uint8>(rng());
			std::vector<uint32> a(width * height), b(width * height);
			TargetPixels::Read(ram.data(), psm, fbp, fbw, x0, y0, width, height, a.data());
			TargetPixels::ReadPixels(ram.data(), psm, fbp, fbw, x0, y0, width, height, b.data(), width);
			if(a != b) readFailures++;
			std::vector<uint32> pixels(width * height);
			for(auto& v : pixels) v = rng();
			std::vector<uint8> ramB = ram;
			TargetPixels::Write(ram.data(), psm, fbp, fbw, x0, y0, width, height, pixels.data());
			TargetPixels::WritePixels(ramB.data(), psm, fbp, fbw, x0, y0, width, height, pixels.data(), width);
			if(ram != ramB) writeFailures++;
		}
		std::printf("  %s %s: 300 rectangles, read %d differ, write %d differ\n", (readFailures || writeFailures) ? "FAIL" : "ok  ", names[p], readFailures, writeFailures);
		failures += readFailures + writeFailures;
	}
	uint32 selfTest = TargetPixels::SelfTest(100);
	std::printf("  %s self-test (as the Vita runs it): %u of 100 differ\n", selfTest ? "FAIL" : "ok  ", selfTest);
	failures += selfTest;
	std::printf("%s\n", failures ? "FAILED" : "all rectangles match");
	return failures ? 1 : 0;
}
