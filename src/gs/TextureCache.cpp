#include "TextureCache.h"
#include <algorithm>
#include "GsMemory.h"

void CTextureCache::SetMemory(uint8* ram, const uint16* clut)
{
	m_ram = ram;
	m_clutRam = clut;
	m_textures.clear();
	MarkAllWritten();
}

void CTextureCache::MarkPagesWritten(uint32 firstPage, uint32 pageCount)
{
	m_stamp++;
	pageCount = std::min<uint32>(pageCount, PAGE_COUNT);
	for(uint32 i = 0; i < pageCount; i++)
	{
		m_pageStamps[(firstPage + i) & (PAGE_COUNT - 1)] = m_stamp;
	}
}

void CTextureCache::MarkBytesWritten(uint32 start, uint32 size)
{
	if(size == 0) return;
	uint32 firstPage = start >> GS_SURFACE::PAGE_SHIFT;
	uint32 lastPage = (start + size - 1) >> GS_SURFACE::PAGE_SHIFT;
	MarkPagesWritten(firstPage, lastPage - firstPage + 1);
}

void CTextureCache::MarkAllWritten()
{
	MarkPagesWritten(0, PAGE_COUNT);
}

bool CTextureCache::IsValid(const CTexture& texture) const
{
	for(uint32 i = 0; i < texture.m_pageCount; i++)
	{
		if(m_pageStamps[(texture.m_firstPage + i) & (PAGE_COUNT - 1)] > texture.m_decodeStamp) return false;
	}
	return true;
}

bool CTextureCache::IsStale(const CTexture* texture) const
{
	return texture->m_decodeStamp != m_stamp;
}

void CTextureCache::Reset(CTexture& texture)
{
	for(uint32 i = 0; i < texture.m_tileCount; i++)
	{
		texture.m_tileValid[i].store(0, std::memory_order_relaxed);
	}
	texture.m_decodeStamp = m_stamp;
	texture.m_generation++;
}

CTextureCache::CTexture* CTextureCache::Get(const KEY& key)
{
	m_useCounter++;
	for(auto& texture : m_textures)
	{
		if(texture->m_key == key)
		{
			texture->m_lastUse = m_useCounter;
			if(texture->m_decodeStamp != m_stamp)
			{
				if(!IsValid(*texture))
				{
					Reset(*texture);
				}
				else
				{
					//Nothing it depends on changed: just move its stamp forward.
					texture->m_decodeStamp = m_stamp;
				}
			}
			return texture.get();
		}
	}

	//Evict the least recently used texture
	if(m_textures.size() >= MAX_TEXTURES)
	{
		auto lru = std::min_element(m_textures.begin(), m_textures.end(),
		                            [](const auto& a, const auto& b) { return a->m_lastUse < b->m_lastUse; });
		if(m_deferredRelease) m_retired.push_back(std::move(*lru));
		m_textures.erase(lru);
	}

	static std::atomic<uint64> nextUniqueId{1};
	auto texture = std::make_unique<CTexture>();
	texture->m_uniqueId = nextUniqueId++;
	texture->m_key = key;
	texture->m_cache = this;
	texture->m_hasSurface = texture->m_surface.Init(key.tpsm, key.tbp, key.tbw);
	texture->m_tilesW = (key.tw + 7) / 8;
	uint32 tilesH = (key.th + 7) / 8;
	texture->m_texels.resize(key.tw * key.th);
	texture->m_tileCount = texture->m_tilesW * tilesH;
	texture->m_tileValid = std::make_unique<std::atomic<uint8>[]>(texture->m_tileCount);
	for(uint32 i = 0; i < texture->m_tileCount; i++)
	{
		texture->m_tileValid[i].store(0, std::memory_order_relaxed);
	}
	texture->m_lastUse = m_useCounter;
	texture->m_decodeStamp = m_stamp;

	GetPageRange(key, texture->m_firstPage, texture->m_pageCount);

	//Snapshot the CLUT (the key holds its hash, so contents are fixed for this entry).
	if(CGsPixelFormats::IsPsmIDTEX(key.tpsm))
	{
		uint32 count = CGsPixelFormats::IsPsmIDTEX4(key.tpsm) ? 16 : 256;
		for(uint32 i = 0; i < count; i++)
		{
			texture->m_clut[i] = texture->LookupClut(i);
		}
	}

	m_textures.push_back(std::move(texture));
	return m_textures.back().get();
}

void CTextureCache::SetDeferredRelease(bool deferred)
{
	m_deferredRelease = deferred;
	if(!deferred) ReleaseRetired();
}

void CTextureCache::ReleaseRetired()
{
	m_retired.clear();
}

void CTextureCache::GetPageRange(const KEY& key, uint32& firstPage, uint32& pageCount)
{
	//Conservative: whole page rows.
	GS_SURFACE surface;
	if(!surface.Init(key.tpsm, key.tbp, key.tbw))
	{
		firstPage = 0;
		pageCount = PAGE_COUNT;
		return;
	}
	uint32 pageRows = (key.th + surface.phMask) >> surface.phShift;
	uint32 pagesPerRow = std::max<uint32>(surface.pagesPerRow, 1);
	uint32 widthPages = (key.tw + surface.pwMask) >> surface.pwShift;
	firstPage = (key.tbp >> GS_SURFACE::PAGE_SHIFT) & (PAGE_COUNT - 1);
	pageCount = std::min<uint32>(pagesPerRow * pageRows + widthPages + 1, PAGE_COUNT);
}

