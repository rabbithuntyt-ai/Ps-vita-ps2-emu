// Native overlays of hot EE functions: see EeOverlays.h.
//
// Each overlay follows its function's code instruction by instruction where that
// decides a value the game can see (registers included). The float arithmetic is
// the recompiler's: the FPU's operands are clamped (FP_ClampS) and computed in
// scalar floats; VU0 macro instructions are computed four lanes at a time with the
// host's vector unit (NEON on the Vita, SSE on the PC), as the recompiler does,
// with the same clamps. This file is built without fast-math or contraction
// (CMakeLists.txt) so the compiler keeps every rounding where it is.

#include "EeOverlays.h"
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstring>
#include <map>
#include "PS2VM.h"
#include "Ps2Const.h"
#include "MIPS.h"
#include "COP_SCU.h"
#include "ee/Ee_SubSystem.h"

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define OVERLAY_NEON 1
#else
#include <emmintrin.h>
#endif

namespace
{
	// ------------------------------------------------------------------
	// Registers and memory
	// ------------------------------------------------------------------

	inline uint64 Sx(uint32 value)
	{
		return static_cast<uint64>(static_cast<int64>(static_cast<int32>(value)));
	}

	struct CALL
	{
		CMIPS& ee;
		uint8* ram;
		uint8* spr;
		const uint32* params;

		uint128& Gpr(unsigned int r)
		{
			return ee.m_State.nGPR[r];
		}
		uint64 Lo(unsigned int r)
		{
			return ee.m_State.nGPR[r].nD0;
		}
		// 64-bit instructions write the low half only
		void SetLo(unsigned int r, uint64 value)
		{
			ee.m_State.nGPR[r].nD0 = value;
		}
		uint32& Fpr(unsigned int r)
		{
			return ee.m_State.nCOP1[r];
		}
		uint128& Vf(unsigned int r)
		{
			return ee.m_State.nCOP2[r];
		}

		// Host address of [address, address + size) in RAM or the scratchpad, or null
		uint8* Host(uint32 address, uint32 size)
		{
			uint32 physical = ee.m_pAddrTranslator(&ee, address);
			if((physical < PS2::EE_RAM_SIZE) && (size <= PS2::EE_RAM_SIZE - physical)) return ram + physical;
			if((physical >= PS2::EE_SPR_ADDR) && (physical - PS2::EE_SPR_ADDR + size <= PS2::EE_SPR_SIZE)) return spr + (physical - PS2::EE_SPR_ADDR);
			return nullptr;
		}
		uint32 Read32(uint32 address)
		{
			uint32 value = 0;
			if(auto host = Host(address, 4)) std::memcpy(&value, host, 4);
			else value = ee.m_pMemoryMap->GetWord(ee.m_pAddrTranslator(&ee, address));
			return value;
		}
		void Write32(uint32 address, uint32 value)
		{
			if(auto host = Host(address, 4)) std::memcpy(host, &value, 4);
			else ee.m_pMemoryMap->SetWord(ee.m_pAddrTranslator(&ee, address), value);
		}
		void Write64(uint32 address, uint64 value)
		{
			Write32(address, static_cast<uint32>(value));
			Write32(address + 4, static_cast<uint32>(value >> 32));
		}
		uint64 Read64(uint32 address)
		{
			return Read32(address) | (static_cast<uint64>(Read32(address + 4)) << 32);
		}
		// lq/lqc2/sq/sqc2: the low 4 bits of the address are ignored
		uint128 Read128(uint32 address)
		{
			address &= ~0xFU;
			uint128 value;
			if(auto host = Host(address, 16)) std::memcpy(&value, host, 16);
			else
				for(unsigned int i = 0; i < 4; i++) value.nV[i] = Read32(address + i * 4);
			return value;
		}
		void Write128(uint32 address, const uint128& value)
		{
			address &= ~0xFU;
			if(auto host = Host(address, 16)) std::memcpy(host, &value, 16);
			else
				for(unsigned int i = 0; i < 4; i++) Write32(address + i * 4, value.nV[i]);
		}
		void Write8(uint32 address, uint8 value)
		{
			if(auto host = Host(address, 1)) *host = value;
			else ee.m_pMemoryMap->SetByte(ee.m_pAddrTranslator(&ee, address), value);
		}
	};

	// ------------------------------------------------------------------
	// FPU (COP1) as compiled by CCOP_FPU
	// ------------------------------------------------------------------

	inline float F(uint32 bits)
	{
		float value;
		std::memcpy(&value, &bits, 4);
		return value;
	}
	inline uint32 B(float value)
	{
		uint32 bits;
		std::memcpy(&bits, &value, 4);
		return bits;
	}
	// FP_ClampS / MD_ClampS: signed min with +FLT_MAX, unsigned min with -FLT_MAX
	inline uint32 Clamp(uint32 v)
	{
		if(static_cast<int32>(v) > 0x7F7FFFFF) v = 0x7F7FFFFF;
		if(v > 0xFF7FFFFFU) v = 0xFF7FFFFFU;
		return v;
	}
	inline uint32 FAdd(uint32 a, uint32 b)
	{
		return B(F(Clamp(a)) + F(Clamp(b)));
	}
	inline uint32 FSub(uint32 a, uint32 b)
	{
		return B(F(Clamp(a)) - F(Clamp(b)));
	}
	inline uint32 FMul(uint32 a, uint32 b)
	{
		return B(F(Clamp(a)) * F(Clamp(b)));
	}
	inline uint32 FDiv(uint32 a, uint32 b)
	{
		if((b & 0x7F800000) == 0) return 0x7F7FFFFF | ((a ^ b) & 0x80000000); //FpUtils::ComputeDivisionByZero
		return B(F(Clamp(a)) / F(Clamp(b)));
	}
	inline bool FLt(uint32 a, uint32 b)
	{
		return F(Clamp(a)) < F(Clamp(b));
	}
	inline uint32 FAbs(uint32 a)
	{
		return a & 0x7FFFFFFF;
	}
	inline uint32 FNeg(uint32 a)
	{
		return a ^ 0x80000000;
	}
	// cvt.w.s (FP_ToInt32TruncateS): the host's truncating conversion
	inline uint32 FToW(uint32 a)
	{
		return static_cast<uint32>(static_cast<int32>(F(a)));
	}
	inline uint32 WToF(uint32 a)
	{
		return B(static_cast<float>(static_cast<int32>(a)));
	}
	// c.cond.s: condition bit 0 of FCSR
	inline void SetCc(CALL& c, bool condition)
	{
		if(condition) c.ee.m_State.nFCSR |= 0x00800000;
		else c.ee.m_State.nFCSR &= ~0x00800000;
	}

	// ------------------------------------------------------------------
	// VU0 macro instructions as compiled by VUShared (clamping on, no flags)
	// ------------------------------------------------------------------

