#include "Gfx.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <vector>
#include "FontData.h"

namespace
{
	struct VERTEX
	{
		float x, y;
		float s, t;
		uint32_t color;
	};

	GLuint g_fontTexture = 0;
	GLuint g_whiteTexture = 0;
	GLuint g_frameTexture = 0;
	float g_screenWidth = 960, g_screenHeight = 544;

	std::vector<VERTEX> g_vertices;
	GLuint g_batchTexture = 0;
	bool g_batchBlend = true;
	bool g_batchLinear = false;

	void BindBatch(GLuint texture, bool blend, bool linear)
	{
		if((texture == g_batchTexture) && (blend == g_batchBlend) && (linear == g_batchLinear)) return;
		Gfx::Flush();
		g_batchTexture = texture;
		g_batchBlend = blend;
		g_batchLinear = linear;
	}

	void Quad(float x0, float y0, float x1, float y1, float s0, float t0, float s1, float t1, uint32_t color)
	{
		// Pixels to clip space, y down.
		float sx = 2.0f / g_screenWidth, sy = -2.0f / g_screenHeight;
		VERTEX tl = {x0 * sx - 1.0f, y0 * sy + 1.0f, s0, t0, color};
		VERTEX tr = {x1 * sx - 1.0f, y0 * sy + 1.0f, s1, t0, color};
		VERTEX bl = {x0 * sx - 1.0f, y1 * sy + 1.0f, s0, t1, color};
		VERTEX br = {x1 * sx - 1.0f, y1 * sy + 1.0f, s1, t1, color};
		g_vertices.push_back(tl);
		g_vertices.push_back(tr);
		g_vertices.push_back(bl);
		g_vertices.push_back(tr);
		g_vertices.push_back(br);
		g_vertices.push_back(bl);
	}

	GLuint CreateTexture(uint32_t width, uint32_t height, const void* pixels)
	{
		GLuint texture = 0;
		glGenTextures(1, &texture);
		glBindTexture(GL_TEXTURE_2D, texture);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
		return texture;
	}
}

bool Gfx::Init()
{
	std::vector<uint32_t> font(FontData::ATLAS_WIDTH * FontData::ATLAS_HEIGHT);
	for(size_t i = 0; i < font.size(); i++)
	{
		font[i] = 0x00FFFFFF | (static_cast<uint32_t>(FontData::ALPHA[i]) << 24);
	}
	g_fontTexture = CreateTexture(FontData::ATLAS_WIDTH, FontData::ATLAS_HEIGHT, font.data());
	uint32_t white = 0xFFFFFFFF;
	g_whiteTexture = CreateTexture(1, 1, &white);
	std::vector<uint32_t> black(FRAME_TEXTURE_SIZE * FRAME_TEXTURE_SIZE, 0xFF000000);
	g_frameTexture = CreateTexture(FRAME_TEXTURE_SIZE, FRAME_TEXTURE_SIZE, black.data());
	return glGetError() == GL_NO_ERROR;
}

void Gfx::Shutdown()
{
	GLuint textures[] = {g_fontTexture, g_whiteTexture, g_frameTexture};
	glDeleteTextures(3, textures);
	g_fontTexture = g_whiteTexture = g_frameTexture = 0;
}

void Gfx::BeginFrame(uint32_t screenWidth, uint32_t screenHeight, uint32_t clearColor)
{
	g_vertices.clear();
	g_screenWidth = static_cast<float>(screenWidth);
	g_screenHeight = static_cast<float>(screenHeight);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, screenWidth, screenHeight);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);
	glDisable(GL_ALPHA_TEST);
	glDisable(GL_CULL_FACE);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glBlendEquation(GL_FUNC_ADD);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glActiveTexture(GL_TEXTURE0);
	glEnable(GL_TEXTURE_2D);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, 1.0f);
	glTexEnvf(GL_TEXTURE_ENV, GL_ALPHA_SCALE, 1.0f);
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);

	glClearColor((clearColor & 0xFF) / 255.0f, ((clearColor >> 8) & 0xFF) / 255.0f, ((clearColor >> 16) & 0xFF) / 255.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	g_batchTexture = g_whiteTexture;
	g_batchBlend = true;
	g_batchLinear = false;
}

