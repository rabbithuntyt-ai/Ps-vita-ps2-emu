#pragma once

// Decoded texture cache for the software GS.
//
// Sampling a PS2 texture straight from GS memory means de-swizzling, CLUT
// lookup and TEXA alpha expansion for every texel fetch. Instead, textures are
// decoded to linear RGBA8888 lazily, one 8x8 tile at a time on first touch,
// so only the texels that are actually sampled are ever decoded.
//
// Correctness relies on write tracking: every write to GS memory (transfers
// and rendering) stamps the 8KB pages it touches. A cached texture is
// discarded as soon as any page it spans has been written since it was
// decoded. CLUT and TEXA are part of the cache key.

#include <array>
#include <memory>
#include <vector>
#include "Types.h"
#include "GsSurface.h"

class CTextureCache
{
public:
	struct KEY
	{
		uint32 tbp = 0;
		uint32 tbw = 0;
		uint32 tpsm = 0;
		uint32 tw = 0, th = 0;
		uint32 cpsm = 0, csa = 0;
		uint64 clutHash = 0;
		uint32 texa = 0;

		bool operator==(const KEY& rhs) const
		{
			return tbp == rhs.tbp && tbw == rhs.tbw && tpsm == rhs.tpsm && tw == rhs.tw && th == rhs.th &&
			       cpsm == rhs.cpsm && csa == rhs.csa && clutHash == rhs.clutHash && texa == rhs.texa;
		}
	};

	class CTexture
	{
	public:
		inline uint32 Fetch(uint32 u, uint32 v)
		{
			uint32 tile = (v >> 3) * m_tilesW + (u >> 3);
			if(!m_tileValid[tile]) DecodeTile(u >> 3, v >> 3);
			return m_texels[v * m_key.tw + u];
		}

		// Region clamp/repeat modes can address texels outside of TW x TH.
		inline uint32 FetchAny(uint32 u, uint32 v)
		{
			if((u < m_key.tw) && (v < m_key.th)) return Fetch(u, v);
			return DecodeTexel(u & 2047, v & 2047);
		}

		uint32 GetDecodeStamp() const
		{
			return m_decodeStamp;
		}

	private:
		friend class CTextureCache;
		void DecodeTile(uint32 tileX, uint32 tileY);
		uint32 DecodeTexel(uint32 u, uint32 v) const;
		uint32 LookupClut(uint32 index) const;
		uint32 Expand16(uint32 color16) const;

		KEY m_key;
		CTextureCache* m_cache = nullptr;
		GS_SURFACE m_surface;
		bool m_hasSurface = false;
		std::vector<uint32> m_texels;
		std::vector<uint8> m_tileValid;
		uint32 m_tilesW = 0;
		uint32 m_firstPage = 0;
		uint32 m_pageCount = 0;
		uint32 m_decodeStamp = 0;
		uint64 m_lastUse = 0;
		std::array<uint32, 256> m_clut = {};
	};

	void SetMemory(uint8* ram, const uint16* clut);

	// Records a write to GS memory. Ranges are in bytes and may wrap.
	void MarkPagesWritten(uint32 firstPage, uint32 pageCount);
	void MarkBytesWritten(uint32 start, uint32 size);
	void MarkAllWritten();

	// Returns a texture valid for the current memory contents.
	CTexture* Get(const KEY&);

	// True if a write happened since the given texture was last validated.
	bool IsStale(const CTexture*) const;

	uint32 GetStamp() const
	{
		return m_stamp;
	}

	uint32 GetDecodedTileCount() const
	{
		return m_decodedTiles;
	}

private:
	enum
	{
		PAGE_COUNT = 512,
		MAX_TEXTURES = 24,
	};

	bool IsValid(const CTexture&) const;
	void Reset(CTexture&);

	uint8* m_ram = nullptr;
	const uint16* m_clutRam = nullptr;
	std::array<uint32, PAGE_COUNT> m_pageStamps = {};
	uint32 m_stamp = 1;
	uint64 m_useCounter = 0;
	uint32 m_decodedTiles = 0;
	std::vector<std::unique_ptr<CTexture>> m_textures;
};