	struct V4
	{
		uint32 v[4];
	};
	inline V4 ToV4(const uint128& r)
	{
		V4 out;
		std::memcpy(out.v, r.nV, 16);
		return out;
	}
	inline V4 Splat(uint32 value)
	{
		return V4{{value, value, value, value}};
	}
	inline V4 VClamp(V4 a)
	{
		for(auto& lane : a.v) lane = Clamp(lane);
		return a;
	}
#ifdef OVERLAY_NEON
	inline V4 VMul(const V4& a, const V4& b)
	{
		V4 out;
		vst1q_f32(reinterpret_cast<float*>(out.v), vmulq_f32(vld1q_f32(reinterpret_cast<const float*>(a.v)), vld1q_f32(reinterpret_cast<const float*>(b.v))));
		return out;
	}
	inline V4 VAdd(const V4& a, const V4& b)
	{
		V4 out;
		vst1q_f32(reinterpret_cast<float*>(out.v), vaddq_f32(vld1q_f32(reinterpret_cast<const float*>(a.v)), vld1q_f32(reinterpret_cast<const float*>(b.v))));
		return out;
	}
#else
	inline V4 VMul(const V4& a, const V4& b)
	{
		V4 out;
		_mm_storeu_ps(reinterpret_cast<float*>(out.v), _mm_mul_ps(_mm_loadu_ps(reinterpret_cast<const float*>(a.v)), _mm_loadu_ps(reinterpret_cast<const float*>(b.v))));
		return out;
	}
	inline V4 VAdd(const V4& a, const V4& b)
	{
		V4 out;
		_mm_storeu_ps(reinterpret_cast<float*>(out.v), _mm_add_ps(_mm_loadu_ps(reinterpret_cast<const float*>(a.v)), _mm_loadu_ps(reinterpret_cast<const float*>(b.v))));
		return out;
	}
#endif
	// Lanes of 'dest' (x = 8, y = 4, z = 2, w = 1, as in the opcode)
	inline void Store(uint128& reg, const V4& value, unsigned int dest)
	{
		for(unsigned int i = 0; i < 4; i++)
			if(dest & (8 >> i)) reg.nV[i] = value.v[i];
	}
	enum : unsigned int
	{
		X = 8,
		Y = 4,
		Z = 2,
		W = 1,
		XYZW = 15,
	};
	// VMUL[bc] fd, fs, ft: clamp(fs) * clamp(ft)
	inline void VuMul(CALL& c, unsigned int dest, unsigned int fd, unsigned int fs, const V4& ft)
	{
		Store(c.Vf(fd), VMul(VClamp(ToV4(c.Vf(fs))), VClamp(ft)), dest);
	}
	// VADD[bc] fd, fs, ft: clamp(fs) + ft
	inline void VuAdd(CALL& c, unsigned int dest, unsigned int fd, unsigned int fs, const V4& ft)
	{
		Store(c.Vf(fd), VAdd(VClamp(ToV4(c.Vf(fs))), ft), dest);
	}
	// VMULAbc ACC, fs, ft.bc: clamp(fs) * bc (the broadcast is not clamped)
	inline void VuMulaBc(CALL& c, unsigned int dest, unsigned int fs, uint32 bc)
	{
		Store(c.ee.m_State.nCOP2A, VMul(VClamp(ToV4(c.Vf(fs))), Splat(bc)), dest);
	}
	// VMADDA[bc] ACC / VMADD[bc] fd: ACC + clamp(fs) * clamp(ft)
	inline V4 VuMaddValue(CALL& c, unsigned int fs, const V4& ft)
	{
		return VAdd(ToV4(c.ee.m_State.nCOP2A), VMul(VClamp(ToV4(c.Vf(fs))), VClamp(ft)));
	}
	// VF0's broadcast elements are constants in the recompiler (PushBcElement)
	inline uint32 Vf0Element(unsigned int bc)
	{
		return (bc == 3) ? 0x3F800000 : 0;
	}

	// ------------------------------------------------------------------
	// Overlays. Each returns the EE cycles its function takes (about one per
	// instruction run), or 0 to let the original run.
	// ------------------------------------------------------------------

	enum
	{
		A0 = CMIPS::A0,
		A1 = CMIPS::A1,
		A2 = CMIPS::A2,
		A3 = CMIPS::A3,
		V0 = CMIPS::V0,
		V1 = CMIPS::V1,
		T0 = CMIPS::T0,
		T1 = CMIPS::T1,
		T2 = CMIPS::T2,
		T3 = CMIPS::T3,
		T4 = CMIPS::T4,
		T5 = CMIPS::T5,
		T6 = CMIPS::T6,
		T7 = CMIPS::T7,
		AT = CMIPS::AT,
		SP = CMIPS::SP,
		RA = CMIPS::RA,
	};

	// PSMTXCopy(src a0, dst a1): four lq, four sq through t0-t3
	uint32 PsmtxCopy(CALL& c)
	{
		uint32 src = static_cast<uint32>(c.Lo(A0));
		uint32 dst = static_cast<uint32>(c.Lo(A1));
		for(unsigned int i = 0; i < 4; i++) c.Gpr(T0 + i) = c.Read128(src + i * 16);
		for(unsigned int i = 0; i < 4; i++) c.Write128(dst + i * 16, c.Gpr(T0 + i));
		return 10;
	}

	// PSMTXConcat(a a0, b a1, ab a2): VF20-23 = a, VF24-27 = b, VF28-31 = ab,
	// columns by VMULAx/VMADDAy/VMADDAz/VMADDw
	uint32 PsmtxConcat(CALL& c)
	{
		uint32 a = static_cast<uint32>(c.Lo(A0));
		uint32 b = static_cast<uint32>(c.Lo(A1));
		uint32 ab = static_cast<uint32>(c.Lo(A2));
		for(unsigned int i = 0; i < 4; i++) c.Vf(20 + i) = c.Read128(a + i * 16);
		for(unsigned int i = 0; i < 4; i++) c.Vf(24 + i) = c.Read128(b + i * 16);
		for(unsigned int j = 0; j < 4; j++)
		{
			const uint128& col = c.Vf(24 + j);
			VuMulaBc(c, XYZW, 20, col.nV[0]);
			Store(c.ee.m_State.nCOP2A, VuMaddValue(c, 21, Splat(col.nV[1])), XYZW);
			Store(c.ee.m_State.nCOP2A, VuMaddValue(c, 22, Splat(col.nV[2])), XYZW);
			Store(c.Vf(28 + j), VuMaddValue(c, 23, Splat(col.nV[3])), XYZW);
		}
		for(unsigned int i = 0; i < 4; i++) c.Write128(ab + i * 16, c.Vf(28 + i));
		return 30;
	}

	// PSMTXTranspose(src a0, dst a1): lq t0-t3, pextlw/pextuw/pcpyld/pcpyud, sq
	uint32 PsmtxTranspose(CALL& c)
	{
		uint32 src = static_cast<uint32>(c.Lo(A0));
		uint32 dst = static_cast<uint32>(c.Lo(A1));
		uint128 r[4];
		for(unsigned int i = 0; i < 4; i++) r[i] = c.Read128(src + i * 16);
		auto pextlw = [](const uint128& rs, const uint128& rt) { return uint128{{{rt.nV[0], rs.nV[0], rt.nV[1], rs.nV[1]}}}; };
		auto pextuw = [](const uint128& rs, const uint128& rt) { return uint128{{{rt.nV[2], rs.nV[2], rt.nV[3], rs.nV[3]}}}; };
		auto pcpyld = [](const uint128& rs, const uint128& rt) { uint128 d; d.nD0 = rt.nD0; d.nD1 = rs.nD0; return d; };
		auto pcpyud = [](const uint128& rs, const uint128& rt) { uint128 d; d.nD0 = rs.nD1; d.nD1 = rt.nD1; return d; };
		uint128 t4 = pextlw(r[1], r[0]);
		uint128 t5 = pextuw(r[1], r[0]);
		uint128 t6 = pextlw(r[3], r[2]);
		uint128 t7 = pextuw(r[3], r[2]);
		c.Gpr(T0) = pcpyld(t6, t4);
		c.Gpr(T1) = pcpyud(t4, t6);
		c.Gpr(T2) = pcpyld(t7, t5);
		c.Gpr(T3) = pcpyud(t5, t7);
		c.Gpr(T4) = t4;
		c.Gpr(T5) = t5;
		c.Gpr(T6) = t6;
		c.Gpr(T7) = t7;
		for(unsigned int i = 0; i < 4; i++) c.Write128(dst + i * 16, c.Gpr(T0 + i));
		return 18;
	}

