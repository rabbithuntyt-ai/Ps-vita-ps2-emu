#pragma once

// Resident Evil 4 (SLUS-21134): its VU1 vertex programs, described natively.
//
// The world program (continued at 0x1408: B 0x1040, loop 0x12B8-0x13E8, XGKICK and E at
// 0x13F0) transforms a batch of vertices from the VIF1 double buffer at TOP:
//   TOP+0x20  header: a GIF tag (NLOOP = vertex count in x & 0x7FFF; NREG 3) whose unused
//             top word holds the position scale as a float
//   TOP+0x21+3i  P: position (integers xyz, w: winding sign for the strip)
//   TOP+0x22+3i  T: texture coordinates (integers xy in 1/256, w: strip restart flag)
//   TOP+0x23+3i  C: colour (integers)
// with constants at VI5: -2 colour offset (xyz) and scale (w), -1 colour scale (xyz),
// 0..3 the projection matrix, 4..7 the clip matrix; VF31: texture offset (xy) and q scale (w).
// It writes at TOP+0xB4: the tag (top word 0), then per vertex ST, RGBA, XYZ (12.4 fixed,
// ADC bit 15 of w for culled or restarting triangles). Triangles near the clip planes go
// through a clipping routine (0x14D8...), which this description leaves out.
//
// Used by the PC runner's --vu1-verify to check the description against the JIT, and as
// the base for running the program on the GPU.

#include <cmath>
#include <cstdint>
#include <cstring>

namespace Re4Vu1
{
	struct VEC4
	{
		float x, y, z, w;
	};

	struct WORLD_VERTEX_OUT
	{
		float st[4];
		uint32_t rgba[4];
		int32_t xyz[4];
		float screenX, screenY; //before the conversion to 12.4
		float windingSign;      //t.w (P.w * VF31.w): multiplies the back face test
		int32_t restart;        //T.w as loaded
	};

	inline float AsFloat(uint32_t value)
	{
		float result;
		std::memcpy(&result, &value, 4);
		return result;
	}

	inline VEC4 LoadFloat(const uint8_t* vuMem, uint32_t qword)
	{
		VEC4 v;
		std::memcpy(&v, vuMem + ((qword * 16) & 0x3FFF), 16);
		return v;
	}

	inline VEC4 LoadIntAsFloat(const uint8_t* vuMem, uint32_t qword)
	{
		int32_t i[4];
		std::memcpy(i, vuMem + ((qword * 16) & 0x3FFF), 16);
		return {static_cast<float>(i[0]), static_cast<float>(i[1]), static_cast<float>(i[2]), static_cast<float>(i[3])};
	}

	inline int32_t Ftoi(float value, float scale)
	{
		float scaled = value * scale;
		if(!(scaled == scaled)) return 0;
		if(scaled >= 2147483520.0f) return 0x7FFFFFFF;
		if(scaled <= -2147483648.0f) return static_cast<int32_t>(0x80000000);
		return static_cast<int32_t>(scaled);
	}

	// Clip flags of a vertex (CLIP of the clip matrix's result against its w): 6 bits.
	inline uint32_t WorldClipFlags(const uint8_t* vuMem, uint32_t top, uint32_t vi5, uint32_t index)
	{
		VEC4 header = LoadFloat(vuMem, top + 0x20);
		VEC4 p = LoadIntAsFloat(vuMem, top + 0x21 + 3 * index);
		p.x *= header.w;
		p.y *= header.w;
		p.z *= header.w;
		VEC4 m[4];
		for(int r = 0; r < 4; r++) m[r] = LoadFloat(vuMem, vi5 + 4 + r);
		float x = m[0].x * p.x + m[1].x * p.y + m[2].x * p.z + m[3].x;
		float y = m[0].y * p.x + m[1].y * p.y + m[2].y * p.z + m[3].y;
		float z = m[0].z * p.x + m[1].z * p.y + m[2].z * p.z + m[3].z;
		float w = std::fabs(m[0].w * p.x + m[1].w * p.y + m[2].w * p.z + m[3].w);
		uint32_t flags = 0;
		if(x > w) flags |= 1;
		if(x < -w) flags |= 2;
		if(y > w) flags |= 4;
		if(y < -w) flags |= 8;
		if(z > w) flags |= 16;
		if(z < -w) flags |= 32;
		return flags;
	}

	// One vertex of the world program, without the clipping/culling decisions.
	inline WORLD_VERTEX_OUT TransformWorldVertex(const uint8_t* vuMem, uint32_t top, uint32_t vi5, const VEC4& vf31, uint32_t index)
	{
		VEC4 header = LoadFloat(vuMem, top + 0x20);
		float scale = header.w;
		VEC4 colorOffset = LoadFloat(vuMem, vi5 - 2), colorScale = LoadFloat(vuMem, vi5 - 1);
		VEC4 m[4];
		for(int r = 0; r < 4; r++) m[r] = LoadFloat(vuMem, vi5 + r);
		VEC4 p = LoadIntAsFloat(vuMem, top + 0x21 + 3 * index);
		VEC4 t = LoadIntAsFloat(vuMem, top + 0x22 + 3 * index);
		VEC4 c = LoadIntAsFloat(vuMem, top + 0x23 + 3 * index);
		p.x *= scale;
		p.y *= scale;
		p.z *= scale;
		t.x = t.x * (1.0f / 256.0f) + vf31.x;
		t.y = t.y * (1.0f / 256.0f) + vf31.y;
		t.w = p.w * vf31.w;
		c.x = (c.x + colorOffset.x) * colorScale.x * colorOffset.w;
		c.y = (c.y + colorOffset.y) * colorScale.y * colorOffset.w;
		c.z = (c.z + colorOffset.z) * colorScale.z * colorOffset.w;
		VEC4 v;
		v.x = m[0].x * p.x + m[1].x * p.y + m[2].x * p.z + m[3].x;
		v.y = m[0].y * p.x + m[1].y * p.y + m[2].y * p.z + m[3].y;
		v.z = m[0].z * p.x + m[1].z * p.y + m[2].z * p.z + m[3].z;
		v.w = m[0].w * p.x + m[1].w * p.y + m[2].w * p.z + m[3].w;
		float q = 1.0f / v.w;
		WORLD_VERTEX_OUT out;
		out.st[0] = t.x * q;
		out.st[1] = t.y * q;
		out.st[2] = t.z * q;
		out.st[3] = t.w * q;
		float cc[4] = {std::fmin(c.x, 255.0f), std::fmin(c.y, 255.0f), std::fmin(c.z, 255.0f), std::fmin(c.w, 255.0f)};
		for(int k = 0; k < 4; k++) out.rgba[k] = static_cast<uint32_t>(Ftoi(cc[k], 1.0f));
		out.xyz[0] = Ftoi(v.x * q, 16.0f);
		out.xyz[1] = Ftoi(v.y * q, 16.0f);
		out.xyz[2] = Ftoi(v.z * q, 16.0f);
		out.xyz[3] = Ftoi(v.w * q, 16.0f);
		out.screenX = v.x * q;
		out.screenY = v.y * q;
		out.windingSign = t.w;
		int32_t raw[4];
		std::memcpy(raw, vuMem + (((top + 0x22 + 3 * index) * 16) & 0x3FFF), 16);
		out.restart = raw[3];
		return out;
	}
}
