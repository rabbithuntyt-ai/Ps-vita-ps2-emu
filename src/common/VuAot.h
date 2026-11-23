#pragma once

// VU1 programs translated to C++ ahead of time (tools/vu_aot.py): the registers they work
// on and the instruction helpers, matching Play's VU JIT (Source/ee/VUShared.cpp).
#include <cstdint>
#include <cstring>

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-label"
#endif

union VU_AOT_VEC
{
	float f[4];
	uint32_t u[4];
	int32_t i[4];
};

union VU_AOT_SCALAR
{
	float f;
	uint32_t u;
};

struct VU_AOT_REGS
{
	VU_AOT_VEC vf[32];
	VU_AOT_VEC acc;
	uint32_t vi[16];
	VU_AOT_SCALAR q, i;
	uint32_t clip; // newest CLIP result (previous ones shifted up by 6 bits)
	uint32_t top;  // XTOP
};

// XGKICK: the program's VI value (Play's CVpu::ProcessXgKick takes it as is)
typedef void (*VU_AOT_KICK)(void* ctx, uint32_t address);

// MAX/MINI compare floats as integers (VUShared::MakeComparableFromFloat): +0 and -0 equal,
// on a tie the second operand is taken.
inline int32_t VuComparable(float value)
{
	uint32_t bits;
	std::memcpy(&bits, &value, 4);
	return static_cast<int32_t>((bits & 0x80000000u) ? (0x7FFFFFFFu - bits) : (bits - 1u));
}

inline float VuMax(float fs, float ft)
{
	return (VuComparable(fs) > VuComparable(ft)) ? fs : ft;
}

inline float VuMini(float fs, float ft)
{
	return (VuComparable(ft) > VuComparable(fs)) ? fs : ft;
}

// FTOI: truncation; out of range as the host's vector conversion gives it (the JIT uses it):
// ARM saturates (NaN 0), x86 returns 0x80000000.
inline int32_t VuFtoi(float value)
{
#if defined(__arm__) || defined(__aarch64__)
	if(!(value == value)) return 0;
	if(value >= 2147483648.0f) return 0x7FFFFFFF;
	if(value <= -2147483648.0f) return static_cast<int32_t>(0x80000000u);
	return static_cast<int32_t>(value);
#else
	if(!(value == value) || (value >= 2147483648.0f) || (value < -2147483648.0f)) return static_cast<int32_t>(0x80000000u);
	return static_cast<int32_t>(value);
#endif
}

// DIV: a divisor with a zero exponent (zero or denormal) gives +/-FLT_MAX by the signs
inline VU_AOT_SCALAR VuDiv(uint32_t fs, uint32_t ft)
{
	VU_AOT_SCALAR result;
	if((ft & 0x7F800000u) == 0)
	{
		result.u = 0x7F7FFFFFu | ((fs ^ ft) & 0x80000000u);
		return result;
	}
	float a, b;
	std::memcpy(&a, &fs, 4);
	std::memcpy(&b, &ft, 4);
	result.f = a / b;
	return result;
}

// CLIP xyz against |w|: bit 0 x > |w|, 1 x < -|w|, 2/3 y, 4/5 z. With a NaN the JIT's compare
// decides: on x86 "greater" is CMPNLEPS (true for NaN), on ARM VCGT (false).
inline bool VuClipGreater(float value, float bound)
{
#if defined(__arm__) || defined(__aarch64__)
	return value > bound;
#else
	return !(value <= bound);
#endif
}

inline uint32_t VuClip(const VU_AOT_VEC& v, float w)
{
	uint32_t bits;
	std::memcpy(&bits, &w, 4);
	bits &= 0x7FFFFFFFu;
	float bound, negBound;
	std::memcpy(&bound, &bits, 4);
	bits |= 0x80000000u;
	std::memcpy(&negBound, &bits, 4);
	uint32_t flags = 0;
	if(VuClipGreater(v.f[0], bound)) flags |= 1;
	if(v.f[0] < negBound) flags |= 2;
	if(VuClipGreater(v.f[1], bound)) flags |= 4;
	if(v.f[1] < negBound) flags |= 8;
	if(VuClipGreater(v.f[2], bound)) flags |= 16;
	if(v.f[2] < negBound) flags |= 32;
	return flags;
}
