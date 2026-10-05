#pragma once

// Minimal immediate-mode 2D drawing on OpenGL (vitaGL on the Vita, Mesa on the
// host for tests): rectangles, text and textured quads in screen pixels,
// origin top-left. Used for menus, overlays and presenting the emulated frame.
//
// Uses only state that it resets in BeginFrame(), and leaves the matrix
// stacks at identity, so it can share the GL context with the GPU GS
// renderer.

#include <cstdint>
#include "GpuGl.h"

namespace Gfx
{
	// Same layout as vita2d's RGBA8: 0xAABBGGRR.
	constexpr uint32_t Rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a)
	{
		return r | (g << 8) | (b << 16) | (a << 24);
	}

	bool Init();
	void Shutdown();

	// Binds the default framebuffer, resets GL state and clears it.
	void BeginFrame(uint32_t screenWidth, uint32_t screenHeight, uint32_t clearColor);
	// Submits everything drawn since BeginFrame.
	void Flush();

	void Rect(float x, float y, float width, float height, uint32_t color);

	// 'y' is the text baseline (like vita2d_pgf_draw_text). '\n' starts a new line.
	void Text(float x, float y, uint32_t color, float scale, const char* text);
	void Textf(float x, float y, uint32_t color, float scale, const char* format, ...)
#if defined(__GNUC__)
	    __attribute__((format(printf, 5, 6)))
#endif
	    ;
	float TextWidth(float scale, const char* text);
	float LineHeight(float scale);

	// Draws a region of a texture (texel coordinates) into a screen rectangle.
	// Alpha is ignored (the emulated display is opaque).
	void Image(GLuint texture, uint32_t textureWidth, uint32_t textureHeight, float srcX, float srcY, float srcW,
	           float srcH, float x, float y, float width, float height, bool linear);

	// A 1024x1024 texture for frames rendered on the CPU.
	void UploadFrame(const uint32_t* pixels, uint32_t width, uint32_t height);
	GLuint FrameTexture();
	constexpr uint32_t FRAME_TEXTURE_SIZE = 1024;
}