uint32 CTextureCache::CTexture::Expand16(uint32 color16) const
{
	uint32 ta0 = m_key.texa & 0xFF;
	uint32 ta1 = (m_key.texa >> 8) & 0xFF;
	bool aem = (m_key.texa >> 16) & 1;
	uint32 rgb = GsMemory::Color16To32(color16) & 0x00FFFFFF;
	uint32 alpha = 0;
	if(color16 & 0x8000)
	{
		alpha = ta1;
	}
	else if(!(aem && ((color16 & 0x7FFF) == 0)))
	{
		alpha = ta0;
	}
	return rgb | (alpha << 24);
}

uint32 CTextureCache::CTexture::LookupClut(uint32 index) const
{
	const uint16* clut = m_cache->m_clutRam;
	bool clut32 = !GsMemory::IsPsm16(m_key.cpsm);
	bool idtex4 = CGsPixelFormats::IsPsmIDTEX4(m_key.tpsm);
	if(clut32)
	{
		uint32 offset = idtex4 ? (((m_key.csa & 0xF) * 16) + index) : (((m_key.csa * 16) + index) & 0xFF);
		return static_cast<uint32>(clut[offset]) | (static_cast<uint32>(clut[offset + 0x100]) << 16);
	}
	else
	{
		uint32 offset = ((m_key.csa * 16) + index) & 0x1FF;
		return Expand16(clut[offset]);
	}
}

uint32 CTextureCache::CTexture::DecodeTexel(uint32 u, uint32 v) const
{
	uint8* ram = m_cache->m_ram;
	uint32 raw = 0;
	if(m_hasSurface)
	{
		if(m_surface.nibbles)
		{
			uint32 nibble = m_surface.NibbleOffset(u, v);
			raw = (ram[nibble >> 1] >> ((nibble & 1) * 4)) & 0xF;
		}
		else
		{
			uint32 offset = m_surface.Offset(u, v);
			switch(m_key.tpsm)
			{
			case CGSHandler::PSMT8:
				raw = ram[offset];
				break;
			case CGSHandler::PSMCT16:
			case CGSHandler::PSMCT16S:
			case CGSHandler::PSMZ16:
			case CGSHandler::PSMZ16S:
				raw = *reinterpret_cast<const uint16*>(ram + offset);
				break;
			default:
				raw = *reinterpret_cast<const uint32*>(ram + offset);
				break;
			}
		}
	}
	else
	{
		raw = GsMemory::ReadRaw(ram, m_key.tpsm, m_key.tbp, m_key.tbw, u, v);
	}

	switch(m_key.tpsm)
	{
	default:
	case CGSHandler::PSMCT32:
	case CGSHandler::PSMCT32_UNK:
	case CGSHandler::PSMZ32:
		return raw;
	case CGSHandler::PSMCT24:
	case CGSHandler::PSMCT24_UNK:
	case CGSHandler::PSMZ24:
	{
		uint32 rgb = raw & 0x00FFFFFF;
		bool aem = (m_key.texa >> 16) & 1;
		uint32 alpha = (aem && (rgb == 0)) ? 0 : (m_key.texa & 0xFF);
		return rgb | (alpha << 24);
	}
	case CGSHandler::PSMCT16:
	case CGSHandler::PSMCT16S:
	case CGSHandler::PSMZ16:
	case CGSHandler::PSMZ16S:
		return Expand16(raw);
	case CGSHandler::PSMT8:
		return m_clut[raw & 0xFF];
	case CGSHandler::PSMT4:
		return m_clut[raw & 0xF];
	case CGSHandler::PSMT8H:
		return m_clut[raw >> 24];
	case CGSHandler::PSMT4HL:
		return m_clut[(raw >> 24) & 0xF];
	case CGSHandler::PSMT4HH:
		return m_clut[raw >> 28];
	}
}

void CTextureCache::CTexture::DecodeTile(uint32 tileX, uint32 tileY)
{
	uint32 x0 = tileX * 8, y0 = tileY * 8;
	uint32 x1 = std::min(x0 + 8, m_key.tw), y1 = std::min(y0 + 8, m_key.th);
	for(uint32 v = y0; v < y1; v++)
	{
		uint32* row = m_texels.data() + v * m_key.tw;
		for(uint32 u = x0; u < x1; u++)
		{
			row[u] = DecodeTexel(u, v);
		}
	}
	m_tileValid[tileY * m_tilesW + tileX].store(1, std::memory_order_release);
	m_cache->m_decodedTiles++;
}

void CTextureCache::CTexture::DecodeAll()
{
	uint32 tilesH = m_tileCount / m_tilesW;
	for(uint32 ty = 0; ty < tilesH; ty++)
	{
		for(uint32 tx = 0; tx < m_tilesW; tx++)
		{
			if(!m_tileValid[ty * m_tilesW + tx].load(std::memory_order_acquire)) DecodeTile(tx, ty);
		}
	}
}