	// The polynomial of Vu0Sinf/Vu0Cosf: VF20.x = start + sum of VF23's lanes,
	// VF23 = coefficients times powers of x^2 (lane by lane, as the code does)
	void Vu0Polynomial(CALL& c, uint32 x, uint32 table, bool sine)
	{
		auto& s = c.ee.m_State;
		// mfc1 t0, f12/f2; qmtc2 t0, vf22 (t0's upper half moves along)
		c.SetLo(T0, Sx(x));
		c.Vf(22) = c.Gpr(T0);
		if(sine)
		{
			c.Vf(21) = c.Read128(table);
			// vmr32.w vf22, vf22 (the same register: x goes through COP2T)
			s.nCOP2T = c.Vf(22).nV[0];
			c.Vf(22).nV[3] = s.nCOP2T;
			VuAdd(c, X, 20, 0, Splat(c.Vf(22).nV[0]));             // vaddx.x vf20, vf0, vf22x
			VuMul(c, X, 22, 22, ToV4(c.Vf(22)));                   // vmul.x vf22, vf22, vf22
			VuMul(c, Y | Z | W, 20, 20, Splat(Vf0Element(0)));     // vmulx.yzw vf20, vf20, vf0x
			VuMul(c, XYZW, 23, 21, Splat(c.Vf(22).nV[3]));         // vmulw vf23, vf21, vf22w
			VuMul(c, XYZW, 23, 23, Splat(c.Vf(22).nV[0]));         // vmulx vf23, vf23, vf22x
			VuMul(c, X | Y | Z, 23, 23, Splat(c.Vf(22).nV[0]));    // vmulx.xyz
		}
		else
		{
			VuAdd(c, X, 20, 0, Splat(Vf0Element(3)));              // vaddw.x vf20, vf0, vf0w
			VuMul(c, X, 22, 22, ToV4(c.Vf(22)));                   // vmul.x vf22, vf22, vf22
			c.Vf(21) = c.Read128(table);
			VuMul(c, XYZW, 23, 21, Splat(c.Vf(22).nV[0]));         // vmulx vf23, vf21, vf22x
		}
		if(sine)
		{
			VuAdd(c, X, 20, 20, Splat(c.Vf(23).nV[3]));            // vaddw.x vf20, vf20, vf23w
			VuMul(c, X | Y, 23, 23, Splat(c.Vf(22).nV[0]));        // vmulx.xy
		}
		else
		{
			VuAdd(c, X, 20, 20, Splat(c.Vf(23).nV[3]));            // vaddw.x
			VuMul(c, X | Y | Z, 23, 23, Splat(c.Vf(22).nV[0]));    // vmulx.xyz
		}
		VuAdd(c, X, 20, 20, Splat(c.Vf(23).nV[2]));                // vaddz.x
		if(!sine) VuMul(c, X | Y, 23, 23, Splat(c.Vf(22).nV[0]));   // vmulx.xy
		if(sine) VuMul(c, X, 23, 23, Splat(c.Vf(22).nV[0]));        // vmulx.x
		VuAdd(c, X, 20, 20, Splat(c.Vf(23).nV[1]));                // vaddy.x
		if(!sine) VuMul(c, X, 23, 23, Splat(c.Vf(22).nV[0]));       // vmulx.x
		VuAdd(c, X, 20, 20, Splat(c.Vf(23).nV[0]));                // vaddx.x
		// qmfc2 t0, vf20; mtc1 t0, f0
		c.Gpr(T0) = c.Vf(20);
		c.Fpr(0) = c.Vf(20).nV[0];
	}

	// The table address: lui v0, hi (params[0]) and addiu v0, v0, lo (params[1])
	inline uint32 TableAddress(const uint32* params)
	{
		return static_cast<uint32>(Sx(params[0] << 16) + Sx(static_cast<uint32>(static_cast<int16>(params[1]))));
	}

	// float Vu0Sinf(float f12): range reduction on the FPU, polynomial on VU0
	uint32 Vu0Sinf(CALL& c)
	{
		const uint32 halfPi = 0x3FC90FDB, twoPi = 0x40C90FDB, pi = 0x40490FDB;
		uint64 hiTable = Sx(c.params[0] << 16);
		uint32 x = c.Fpr(12);
		uint32 cycles = 8;
		auto reduce = [&](uint32 threePiHalf, bool positive) {
			// div.s f1, f12, f3; cvt.w.s f0, f1; mfc1 v0, f0; mtc1 v0, f0; cvt.s.w f0, f0; mul.s f0, f0, f3; sub.s f12, f12, f0
			c.Fpr(3) = twoPi;
			c.Fpr(2) = threePiHalf;
			c.Fpr(1) = FDiv(x, twoPi);
			c.Fpr(0) = FToW(c.Fpr(1));
			c.SetLo(V0, Sx(c.Fpr(0)));
			c.Fpr(0) = static_cast<uint32>(c.Lo(V0));
			c.Fpr(0) = WToF(c.Fpr(0));
			c.Fpr(0) = FMul(c.Fpr(0), twoPi);
			x = FSub(x, c.Fpr(0));
			bool beyond = positive ? FLt(threePiHalf, x) : FLt(x, threePiHalf);
			SetCc(c, beyond);
			if(beyond) x = positive ? FSub(x, twoPi) : FAdd(x, twoPi);
			cycles += 20;
		};
		c.SetLo(AT, Sx(halfPi));
		c.Fpr(4) = halfPi;
		bool above = FLt(halfPi, x);
		SetCc(c, above);
		if(above)
		{
			c.SetLo(AT, Sx(0x4096CBE4));
			reduce(0x4096CBE4, true);
			bool fold = FLt(halfPi, x);
			SetCc(c, fold);
			c.SetLo(V0, hiTable);
			if(fold)
			{
				c.SetLo(AT, Sx(pi));
				c.Fpr(0) = pi;
				x = FSub(pi, x);
			}
		}
		else
		{
			const uint32 minusHalfPi = 0xBFC90FDB, minusPi = 0xC0490FDB;
			c.SetLo(AT, Sx(minusHalfPi));
			c.Fpr(4) = minusHalfPi;
			bool below = FLt(x, minusHalfPi);
			SetCc(c, below);
			c.SetLo(V0, hiTable);
			if(below)
			{
				c.SetLo(AT, Sx(0xC096CBE4));
				reduce(0xC096CBE4, false);
				bool fold = FLt(x, minusHalfPi);
				SetCc(c, fold);
				c.SetLo(V0, hiTable);
				if(fold)
				{
					c.SetLo(AT, Sx(minusPi));
					c.Fpr(0) = minusPi;
					x = FSub(minusPi, x);
				}
			}
		}
		c.Fpr(12) = x;
		uint32 table = TableAddress(c.params);
		c.SetLo(V0, Sx(table));
		Vu0Polynomial(c, x, table, true);
		return cycles + 22;
	}

	// float Vu0Cosf(float f12): |x| reduced to [0, pi/2] (v1 = 1 when negated)
	uint32 Vu0Cosf(CALL& c)
	{
		const uint32 halfPi = 0x3FC90FDB, twoPi = 0x40C90FDB, pi = 0x40490FDB;
		uint32 cycles = 30;
		c.SetLo(V1, 0);
		uint32 x = FAbs(c.Fpr(12));
		c.SetLo(AT, Sx(twoPi));
		c.Fpr(3) = twoPi;
		bool above = FLt(twoPi, x);
		SetCc(c, above);
		if(above)
		{
			c.Fpr(0) = FDiv(x, twoPi);
			c.Fpr(1) = FToW(c.Fpr(0));
			c.SetLo(V0, Sx(c.Fpr(1)));
			c.Fpr(1) = static_cast<uint32>(c.Lo(V0));
			c.Fpr(1) = WToF(c.Fpr(1));
			c.Fpr(1) = FMul(c.Fpr(1), twoPi);
			x = FSub(x, c.Fpr(1));
			cycles += 12;
		}
		c.SetLo(AT, Sx(pi));
		c.Fpr(1) = pi;
		bool beyondPi = FLt(pi, x);
		SetCc(c, beyondPi);
		if(beyondPi) x = FSub(twoPi, x);
		c.SetLo(AT, Sx(halfPi));
		c.Fpr(0) = halfPi;
		bool fold = FLt(halfPi, x);
		SetCc(c, fold);
		c.SetLo(V0, Sx(c.params[0] << 16));
		if(fold)
		{
			x = FSub(pi, x);
			c.SetLo(V1, 1);
		}
		c.Fpr(2) = x;
		uint32 table = TableAddress(c.params);
		c.SetLo(V0, Sx(table));
		Vu0Polynomial(c, x, table, false);
		uint32 result = c.Fpr(0);
		if(c.Lo(V1) != 0) result = FNeg(result);
		c.Fpr(2) = result;
		c.Fpr(0) = result;
		return cycles;
	}

