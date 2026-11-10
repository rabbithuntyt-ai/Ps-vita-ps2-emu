#pragma once

// A batch of Resident Evil 4's world vertex program (see Re4Vu1.h), taken from VU1 memory
// when the program starts, drawn on the GS thread instead of running the program
// (CGSH_Primitives::DrawWorldBatch).

#include <cmath>
#include <cstdint>
#include <vector>
#include "Re4Vu1.h"

namespace Re4Vu1
{
	struct WORLD_BATCH
	{
		uint64_t tagLow = 0;      //GIF tag (NLOOP, EOP, PRE, PRIM, FLG, NREG)
		uint32_t count = 0;
		float scale = 1;           //position scale (the tag's top word)
		VEC4 projection[4] = {};   //VI5+0..3
		VEC4 clip[4] = {};         //VI5+4..7
		VEC4 colorOffset = {};     //VI5-2 (w: colour scale)
		VEC4 colorScale = {};      //VI5-1
		VEC4 vf31 = {};            //texture offset (xy), q scale (w)
		std::vector<int32_t> raw;  //per vertex P, T, C: 12 integers
	};

	// A vertex before the perspective divide (for clipping) and its decisions.
	struct WORLD_VERTEX
	{
		VEC4 p;        //scaled position, w = 1
		VEC4 t;        //texture numerators (x, y, z, w)
		VEC4 c;        //colour before the clamp
		VEC4 v;        //projection
		VEC4 k;        //clip space
		float winding; //t.w
		bool restart;
		uint32_t clipFlags;
	};

	inline VEC4 Transform(const VEC4* m, const VEC4& p)
	{
		return {m[0].x * p.x + m[1].x * p.y + m[2].x * p.z + m[3].x * p.w, m[0].y * p.x + m[1].y * p.y + m[2].y * p.z + m[3].y * p.w,
		        m[0].z * p.x + m[1].z * p.y + m[2].z * p.z + m[3].z * p.w, m[0].w * p.x + m[1].w * p.y + m[2].w * p.z + m[3].w * p.w};
	}

	inline uint32_t ClipFlags(const VEC4& k)
	{
		float w = std::fabs(k.w);
		uint32_t flags = 0;
		if(k.x > w) flags |= 1;
		if(k.x < -w) flags |= 2;
		if(k.y > w) flags |= 4;
		if(k.y < -w) flags |= 8;
		if(k.z > w) flags |= 16;
		if(k.z < -w) flags |= 32;
		return flags;
	}

	inline WORLD_VERTEX ReadWorldVertex(const WORLD_BATCH& batch, uint32_t index)
	{
		const int32_t* r = batch.raw.data() + index * 12;
		WORLD_VERTEX out;
		float pw = static_cast<float>(r[3]);
		out.p = {static_cast<float>(r[0]) * batch.scale, static_cast<float>(r[1]) * batch.scale, static_cast<float>(r[2]) * batch.scale, 1.0f};
		out.t = {static_cast<float>(r[4]) * (1.0f / 256.0f) + batch.vf31.x, static_cast<float>(r[5]) * (1.0f / 256.0f) + batch.vf31.y,
		         static_cast<float>(r[6]), pw * batch.vf31.w};
		out.c = {(static_cast<float>(r[8]) + batch.colorOffset.x) * batch.colorScale.x * batch.colorOffset.w,
		         (static_cast<float>(r[9]) + batch.colorOffset.y) * batch.colorScale.y * batch.colorOffset.w,
		         (static_cast<float>(r[10]) + batch.colorOffset.z) * batch.colorScale.z * batch.colorOffset.w, static_cast<float>(r[11])};
		out.v = Transform(batch.projection, out.p);
		out.k = Transform(batch.clip, out.p);
		out.winding = out.t.w;
		out.restart = (r[7] != 0);
		out.clipFlags = ClipFlags(out.k);
		return out;
	}

	// What the program sends to the GS for a vertex: ST (S, T, Q), RGBA, XYZ (12.4, Z).
	struct GS_VERTEX
	{
		float s, t, q;
		uint8_t rgba[4];
		int32_t x, y; //12.4, before the 16 bit wrap
		uint32_t z;
		float screenX, screenY;
	};

	inline GS_VERTEX Project(const WORLD_VERTEX& in)
	{
		GS_VERTEX out;
		float q = 1.0f / in.v.w;
		out.s = in.t.x * q;
		out.t = in.t.y * q;
		out.q = in.t.z * q;
		const float* c = &in.c.x;
		for(int i = 0; i < 4; i++) out.rgba[i] = static_cast<uint8_t>(Ftoi(std::fmin(c[i], 255.0f), 1.0f));
		out.screenX = in.v.x * q;
		out.screenY = in.v.y * q;
		out.x = Ftoi(out.screenX, 16.0f);
		out.y = Ftoi(out.screenY, 16.0f);
		out.z = static_cast<uint32_t>(Ftoi(in.v.z * q, 16.0f));
		return out;
	}

	inline WORLD_VERTEX Lerp(const WORLD_VERTEX& a, const WORLD_VERTEX& b, float f)
	{
		auto mix = [f](const VEC4& x, const VEC4& y) {
			return VEC4{x.x + (y.x - x.x) * f, x.y + (y.y - x.y) * f, x.z + (y.z - x.z) * f, x.w + (y.w - x.w) * f};
		};
		WORLD_VERTEX out = b;
		out.p = mix(a.p, b.p);
		out.t = mix(a.t, b.t);
		out.c = mix(a.c, b.c);
		out.v = mix(a.v, b.v);
		out.k = mix(a.k, b.k);
		return out;
	}

	// A triangle clipped against the clip volume (|x|, |y|, |z| <= w of the clip matrix),
	// as the program's clipping routine does: a convex polygon (up to 9 vertices).
	inline uint32_t ClipTriangle(const WORLD_VERTEX* in, WORLD_VERTEX* out)
	{
		WORLD_VERTEX buffers[2][12];
		uint32_t count = 3;
		for(uint32_t i = 0; i < 3; i++) buffers[0][i] = in[i];
		int current = 0;
		for(uint32_t plane = 0; plane < 6; plane++)
		{
			auto distance = [plane](const WORLD_VERTEX& v) {
				const float* k = &v.k.x;
				float value = k[plane >> 1];
				return (plane & 1) ? (v.k.w + value) : (v.k.w - value);
			};
			uint32_t outCount = 0;
			const auto* src = buffers[current];
			auto* dst = buffers[current ^ 1];
			for(uint32_t i = 0; i < count; i++)
			{
				const auto& a = src[i];
				const auto& b = src[(i + 1) % count];
				float da = distance(a), db = distance(b);
				if(da >= 0) dst[outCount++] = a;
				if((da >= 0) != (db >= 0))
				{
					float f = da / (da - db);
					dst[outCount++] = Lerp(a, b, f);
				}
				if(outCount >= 11) break;
			}
			count = outCount;
			current ^= 1;
			if(count < 3) return 0;
		}
		for(uint32_t i = 0; i < count; i++) out[i] = buffers[current][i];
		return count;
	}
}
