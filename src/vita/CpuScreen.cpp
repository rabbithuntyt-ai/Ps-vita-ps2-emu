#include "CpuScreen.h"
#include <cstdint>
#include <cstring>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/kernel/sysmem.h>
#include "FontData.h"

namespace
{
	constexpr int WIDTH = 960, HEIGHT = 544, PITCH = 960;

	void DrawText(uint32_t* fb, int x, int y, uint32_t color, const std::string& text)
	{
		int penX = x, top = y;
		for(char ch : text)
		{
			int code = static_cast<unsigned char>(ch);
			if(code == '\n')
			{
				penX = x;
				top += FontData::CELL_HEIGHT;
				continue;
			}
			if((code < FontData::FIRST_CHAR) || (code > FontData::LAST_CHAR)) code = '?';
			int index = code - FontData::FIRST_CHAR;
			int srcX = (index % FontData::COLUMNS) * FontData::CELL_WIDTH;
			int srcY = (index / FontData::COLUMNS) * FontData::CELL_HEIGHT;
			for(int row = 0; row < FontData::CELL_HEIGHT; row++)
			{
				int dy = top + row;
				if(dy < 0 || dy >= HEIGHT) continue;
				for(int col = 0; col < FontData::CELL_WIDTH; col++)
				{
					int dx = penX + col;
					if(dx < 0 || dx >= WIDTH) continue;
					uint32_t alpha = FontData::ALPHA[(srcY + row) * FontData::ATLAS_WIDTH + srcX + col];
					if(alpha == 0) continue;
					uint32_t& pixel = fb[dy * PITCH + dx];
					uint32_t blended = 0xFF000000;
					for(int shift = 0; shift < 24; shift += 8)
					{
						uint32_t src = (color >> shift) & 0xFF, dst = (pixel >> shift) & 0xFF;
						blended |= ((src * alpha + dst * (255 - alpha)) / 255) << shift;
					}
					pixel = blended;
				}
			}
			penX += FontData::CELL_WIDTH;
			if(penX + FontData::CELL_WIDTH > WIDTH - 20)
			{
				penX = x;
				top += FontData::CELL_HEIGHT;
			}
		}
	}
}

void CpuScreen_ShowMessage(const std::string& title, const std::string& body)
{
	const uint32_t size = (WIDTH * HEIGHT * 4 + 0x3FFFF) & ~0x3FFFF;
	SceUID block = sceKernelAllocMemBlock("cpu_screen", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, size, nullptr);
	if(block < 0) return;
	void* base = nullptr;
	sceKernelGetMemBlockBase(block, &base);
	auto fb = static_cast<uint32_t*>(base);
	for(int i = 0; i < PITCH * HEIGHT; i++) fb[i] = 0xFF1C1210;
	DrawText(fb, 40, 50, 0xFFFFA05A, title);
	DrawText(fb, 40, 110, 0xFFFFFFFF, body);
	DrawText(fb, 40, 500, 0xFFA09696, "Press X to continue");

	SceDisplayFrameBuf frameBuf = {};
	frameBuf.size = sizeof(frameBuf);
	frameBuf.base = base;
	frameBuf.pitch = PITCH;
	frameBuf.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
	frameBuf.width = WIDTH;
	frameBuf.height = HEIGHT;
	sceDisplaySetFrameBuf(&frameBuf, SCE_DISPLAY_SETBUF_NEXTFRAME);

	uint32_t previous = ~0u;
	while(true)
	{
		SceCtrlData pad = {};
		sceCtrlPeekBufferPositive(0, &pad, 1);
		uint32_t pressed = pad.buttons & ~previous;
		previous = pad.buttons;
		if(pressed & (SCE_CTRL_CROSS | SCE_CTRL_CIRCLE)) break;
		sceDisplayWaitVblankStart();
	}
	// Keep the memory mapped while it may still be displayed; the next
	// GPU frame (if any) replaces it.
}