	// void* memset(void* a0, int a1, size_t a2): 16 bytes at a time from a 16-byte
	// aligned start (8 or more bytes), then 8 at a time, then bytes
	uint32 Memset(CALL& c)
	{
		uint64 a0 = c.Lo(A0), a1 = c.Lo(A1), n = c.Lo(A2);
		if(n > 0x02000000) return 0; //not a size this overlay follows (addiu wraps at 32 bits)
		uint8 byte = static_cast<uint8>(a1);
		uint32 start = static_cast<uint32>(a0);
		uint32 cycles = 8;
		uint64 rest = n;
		uint64 v1 = a0;
		if((n >= 8) && ((a0 & 0xF) == 0))
		{
			c.SetLo(A3, a0);
			uint64 t1 = a1 & 0xFF;
			c.SetLo(T1, t1);
			c.SetLo(T2, (n < 32) ? 1 : 0);
			uint64 pattern16 = (t1 << 8) | t1;
			c.SetLo(T0, pattern16);
			// pcpyh v1, t0: halfword 0 of each half, four times
			uint128 t0 = c.Gpr(T0);
			uint64 h0 = t0.nV0 & 0xFFFF, h4 = t0.nV2 & 0xFFFF;
			uint128 v1q;
			v1q.nD0 = h0 * 0x0001000100010001ULL;
			v1q.nD1 = h4 * 0x0001000100010001ULL;
			c.Gpr(V1) = v1q;
			uint64 a3 = a0;
			bool moved = false;
			if(n >= 32)
			{
				uint128 t0q;
				t0q.nD0 = v1q.nD0;
				t0q.nD1 = v1q.nD0;
				c.Gpr(T0) = t0q;
				uint64 blocks = n / 32;
				rest = n - blocks * 32;
				a3 = Sx(static_cast<uint32>(a0) + static_cast<uint32>(blocks * 32));
				moved = true;
				cycles += static_cast<uint32>(blocks) * 6;
			}
			uint64 words = rest / 8;
			if(words != 0)
			{
				rest -= words * 8;
				a3 = Sx(static_cast<uint32>(a3) + static_cast<uint32>(words * 8));
				moved = true;
				cycles += static_cast<uint32>(words) * 5;
			}
			if(moved) c.SetLo(A3, a3);
			v1 = a3;
		}
		else if(n >= 8)
		{
			c.SetLo(A3, a0); //misaligned: straight to the byte loop
		}
		// The bytes (all of them on the paths above write the same value)
		uint32 total = static_cast<uint32>(n);
		uint32 done = 0;
		while(done < total)
		{
			uint32 address = start + done;
			uint32 chunk = std::min<uint32>(total - done, 0x1000 - (address & 0xFFF));
			if(auto host = c.Host(address, chunk)) std::memset(host, byte, chunk);
			else
				for(uint32 i = 0; i < chunk; i++) c.Write8(address + i, byte);
			done += chunk;
		}
		cycles += static_cast<uint32>(rest) * 6;
		c.SetLo(V1, (rest == 0) ? v1 : Sx(static_cast<uint32>(v1) + static_cast<uint32>(rest)));
		c.SetLo(A2, ~0ULL);
		c.SetLo(V0, a0);
		return cycles;
	}

	// unsigned int fptoui(float f12) (libgcc fp-bit): unpacks into a stack frame
	// with __unpack_f (params[0]), then converts. Every load and store of the
	// original happens here too (the frame's stale words are read where it reads them).
	uint32 Fptoui(CALL& c)
	{
		uint64 sp0 = c.Lo(SP);
		uint64 sp = Sx(static_cast<uint32>(sp0) - 48);
		uint32 frame = static_cast<uint32>(sp);
		c.SetLo(SP, sp);
		c.Write64(frame + 32, c.Lo(RA));
		c.SetLo(A0, Sx(frame + 16));
		c.Write32(frame + 16, c.Fpr(12));
		c.SetLo(A1, sp);
		// __unpack_f(a0 = &f, a1 = frame)
		{
			uint64 v0 = Sx(c.Read32(frame + 16));
			uint32 bits = static_cast<uint32>(v0);
			uint64 sign = Sx(bits >> 31);
			uint64 exponent = Sx(bits >> 23) & 0xFF;
			uint64 fraction = v0 & 0x7FFFFF;
			c.SetLo(A0, sign);
			c.SetLo(A2, exponent);
			c.Write32(frame + 4, static_cast<uint32>(sign));
			if(exponent == 0)
			{
				c.SetLo(V0, 2);
				c.SetLo(V1, fraction);
				c.Write32(frame, 2);
			}
			else if(exponent == 0xFF)
			{
				c.SetLo(V1, fraction);
				if(fraction == 0)
				{
					c.SetLo(V0, 4);
					c.Write32(frame, 4);
				}
				else
				{
					c.SetLo(V0, 1);
					c.Write32(frame, ((fraction & 0x100000) == 0) ? 0 : 1);
					c.Write32(frame + 12, static_cast<uint32>(fraction));
				}
			}
			else
			{
				uint64 v1 = Sx(static_cast<uint32>(fraction) << 7) | 0x40000000;
				uint64 a0 = Sx(static_cast<uint32>(exponent) - 127);
				c.SetLo(V1, v1);
				c.SetLo(A0, a0);
				c.SetLo(V0, 3);
				c.Write32(frame + 12, static_cast<uint32>(v1));
				c.Write32(frame + 8, static_cast<uint32>(a0));
				c.Write32(frame, 3);
			}
		}
		// back in fptoui
		uint64 kind = Sx(c.Read32(frame));
		c.SetLo(V1, kind);
		uint64 result = 0;
		if(kind != 2)
		{
			uint64 sign = Sx(c.Read32(frame + 4)); //loaded in the delay slot either way
			if(kind >= 2)
			{
				if(sign == 0)
				{
					uint64 exponent = Sx(c.Read32(frame + 8)); //delay slot
					c.SetLo(A0, exponent);
					if(kind == 4)
					{
						result = ~0ULL;
					}
					else if(static_cast<int64>(exponent) >= 0)
					{
						if(static_cast<int64>(exponent) >= 32)
						{
							result = ~0ULL;
						}
						else if(exponent == 31)
						{
							uint64 v1 = Sx(c.Read32(frame + 12));
							c.SetLo(V1, v1);
							result = Sx(static_cast<uint32>(v1) << 1);
						}
						else
						{
							uint64 v1 = Sx(c.Read32(frame + 12));
							c.SetLo(V1, v1);
							uint32 shift = static_cast<uint32>(30 - static_cast<uint32>(exponent));
							result = Sx(static_cast<uint32>(v1) >> (shift & 31));
						}
					}
				}
			}
		}
		c.SetLo(V0, result);
		c.SetLo(RA, c.Read64(frame + 32));
		c.SetLo(SP, Sx(frame + 48));
		return 40;
	}

	// ------------------------------------------------------------------
	// The table
	// ------------------------------------------------------------------

	constexpr uint32 ANY_LO16 = 0xFFFF0000; //mask: the immediate is an address
	constexpr uint32 ANY_JAL = 0xFC000000;

	struct OVERLAY
	{
		const char* name;
		const uint32* code;
		uint32 words;
		// (word, mask) pairs for the words that hold addresses
		std::vector<std::pair<uint32, uint32>> masks;
		uint32 (*function)(CALL&);
		// Extracts params from the matched code (false: not a match after all)
		bool (*params)(CALL&, uint32 address, uint32* out);
		bool stubSafe; //never declines: may also enter through a syscall stub
	};

