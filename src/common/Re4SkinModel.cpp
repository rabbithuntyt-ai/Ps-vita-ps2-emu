// RE4's skinning program (VU1 0x0000-0x07D0) as plain float math, the way a GPU vertex shader
// would compute it, checked against the bit-exact native translation (Re4Skin.gen.cpp) on the PC
// runner (--skin-model). It decides whether a shader version can match the game's output.
//
// Read from the microcode (entries 0x0000/0x0050, batch loop 0x01D0-0x0480; the variant at
// 0x04A8 when the flag at VI5-3 is set swaps the texture coordinates and takes Q's factor from
// the header):
//   header TOP+0x20: GIF tag (xyz), NLOOP in x & 0x7FFF, w = position scale (float)
//   vertex k, 3 qwords from TOP+0x21: position (ints xyz, w = bone list offset from TOP),
//     normal (ints xyz, w = strip restart flag), texture coordinates (ints)
//   bone list: indices (qword offsets from VI5) in xyz, count in w; weights (floats) xyzw
//   VU1 memory 0-3: projection matrix columns, 4-7: clip matrix columns
//   VI3: light direction, light colour; VI5-2: ambient (w forced to 128); VI5-1: material
//   output TOP+0xB4: tag, then per vertex STQ, RGBA, XYZ2 (w: 0x8000 to skip the triangle)
#include "Re4Skin.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include "VuAot.h"

namespace
{
	struct V4
	{
		float x = 0, y = 0, z = 0, w = 0;
	};

	V4 LoadF(const uint8_t* mem, uint32_t qword)
	{
		V4 v;
		std::memcpy(&v, mem + (qword & 0x3FF) * 16, 16);
		return v;
	}

	void LoadI(const uint8_t* mem, uint32_t qword, int32_t out[4])
	{
		std::memcpy(out, mem + (qword & 0x3FF) * 16, 16);
	}

	V4 Combine(const V4 c[4], const V4& v)
	{
		// VU convention: columns times components, then the fourth column times w
		V4 r;
		r.x = c[0].x * v.x + c[1].x * v.y + c[2].x * v.z + c[3].x * v.w;
		r.y = c[0].y * v.x + c[1].y * v.y + c[2].y * v.z + c[3].y * v.w;
		r.z = c[0].z * v.x + c[1].z * v.y + c[2].z * v.z + c[3].z * v.w;
		r.w = c[0].w * v.x + c[1].w * v.y + c[2].w * v.z + c[3].w * v.w;
		return r;
	}

	uint32_t ClipFlags(const V4& v)
	{
		float w = std::fabs(v.w);
		uint32_t f = 0;
		if(v.x > w) f |= 0x01;
		if(v.x < -w) f |= 0x02;
		if(v.y > w) f |= 0x04;
		if(v.y < -w) f |= 0x08;
		if(v.z > w) f |= 0x10;
		if(v.z < -w) f |= 0x20;
		return f;
	}

	int32_t Ftoi(float value, float scale)
	{
		// FTOI: truncation toward zero, saturated
		double scaled = static_cast<double>(value) * scale;
		if(scaled >= 2147483647.0) return 0x7FFFFFFF;
		if(scaled <= -2147483648.0) return static_cast<int32_t>(0x80000000);
		return static_cast<int32_t>(scaled);
	}

	struct MODEL_STATS
	{
		uint64_t batches = 0, vertices = 0, skipped = 0;
		uint64_t xyExact = 0, xyWithin1 = 0, xyWorse = 0, zExact = 0, zWorse = 0;
		uint64_t rgbaExact = 0, rgbaWithin1 = 0, rgbaWorse = 0;
		uint64_t stExact = 0, stClose = 0, stWorse = 0;
		uint64_t adcMatch = 0, adcDiffer = 0;
		std::string examples;
		int exampleCount = 0;
	};
	MODEL_STATS g_model;

	void Example(const char* text)
	{
		if(g_model.exampleCount >= 20) return;
		g_model.exampleCount++;
		g_model.examples += text;
	}
}