void Gfx::Flush()
{
	if(g_vertices.empty()) return;
	if(g_batchBlend) glEnable(GL_BLEND);
	else glDisable(GL_BLEND);
	glBindTexture(GL_TEXTURE_2D, g_batchTexture);
	GLint filter = g_batchLinear ? GL_LINEAR : GL_NEAREST;
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
	const GLsizei stride = sizeof(VERTEX);
	glVertexPointer(2, GL_FLOAT, stride, &g_vertices[0].x);
	glTexCoordPointer(2, GL_FLOAT, stride, &g_vertices[0].s);
	glColorPointer(4, GL_UNSIGNED_BYTE, stride, &g_vertices[0].color);
	glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(g_vertices.size()));
	g_vertices.clear();
}

void Gfx::Rect(float x, float y, float width, float height, uint32_t color)
{
	BindBatch(g_whiteTexture, true, false);
	Quad(x, y, x + width, y + height, 0, 0, 1, 1, color);
}

float Gfx::LineHeight(float scale)
{
	return FontData::CELL_HEIGHT * scale;
}

float Gfx::TextWidth(float scale, const char* text)
{
	size_t longest = 0, current = 0;
	for(const char* c = text; *c; c++)
	{
		if(*c == '\n') current = 0;
		else longest = std::max(longest, ++current);
	}
	return static_cast<float>(longest) * FontData::CELL_WIDTH * scale;
}

void Gfx::Text(float x, float y, uint32_t color, float scale, const char* text)
{
	BindBatch(g_fontTexture, true, true);
	const float cellW = FontData::CELL_WIDTH * scale;
	const float cellH = FontData::CELL_HEIGHT * scale;
	const float invW = 1.0f / FontData::ATLAS_WIDTH, invH = 1.0f / FontData::ATLAS_HEIGHT;
	float penX = x;
	float top = y - FontData::ASCENT * scale;
	for(const char* c = text; *c; c++)
	{
		int code = static_cast<unsigned char>(*c);
		if(code == '\n')
		{
			penX = x;
			top += cellH;
			continue;
		}
		if((code < FontData::FIRST_CHAR) || (code > FontData::LAST_CHAR)) code = '?';
		if(code != ' ')
		{
			int index = code - FontData::FIRST_CHAR;
			float s0 = (index % FontData::COLUMNS) * FontData::CELL_WIDTH * invW;
			float t0 = (index / FontData::COLUMNS) * FontData::CELL_HEIGHT * invH;
			float s1 = s0 + FontData::CELL_WIDTH * invW;
			float t1 = t0 + FontData::CELL_HEIGHT * invH;
			Quad(penX, top, penX + cellW, top + cellH, s0, t0, s1, t1, color);
		}
		penX += cellW;
	}
}

void Gfx::Textf(float x, float y, uint32_t color, float scale, const char* format, ...)
{
	char buffer[512];
	va_list args;
	va_start(args, format);
	std::vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	Text(x, y, color, scale, buffer);
}

void Gfx::Image(GLuint texture, uint32_t textureWidth, uint32_t textureHeight, float srcX, float srcY, float srcW,
                float srcH, float x, float y, float width, float height, bool linear)
{
	BindBatch(texture, false, linear);
	float invW = 1.0f / textureWidth, invH = 1.0f / textureHeight;
	Quad(x, y, x + width, y + height, srcX * invW, srcY * invH, (srcX + srcW) * invW, (srcY + srcH) * invH, 0xFFFFFFFF);
}

void Gfx::UploadFrame(const uint32_t* pixels, uint32_t width, uint32_t height)
{
	Flush();
	width = std::min(width, FRAME_TEXTURE_SIZE);
	height = std::min(height, FRAME_TEXTURE_SIZE);
	glBindTexture(GL_TEXTURE_2D, g_frameTexture);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
}

GLuint Gfx::FrameTexture()
{
	return g_frameTexture;
}