	const uint32 g_codePsmtxCopy[] = {
	    0x78880000, 0x78890010, 0x788A0020, 0x788B0030, 0x7CA80000, 0x7CA90010, 0x7CAA0020, 0x7CAB0030,
	    0x03E00008, 0x00000000};
	const uint32 g_codePsmtxConcat[] = {
	    0xD8940000, 0xD8950010, 0xD8960020, 0xD8970030, 0xD8B80000, 0xD8B90010, 0xD8BA0020, 0xD8BB0030,
	    0x4BF8A1BC, 0x4BF8A8BD, 0x4BF8B0BE, 0x4BF8BF0B, 0x4BF9A1BC, 0x4BF9A8BD, 0x4BF9B0BE, 0x4BF9BF4B,
	    0x4BFAA1BC, 0x4BFAA8BD, 0x4BFAB0BE, 0x4BFABF8B, 0x4BFBA1BC, 0x4BFBA8BD, 0x4BFBB0BE, 0x4BFBBFCB,
	    0xF8DC0000, 0xF8DD0010, 0xF8DE0020, 0xF8DF0030, 0x03E00008, 0x00000000};
	const uint32 g_codePsmtxTranspose[] = {
	    0x78880000, 0x78890010, 0x788A0020, 0x788B0030, 0x71286488, 0x71286CA8, 0x716A7488, 0x716A7CA8,
	    0x71CC4389, 0x718E4BA9, 0x71ED5389, 0x71AF5BA9, 0x7CA80000, 0x7CA90010, 0x7CAA0020, 0x7CAB0030,
	    0x03E00008, 0x00000000};
	const uint32 g_codeVu0Sinf[] = {
	    0x3C013FC9, 0x34210FDB, 0x44812000, 0x00000000, 0x460C2034, 0x00000000, 0x4500001E, 0x00000000,
	    0x3C0140C9, 0x34210FDB, 0x44811800, 0x3C014096, 0x3421CBE4, 0x44811000, 0x00000000, 0x00000000,
	    0x46036043, 0x46000824, 0x44020000, 0x44820000, 0x00000000, 0x46800020, 0x46030002, 0x46006301,
	    0x460C1034, 0x00000000, 0x45030001, 0x46036301, 0x460C2034, 0x00000000, 0x4500002B, 0x3C02003C,
	    0x3C014049, 0x34210FDB, 0x44810000, 0x10000026, 0x460C0301, 0x3C01BFC9, 0x34210FDB, 0x44812000,
	    0x00000000, 0x46046034, 0x00000000, 0x4500001E, 0x3C02003C, 0x3C0140C9, 0x34210FDB, 0x44811800,
	    0x3C01C096, 0x3421CBE4, 0x44811000, 0x00000000, 0x00000000, 0x46036043, 0x46000824, 0x44020000,
	    0x44820000, 0x00000000, 0x46800020, 0x46030002, 0x46006301, 0x46026034, 0x00000000, 0x45030001,
	    0x46036300, 0x46046034, 0x00000000, 0x45000006, 0x3C02003C, 0x3C01C049, 0x34210FDB, 0x44810000,
	    0x00000000, 0x460C0301, 0x24420C90, 0x44086000, 0x48A8B000, 0xD8550000, 0x4A36B33D, 0x4B160500,
	    0x4B16B5AA, 0x4AE0A518, 0x4BF6ADDB, 0x4BF6BDD8, 0x4BD6BDD8, 0x4B17A503, 0x4B96BDD8, 0x4B17A502,
	    0x4B16BDD8, 0x4B17A501, 0x4B17A500, 0x4828A000, 0x44880000, 0x03E00008, 0x00000000};
	const uint32 g_codeVu0Cosf[] = {
	    0x0000182D, 0x46006086, 0x46001085, 0x3C0140C9, 0x34210FDB, 0x44811800, 0x00000000, 0x46021834,
	    0x00000000, 0x4500000B, 0x00000000, 0x00000000, 0x00000000, 0x46031003, 0x46000064, 0x44020800,
	    0x44820800, 0x00000000, 0x46800860, 0x46030842, 0x46011081, 0x3C014049, 0x34210FDB, 0x44810800,
	    0x00000000, 0x46020834, 0x00000000, 0x45030001, 0x46021881, 0x3C013FC9, 0x34210FDB, 0x44810000,
	    0x00000000, 0x46020034, 0x00000000, 0x45000003, 0x3C02003C, 0x46020881, 0x24030001, 0x24420CA0,
	    0x44081000, 0x48A8B000, 0x4B000503, 0x4B16B5AA, 0xD8550000, 0x4BF6ADD8, 0x4B17A503, 0x4BD6BDD8,
	    0x4B17A502, 0x4B96BDD8, 0x4B17A501, 0x4B16BDD8, 0x4B17A500, 0x4828A000, 0x44880000, 0x10600002,
	    0x46000086, 0x46001087, 0x03E00008, 0x46001006};
	const uint32 g_codeMemset[] = {
	    0x2CC20008, 0x1440001D, 0x0080182D, 0x3082000F, 0x1440001A, 0x0080382D, 0x30A900FF, 0x2CCA0020,
	    0x0120402D, 0x00081A38, 0x00694025, 0x70081EE9, 0x1540000E, 0x2CC20008, 0x70634389, 0x7CE80000,
	    0x24C6FFE0, 0x24E70010, 0x2CC20020, 0x7CE80000, 0x1040FFFA, 0x24E70010, 0x10000004, 0x2CC20008,
	    0x24C6FFF8, 0x24E70008, 0x2CC20008, 0x00000000, 0x5040FFFB, 0xFCE30000, 0x00E0182D, 0x3C02FFFF,
	    0x24C6FFFF, 0x3442FFFF, 0x10C20009, 0x00000000, 0x3C02FFFF, 0x3442FFFF, 0xA0650000, 0x24C6FFFF,
	    0x00000000, 0x00000000, 0x14C2FFFB, 0x24630001, 0x03E00008, 0x0080102D};
	const uint32 g_codeUnpackF[] = {
	    0x8C820000, 0x3C03007F, 0x3463FFFF, 0x000227C2, 0x000235C2, 0x00431824, 0x30C600FF, 0x14C00004,
	    0xACA40004, 0x24020002, 0x03E00008, 0xACA20000, 0x240200FF, 0x14C2000E, 0x3C024000, 0x14600004,
	    0x3C020010, 0x24020004, 0x03E00008, 0xACA20000, 0x00621024, 0x10400003, 0x24020001, 0x10000002,
	    0xACA20000, 0xACA00000, 0x03E00008, 0xACA3000C, 0x000319C0, 0x00621825, 0x24C4FF81, 0x24020003,
	    0xACA3000C, 0xACA40008, 0x03E00008, 0xACA20000};
	const uint32 g_codeFptoui[] = {
	    0x27BDFFD0, 0xFFBF0020, 0x27A40010, 0xE7AC0010, 0x0C05D60E, 0x03A0282D, 0x8FA30000, 0x38620002,
	    0x10400003, 0x2C620002, 0x10400003, 0x8FA20004, 0x10000016, 0x0000102D, 0x14400014, 0x0000102D,
	    0x38620004, 0x10400005, 0x8FA40008, 0x0480FFF8, 0x28820020, 0x54400004, 0x2882001F, 0x3C02FFFF,
	    0x1000000A, 0x3442FFFF, 0x54400005, 0x2402001E, 0x8FA3000C, 0x2482FFE2, 0x10000004, 0x00431004,
	    0x8FA3000C, 0x00441023, 0x00431006, 0xDFBF0020, 0x03E00008, 0x27BD0030};

	template <size_t N>
	constexpr uint32 Words(const uint32 (&)[N])
	{
		return static_cast<uint32>(N);
	}