void Re4Skin::CheckModel(const uint8_t* before, const uint8_t* after, const VU_AOT_REGS& r, uint32_t entry)
{
	// Entries 0x0000/0x0050 set everything up from memory; 0x0498/0x07C8 go on with the
	// previous run's registers
	bool setup = (entry == 0x0000) || (entry == 0x0050);
	uint32_t vi5 = (entry == 0x0000) ? 0x29B : (entry == 0x0050) ? 0x2E6 : (r.vi[5] & 0x3FF);
	int32_t setupWord[4];
	LoadI(before, vi5 - 3, setupWord);
	uint32_t vi3 = setup ? (static_cast<uint32_t>(setupWord[1]) & 0xFFFF) : (r.vi[3] & 0x3FF);
	bool swapped = setup ? (setupWord[0] != 0) : (entry == 0x07C8);
	uint32_t top = r.top;
	uint32_t clipIn = r.clip;
	auto reg = [&r](int i) {
		V4 v;
		std::memcpy(&v, r.vf[i].u, 16);
		return v;
	};
	auto& m = g_model;
	m.batches++;
	int32_t header[4];
	LoadI(before, top + 0x20, header);
	uint32_t count = static_cast<uint32_t>(header[0]) & 0x7FFF;
	V4 headerF = LoadF(before, top + 0x20);
	float scale = headerF.w;
	V4 proj[4], clip[4], lightDir, lightColor;
	if(setup)
	{
		for(int i = 0; i < 4; i++)
		{
			proj[i] = LoadF(before, i);
			clip[i] = LoadF(before, 4 + i);
		}
		lightDir = LoadF(before, vi3);
		lightColor = LoadF(before, vi3 + 1);
	}
	else
	{
		proj[0] = reg(30), proj[1] = reg(4), proj[2] = reg(15), proj[3] = reg(17);
		clip[0] = reg(1), clip[1] = reg(2), clip[2] = reg(5), clip[3] = reg(6);
		lightDir = reg(7), lightColor = reg(13);
	}
	V4 ambient = LoadF(before, vi5 - 2);
	ambient.w = 128.0f;
	V4 material = LoadF(before, vi5 - 1);
	uint32_t clipHistory = clipIn;
	for(uint32_t k = 0; k < count; k++)
	{
		int32_t in0[4], in1[4], in2[4];
		LoadI(before, top + 0x21 + 3 * k, in0);
		LoadI(before, top + 0x22 + 3 * k, in1);
		LoadI(before, top + 0x23 + 3 * k, in2);
		// Bone blend
		uint32_t record = top + static_cast<uint32_t>(in0[3]);
		int32_t indices[4];
		LoadI(before, record, indices);
		V4 weightsV = LoadF(before, record + 1);
		float weights[4] = {weightsV.x, weightsV.y, weightsV.z, weightsV.w};
		uint32_t bones = static_cast<uint32_t>(indices[3]) & 0xFFFF;
		V4 mat[4];
		for(uint32_t b = 0; b < bones && b < 4; b++)
		{
			uint32_t base = vi5 + (static_cast<uint32_t>(indices[b]) & 0xFFFF);
			for(int c = 0; c < 4; c++)
			{
				V4 col = LoadF(before, base + c);
				mat[c].x += col.x * weights[b];
				mat[c].y += col.y * weights[b];
				mat[c].z += col.z * weights[b];
				mat[c].w += col.w * weights[b];
			}
		}
		V4 pos{static_cast<float>(in0[0]) * scale, static_cast<float>(in0[1]) * scale, static_cast<float>(in0[2]) * scale, 1.0f};
		V4 world = Combine(mat, pos);
		float normalScale;
		const uint32_t normalScaleBits = 0x3C0101FE; //the program's I constant (1/127)
		std::memcpy(&normalScale, &normalScaleBits, 4);
		V4 nrm{static_cast<float>(in1[0]) * normalScale, static_cast<float>(in1[1]) * normalScale, static_cast<float>(in1[2]) * normalScale, 0.0f};
		V4 n = Combine(mat, nrm);
		// Lighting
		float d = (lightDir.x * n.x + lightDir.y * n.y) + lightDir.z * n.z;
		if(!(d > 0.0f)) d = 0.0f;
		V4 color{(lightColor.x * d + ambient.x) * material.x, (lightColor.y * d + ambient.y) * material.y,
		         (lightColor.z * d + ambient.z) * material.z, ambient.w * material.w};
		int32_t rgba[4] = {Ftoi(std::fmin(color.x, 255.0f), 1), Ftoi(std::fmin(color.y, 255.0f), 1), Ftoi(std::fmin(color.z, 255.0f), 1), Ftoi(std::fmin(color.w, 255.0f), 1)};
		// Projection
		V4 screen = Combine(proj, world);
		float q = 1.0f / screen.w;
		int32_t xyz[3] = {Ftoi(screen.x * q, 16), Ftoi(screen.y * q, 16), Ftoi(screen.z * q, 16)};
		// Texture coordinates
		float s = static_cast<float>(in2[0]) / 256.0f, t = static_cast<float>(in2[1]) / 256.0f, z = static_cast<float>(in2[2]);
		if(swapped)
		{
			std::swap(s, t);
			z = 1.0f;
		}
		float st[3] = {s * q, t * q, z * q};
		// Skip bit: this vertex or one of the two before outside the clip box, or a restart
		V4 c = Combine(clip, world);
		clipHistory = ((clipHistory << 6) | ClipFlags(c)) & 0xFFFFFF;
		bool skip = ((clipHistory & 0x3FFFF) != 0) || (in1[3] != 0);
		// Native output
		int32_t outSt[4], outRgba[4], outXyz[4];
		LoadI(after, top + 0xB5 + 3 * k, outSt);
		LoadI(after, top + 0xB6 + 3 * k, outRgba);
		LoadI(after, top + 0xB7 + 3 * k, outXyz);
		float outStF[4];
		std::memcpy(outStF, outSt, 16);
		m.vertices++;
		bool nativeSkip = (outXyz[3] & 0x8000) != 0;
		if(nativeSkip == skip) m.adcMatch++;
		else m.adcDiffer++;
		if(nativeSkip)
		{
			m.skipped++;
			continue; //not drawn: its other values do not matter
		}
		int dx = std::abs(outXyz[0] - xyz[0]), dy = std::abs(outXyz[1] - xyz[1]), dz = std::abs(outXyz[2] - xyz[2]);
		if((dx == 0) && (dy == 0)) m.xyExact++;
		else if((dx <= 1) && (dy <= 1)) m.xyWithin1++;
		else
		{
			m.xyWorse++;
			char text[200];
			std::snprintf(text, sizeof(text), "top %03X v%u: xy native %d,%d model %d,%d\n", top, k, outXyz[0], outXyz[1], xyz[0], xyz[1]);
			Example(text);
		}
		if(dz == 0) m.zExact++;
		else m.zWorse++;
		int dc = 0;
		for(int i = 0; i < 4; i++) dc = std::max(dc, std::abs(outRgba[i] - rgba[i]));
		if(dc == 0) m.rgbaExact++;
		else if(dc <= 1) m.rgbaWithin1++;
		else
		{
			m.rgbaWorse++;
			char text[200];
			std::snprintf(text, sizeof(text), "top %03X v%u: rgba native %d,%d,%d,%d model %d,%d,%d,%d\n", top, k, outRgba[0], outRgba[1], outRgba[2], outRgba[3], rgba[0], rgba[1], rgba[2], rgba[3]);
			Example(text);
		}
		float ds = 0;
		for(int i = 0; i < 3; i++)
		{
			float scaleRef = std::fmax(std::fabs(outStF[i]), 1e-6f);
			ds = std::fmax(ds, std::fabs(outStF[i] - st[i]) / scaleRef);
		}
		if(ds == 0) m.stExact++;
		else if(ds < 1e-4f) m.stClose++;
		else
		{
			m.stWorse++;
			char text[200];
			std::snprintf(text, sizeof(text), "top %03X v%u: stq native %g,%g,%g model %g,%g,%g\n", top, k, outStF[0], outStF[1], outStF[2], st[0], st[1], st[2]);
			Example(text);
		}
	}
}

void Re4Skin::PrintModelStats()
{
	const auto& m = g_model;
	std::printf("skin model: %llu batches, %llu vertices (%llu skipped); skip bit %llu match %llu differ\n", (unsigned long long)m.batches, (unsigned long long)m.vertices,
	            (unsigned long long)m.skipped, (unsigned long long)m.adcMatch, (unsigned long long)m.adcDiffer);
	std::printf("skin model, drawn vertices: xy exact %llu, within 1/16 px %llu, worse %llu; z exact %llu, other %llu; rgba exact %llu, within 1 %llu, worse %llu; stq exact %llu, close %llu, worse %llu\n",
	            (unsigned long long)m.xyExact, (unsigned long long)m.xyWithin1, (unsigned long long)m.xyWorse, (unsigned long long)m.zExact, (unsigned long long)m.zWorse,
	            (unsigned long long)m.rgbaExact, (unsigned long long)m.rgbaWithin1, (unsigned long long)m.rgbaWorse, (unsigned long long)m.stExact, (unsigned long long)m.stClose, (unsigned long long)m.stWorse);
	if(!m.examples.empty()) std::printf("%s", m.examples.c_str());
}