	bool NoParams(CALL&, uint32, uint32*)
	{
		return true;
	}
	// lui v0, hi at word 'lui'; addiu v0, v0, lo at word 'addiu' (both read from the code)
	template <uint32 lui, uint32 addiu>
	bool TableParams(CALL& c, uint32 address, uint32* out)
	{
		out[0] = c.Read32(address + lui * 4) & 0xFFFF;
		out[1] = c.Read32(address + addiu * 4) & 0xFFFF;
		return true;
	}
	// jal __unpack_f: the callee must be the expected code
	bool FptouiParams(CALL& c, uint32 address, uint32* out)
	{
		uint32 jal = c.Read32(address + 4 * 4);
		uint32 target = ((address + 4 * 4 + 4) & 0xF0000000) | ((jal & 0x03FFFFFF) << 2);
		for(uint32 i = 0; i < Words(g_codeUnpackF); i++)
			if(c.Read32(target + i * 4) != g_codeUnpackF[i]) return false;
		out[0] = target;
		return true;
	}

	const std::vector<OVERLAY>& Overlays()
	{
		static const std::vector<OVERLAY> overlays = {
		    {"psmtx_copy", g_codePsmtxCopy, Words(g_codePsmtxCopy), {}, &PsmtxCopy, &NoParams, true},
		    {"psmtx_concat", g_codePsmtxConcat, Words(g_codePsmtxConcat), {}, &PsmtxConcat, &NoParams, true},
		    {"psmtx_transpose", g_codePsmtxTranspose, Words(g_codePsmtxTranspose), {}, &PsmtxTranspose, &NoParams, true},
		    {"vu0_sinf", g_codeVu0Sinf, Words(g_codeVu0Sinf), {{31, ANY_LO16}, {44, ANY_LO16}, {68, ANY_LO16}, {74, ANY_LO16}}, &Vu0Sinf, &TableParams<31, 74>, true},
		    {"vu0_cosf", g_codeVu0Cosf, Words(g_codeVu0Cosf), {{36, ANY_LO16}, {39, ANY_LO16}}, &Vu0Cosf, &TableParams<36, 39>, true},
		    {"memset", g_codeMemset, Words(g_codeMemset), {}, &Memset, &NoParams, false},
		    {"fptoui", g_codeFptoui, Words(g_codeFptoui), {{4, ANY_JAL}}, &Fptoui, &FptouiParams, true},
		};
		return overlays;
	}

	// Where verification's originals return to (BIOS area, unused by CPS2OS):
	// a block whose overlay hook does the comparison
	constexpr uint32 VERIFY_RETURN = 0x1FC07F00;
	constexpr uint32 VERIFY_RETURN_ID = 0xFFFFFFFF;
	constexpr uint32 PAGE = 0x1000;

	CEeOverlays* g_overlays = nullptr;
}

struct CEeOverlays::INSTANCE
{
	const OVERLAY* overlay = nullptr;
	uint32 address = 0;
	uint32 params[2] = {};
	bool enabled = true;
	bool stub = false;    //wanted as a stub (settings)
	bool patched = false; //the stub is in place
	std::atomic<uint64_t> calls{0};
	std::atomic<uint64_t> taken{0};
	std::atomic<uint64_t> verified{0};
	std::atomic<uint64_t> mismatches{0};
	std::atomic<uint64_t> flagOnly{0};
	std::atomic<uint64_t> interrupted{0};
	uint32 mismatchLogs = 0;
};

CEeOverlays::CEeOverlays() = default;

CEeOverlays::~CEeOverlays()
{
	Disable();
}

std::vector<std::string> CEeOverlays::GetNames()
{
	std::vector<std::string> names;
	for(const auto& overlay : Overlays()) names.push_back(overlay.name);
	return names;
}

void CEeOverlays::Enable(CPS2VM& vm, const SETTINGS& settings, const char* logPath)
{
	Disable();
	m_vm = &vm;
	m_settings = settings;
	m_log = logPath ? std::fopen(logPath, "w") : nullptr;
	g_overlays = this;
	if(m_settings.verify)
	{
		// The return block's own code (it never runs: its hook always takes the call)
		uint32 words[2] = {0x03E00008, 0x00000000};
		std::memcpy(vm.m_ee->m_bios + (VERIFY_RETURN - PS2::EE_BIOS_ADDR), words, sizeof(words));
		m_ram0.resize(PS2::EE_RAM_SIZE);
		m_ram1.resize(PS2::EE_RAM_SIZE);
		m_spr0.resize(PS2::EE_SPR_SIZE);
	}
	EeOverlay::g_lookupContext = this;
	EeOverlay::g_syscallContext = this;
	EeOverlay::g_vblankContext = this;
	EeOverlay::g_syscall = &CEeOverlays::OnSyscall;
	EeOverlay::g_vblank = &CEeOverlays::OnVBlank;
	EeOverlay::g_lookup = &CEeOverlays::Lookup;
	Log("EE overlays: %s%s\n", m_settings.enabled ? "on" : "off", m_settings.verify ? ", verifying against the recompiler" : "");
}

void CEeOverlays::Disable()
{
	if(!m_vm) return;
	EeOverlay::g_lookup = nullptr;
	EeOverlay::g_syscall = nullptr;
	EeOverlay::g_vblank = nullptr;
	g_overlays = nullptr;
	if(m_log)
	{
		WriteReport(m_log);
		std::fclose(m_log);
		m_log = nullptr;
	}
	for(auto* instance : m_instances) delete instance;
	m_instances.clear();
	m_vm = nullptr;
}

void CEeOverlays::Log(const char* format, ...)
{
	if(!m_log) return;
	va_list args;
	va_start(args, format);
	std::vfprintf(m_log, format, args);
	va_end(args);
	std::fflush(m_log);
}

bool CEeOverlays::Lookup(void* context, CMIPS& ee, uint32 address, EeOverlay::HOOK& hook)
{
	auto self = static_cast<CEeOverlays*>(context);
	if(self->m_settings.verify && (address == VERIFY_RETURN))
	{
		hook.function = &CEeOverlays::Dispatch;
		hook.id = VERIFY_RETURN_ID;
		hook.codeSize = 8;
		return true;
	}
	if(address >= PS2::EE_RAM_SIZE) return false;
	const uint8* ram = self->m_vm->m_ee->m_ram;
	uint32 first = 0;
	std::memcpy(&first, ram + address, 4);
	for(const auto& overlay : Overlays())
	{
		if(first != overlay.code[0]) continue;
		if(address + overlay.words * 4 > PS2::EE_RAM_SIZE) continue;
		bool match = true;
		for(uint32 i = 0; match && (i < overlay.words); i++)
		{
			uint32 mask = ~0U;
			for(const auto& [word, wordMask] : overlay.masks)
				if(word == i) mask = wordMask;
			uint32 value = 0;
			std::memcpy(&value, ram + address + i * 4, 4);
			match = ((value ^ overlay.code[i]) & mask) == 0;
		}
		if(!match) continue;
		CALL call{ee, self->m_vm->m_ee->m_ram, self->m_vm->m_ee->m_spr, nullptr};
		uint32 params[2] = {};
		if(!overlay.params(call, address, params)) continue;
		bool disabled = false;
		for(const auto& name : self->m_settings.disabled)
			if(name == overlay.name) disabled = true;
		if(!self->m_settings.enabled) disabled = true;
		if(disabled && !self->m_settings.verify) return false;
		// One instance per address (blocks there may be compiled again)
		INSTANCE* instance = nullptr;
		uint32 id = 0;
		for(; id < self->m_instances.size(); id++)
			if(self->m_instances[id]->address == address) break;
		if(id == self->m_instances.size())
		{
			instance = new INSTANCE;
			self->m_instances.push_back(instance);
			self->Log("%s at 0x%08X\n", overlay.name, address);
		}
		instance = self->m_instances[id];
		instance->overlay = &overlay;
		instance->address = address;
		std::memcpy(instance->params, params, sizeof(params));
		instance->enabled = !disabled;
		for(const auto& name : self->m_settings.stubs)
			if((name == overlay.name) && overlay.stubSafe && !self->m_settings.verify && !disabled) instance->stub = true;
		hook.function = &CEeOverlays::Dispatch;
		hook.id = id;
		hook.codeSize = overlay.words * 4;
		return true;
	}
	return false;
}

uint32 CEeOverlays::Run(CMIPS& ee, INSTANCE& instance)
{
	CALL call{ee, m_vm->m_ee->m_ram, m_vm->m_ee->m_spr, instance.params};
	return instance.overlay->function(call);
}

uint32 CEeOverlays::Dispatch(CMIPS* ee, uint32 id)
{
	auto self = g_overlays;
	if(!self) return 0;
	if(id == VERIFY_RETURN_ID)
	{
		self->VerifyReturn(*ee);
		return 1;
	}
	auto& instance = *self->m_instances[id];
	instance.calls.fetch_add(1, std::memory_order_relaxed);
	if(self->m_settings.verify) return self->VerifyEntry(*ee, instance);
	if(!instance.enabled) return 0;
	uint32 cycles = self->Run(*ee, instance);
	if(cycles) instance.taken.fetch_add(1, std::memory_order_relaxed);
	return cycles;
}

void CEeOverlays::OnSyscall(void* context, CMIPS& ee)
{
	// A stub at an overlay's entry: the syscall is its second word
	auto self = static_cast<CEeOverlays*>(context);
	uint32 address = ee.m_State.nCOP0[CCOP_SCU::EPC] - 4;
	for(auto* instance : self->m_instances)
	{
		if(!instance->patched || (instance->address != address)) continue;
		instance->calls.fetch_add(1, std::memory_order_relaxed);
		if(self->Run(ee, *instance)) instance->taken.fetch_add(1, std::memory_order_relaxed);
		return;
	}
}

void CEeOverlays::OnVBlank(void* context, CMIPS& ee)
{
	auto self = static_cast<CEeOverlays*>(context);
	self->PatchStubs(ee);
}

void CEeOverlays::PatchStubs(CMIPS& ee)
{
	// Stubs (settings: stubs, only overlays that never decline): the function's first
	// four words become addiu v1, zero, SYSCALL; syscall; jr ra; nop. The overlay was
	// found by its code when a block was first compiled there; here the code is
	// checked again before it is patched, and a stub the game overwrote (code loaded
	// again) is noticed and patched again once the code matches.
	static const uint32 stub[4] = {0x24030000 | EeOverlay::SYSCALL, 0x0000000C, 0x03E00008, 0x00000000};
	uint8* ram = m_vm->m_ee->m_ram;
	for(auto* instance : m_instances)
	{
		if(!instance->stub) continue;
		uint32 address = instance->address;
		const OVERLAY& overlay = *instance->overlay;
		if(std::memcmp(ram + address, stub, sizeof(stub)) == 0)
		{
			instance->patched = true;
			continue;
		}
		instance->patched = false;
		bool match = true;
		for(uint32 i = 0; match && (i < overlay.words); i++)
		{
			uint32 mask = ~0U;
			for(const auto& [word, wordMask] : overlay.masks)
				if(word == i) mask = wordMask;
			uint32 value = 0;
			std::memcpy(&value, ram + address + i * 4, 4);
			match = ((value ^ overlay.code[i]) & mask) == 0;
		}
		if(!match) continue;
		uint32 pc = ee.m_State.nPC & 0x1FFFFFFF;
		if((pc >= address) && (pc < address + sizeof(stub))) continue; //not under the PC
		std::memcpy(ram + address, stub, sizeof(stub));
		ee.m_executor->ClearActiveBlocksInRange(address, address + overlay.words * 4, false);
		instance->patched = true;
		Log("%s at 0x%08X: syscall stub\n", overlay.name, address);
	}
}

uint32 CEeOverlays::VerifyEntry(CMIPS& ee, INSTANCE& instance)
{
	if(m_verifying) return 0; //a call inside the one being checked: the original runs
	uint64 verified = instance.verified.load(std::memory_order_relaxed) + instance.interrupted.load(std::memory_order_relaxed);
	uint64 calls = instance.calls.load(std::memory_order_relaxed);
	if(verified >= m_settings.verifyMax) return 0;
	if((verified >= m_settings.verifyFirst) && ((calls % m_settings.verifyEvery) != 0)) return 0;
	// The state before the call; the original returns to VERIFY_RETURN
	m_state0.resize(sizeof(MIPSSTATE));
	std::memcpy(m_state0.data(), &ee.m_State, sizeof(MIPSSTATE));
	std::memcpy(m_ram0.data(), m_vm->m_ee->m_ram, PS2::EE_RAM_SIZE);
	std::memcpy(m_spr0.data(), m_vm->m_ee->m_spr, PS2::EE_SPR_SIZE);
	m_verifyCount = ee.m_State.nCOP0[CCOP_SCU::COUNT];
	m_verifyId = static_cast<uint32>(std::find(m_instances.begin(), m_instances.end(), &instance) - m_instances.begin());
	ee.m_State.nGPR[RA].nD0 = VERIFY_RETURN;
	m_verifying = true;
	return 0;
}

namespace
{
	struct FIELD
	{
		const char* name;
		size_t offset;
		size_t size;
		bool flag; //VU0 flag state: not kept by overlays
	};

	std::vector<FIELD> StateFields()
	{
		std::vector<FIELD> fields;
		static const char* gprNames[32] = {"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
		                                   "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};
		static char fprNames[32][8], vfNames[33][8], viNames[16][8];
		for(unsigned int i = 0; i < 32; i++) fields.push_back({gprNames[i], offsetof(MIPSSTATE, nGPR) + i * 16, 16, false});
		fields.push_back({"hi", offsetof(MIPSSTATE, nHI), 8, false});
		fields.push_back({"lo", offsetof(MIPSSTATE, nLO), 8, false});
		fields.push_back({"hi1", offsetof(MIPSSTATE, nHI1), 8, false});
		fields.push_back({"lo1", offsetof(MIPSSTATE, nLO1), 8, false});
		fields.push_back({"sa", offsetof(MIPSSTATE, nSA), 4, false});
		for(unsigned int i = 0; i < 32; i++)
		{
			std::snprintf(fprNames[i], sizeof(fprNames[i]), "f%u", i);
			fields.push_back({fprNames[i], offsetof(MIPSSTATE, nCOP1) + i * 4, 4, false});
		}
		fields.push_back({"acc.s", offsetof(MIPSSTATE, nCOP1A), 4, false});
		fields.push_back({"fcsr", offsetof(MIPSSTATE, nFCSR), 4, false});
		for(unsigned int i = 0; i < 33; i++)
		{
			std::snprintf(vfNames[i], sizeof(vfNames[i]), "vf%u", i);
			fields.push_back({vfNames[i], offsetof(MIPSSTATE, nCOP2) + i * 16, 16, false});
		}
		fields.push_back({"acc", offsetof(MIPSSTATE, nCOP2A), 16, false});
		fields.push_back({"q", offsetof(MIPSSTATE, nCOP2Q), 4, false});
		fields.push_back({"i", offsetof(MIPSSTATE, nCOP2I), 4, false});
		fields.push_back({"p", offsetof(MIPSSTATE, nCOP2P), 4, false});
		fields.push_back({"r", offsetof(MIPSSTATE, nCOP2R), 4, false});
		fields.push_back({"cop2t", offsetof(MIPSSTATE, nCOP2T), 4, false});
		for(unsigned int i = 0; i < 16; i++)
		{
			std::snprintf(viNames[i], sizeof(viNames[i]), "vi%u", i);
			fields.push_back({viNames[i], offsetof(MIPSSTATE, nCOP2VI) + i * 4, 4, false});
		}
		fields.push_back({"clipflag", offsetof(MIPSSTATE, nCOP2CF), 4, true});
		fields.push_back({"macflag", offsetof(MIPSSTATE, nCOP2MF), 4, true});
		fields.push_back({"sticky", offsetof(MIPSSTATE, nCOP2SF), 4, true});
		fields.push_back({"divflag", offsetof(MIPSSTATE, nCOP2DF), 4, true});
		fields.push_back({"pipeMac", offsetof(MIPSSTATE, pipeMac), sizeof(FLAG_PIPELINE), true});
		fields.push_back({"pipeSticky", offsetof(MIPSSTATE, pipeSticky), sizeof(FLAG_PIPELINE), true});
		fields.push_back({"pipeClip", offsetof(MIPSSTATE, pipeClip), sizeof(FLAG_PIPELINE), true});
		return fields;
	}

	void FormatBytes(char* out, size_t outSize, const uint8* data, size_t size)
	{
		size_t used = 0;
		out[0] = 0;
		for(size_t i = size; i-- > 0;)
			used += std::snprintf(out + used, (used < outSize) ? outSize - used : 0, "%02X", data[i]);
	}
}

void CEeOverlays::VerifyReturn(CMIPS& ee)
{
	if(!m_verifying) return;
	m_verifying = false;
	auto& instance = *m_instances[m_verifyId];
	auto& state = ee.m_State;
	auto* state0 = reinterpret_cast<MIPSSTATE*>(m_state0.data());
	const uint128 entryRa = state0->nGPR[RA];
	if(state.nCOP0[CCOP_SCU::COUNT] != m_verifyCount)
	{
		// A slice ended inside the original (interrupts, DMA): not comparable
		instance.interrupted.fetch_add(1, std::memory_order_relaxed);
		state.nGPR[RA] = entryRa;
		return;
	}
	uint8* ram = m_vm->m_ee->m_ram;
	uint8* spr = m_vm->m_ee->m_spr;
	// S1: the original's result
	MIPSSTATE state1 = state;
	state1.nGPR[RA] = entryRa;
	std::vector<uint32> pages;
	for(uint32 page = 0; page < PS2::EE_RAM_SIZE; page += PAGE)
	{
		if(std::memcmp(ram + page, m_ram0.data() + page, PAGE) == 0) continue;
		pages.push_back(page);
		std::memcpy(m_ram1.data() + page, ram + page, PAGE);
		std::memcpy(ram + page, m_ram0.data() + page, PAGE);
	}
	std::vector<uint8> spr1(spr, spr + PS2::EE_SPR_SIZE);
	std::memcpy(spr, m_spr0.data(), PS2::EE_SPR_SIZE);
	// S2: the overlay from the same state
	state = *state0;
	uint32 cycles = Run(ee, instance);
	if(m_settings.selfTest)
	{
		//Alternately a register and a byte of memory the original does not touch
		if(instance.verified.load() & 1) ram[0xFFF00] ^= 1;
		else state.nGPR[T0].nV0 ^= 1;
	}
	MIPSSTATE state2 = state;
	std::vector<uint32> pages2;
	for(uint32 page = 0; page < PS2::EE_RAM_SIZE; page += PAGE)
	{
		if(std::binary_search(pages.begin(), pages.end(), page)) continue;
		if(std::memcmp(ram + page, m_ram0.data() + page, PAGE) == 0) continue;
		pages2.push_back(page);
		std::memcpy(m_ram1.data() + page, m_ram0.data() + page, PAGE); //the original left it as it was
	}
	pages.insert(pages.end(), pages2.begin(), pages2.end());
	std::sort(pages.begin(), pages.end());
	// Compare: memory (below the entry sp: the dead stack, reported apart), then registers
	char text[2048];
	size_t used = 0;
	bool mismatch = false, flagMismatch = false;
	auto note = [&](const char* format, auto... args) {
		if(used < sizeof(text)) used += std::snprintf(text + used, sizeof(text) - used, format, args...);
	};
	if(cycles == 0)
	{
		mismatch = true;
		note(" declined");
	}
	uint32 sp0 = static_cast<uint32>(state0->nGPR[CMIPS::SP].nD0) & 0x1FFFFFFF;
	uint32 memDiffs = 0, stackDiffs = 0;
	for(uint32 page : pages)
	{
		for(uint32 i = 0; i < PAGE; i++)
		{
			uint32 address = page + i;
			if(ram[address] == m_ram1[address]) continue;
			bool deadStack = (address < sp0) && (address + 0x10000 >= sp0);
			if(deadStack)
			{
				stackDiffs++;
				continue;
			}
			if(memDiffs++ < 4) note(" mem[%08X] %02X/%02X", address, m_ram1[address], ram[address]);
		}
	}
	for(uint32 i = 0; i < PS2::EE_SPR_SIZE; i++)
	{
		if(spr[i] == spr1[i]) continue;
		if(memDiffs++ < 4) note(" spr[%04X] %02X/%02X", i, spr1[i], spr[i]);
	}
	if(memDiffs) mismatch = true;
	static const std::vector<FIELD> fields = StateFields();
	for(const auto& field : fields)
	{
		auto a = reinterpret_cast<const uint8*>(&state1) + field.offset;
		auto b = reinterpret_cast<const uint8*>(&state2) + field.offset;
		if(std::memcmp(a, b, field.size) == 0) continue;
		if(field.flag)
		{
			flagMismatch = true;
			continue;
		}
		mismatch = true;
		char va[40], vb[40];
		size_t shown = std::min<size_t>(field.size, 16);
		FormatBytes(va, sizeof(va), a, shown);
		FormatBytes(vb, sizeof(vb), b, shown);
		note(" %s %s/%s", field.name, va, vb);
	}
	instance.verified.fetch_add(1, std::memory_order_relaxed);
	if(mismatch)
	{
		instance.mismatches.fetch_add(1, std::memory_order_relaxed);
		if(instance.mismatchLogs++ < 20)
		{
			Log("MISMATCH %s@%08X call %llu: a0 %08X a1 %08X a2 %08X a3 %08X f12 %08X sp %08X (original/overlay):%s\n", instance.overlay->name,
			    instance.address, static_cast<unsigned long long>(instance.calls.load()), state0->nGPR[A0].nV0, state0->nGPR[A1].nV0,
			    state0->nGPR[A2].nV0, state0->nGPR[A3].nV0, state0->nCOP1[12], sp0, text);
		}
	}
	else if(flagMismatch)
	{
		instance.flagOnly.fetch_add(1, std::memory_order_relaxed);
	}
	(void)stackDiffs;
	// The game goes on with the original's result
	for(uint32 page : pages) std::memcpy(ram + page, m_ram1.data() + page, PAGE);
	std::memcpy(spr, spr1.data(), PS2::EE_SPR_SIZE);
	state = state1; //with the caller's ra: the block that called this returns there
}

std::vector<CEeOverlays::STATS> CEeOverlays::GetStats() const
{
	std::vector<STATS> stats;
	for(const auto* instance : m_instances)
	{
		STATS s;
		s.name = instance->overlay->name;
		s.address = instance->address;
		s.calls = instance->calls.load();
		s.taken = instance->taken.load();
		s.verified = instance->verified.load();
		s.mismatches = instance->mismatches.load();
		s.flagOnly = instance->flagOnly.load();
		s.interrupted = instance->interrupted.load();
		stats.push_back(s);
	}
	return stats;
}

void CEeOverlays::WriteReport(std::FILE* file) const
{
	if(!file) return;
	for(const auto& s : GetStats())
	{
		std::fprintf(file, "%-16s 0x%08X calls %llu taken %llu verified %llu mismatches %llu flags-only %llu interrupted %llu\n", s.name.c_str(), s.address,
		             static_cast<unsigned long long>(s.calls), static_cast<unsigned long long>(s.taken), static_cast<unsigned long long>(s.verified),
		             static_cast<unsigned long long>(s.mismatches), static_cast<unsigned long long>(s.flagOnly), static_cast<unsigned long long>(s.interrupted));
	}
	std::fflush(file);
}
