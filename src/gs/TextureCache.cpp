#include "TextureCache.h"
#include <algorithm>
#include <cstring>
#include "GsMemory.h"
#include "xxhash.h"

void CTextureCache::SetMemory(uint8* ram, const uint16* clut)
{
	m_ram = ram;
	m_clutRam = clut;
	m_textures.clear();
	m_textureBytes = 0;
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
	ApplyPendingWrite();
	if(!m_pictureSources.empty())
	{
		m_pendingWrite = true;
		m_pendingStart = start;
		m_pendingSize = size;
	}
	uint32 firstPage = start >> GS_SURFACE::PAGE_SHIFT;
	uint32 lastPage = (start + size - 1) >> GS_SURFACE::PAGE_SHIFT;
	MarkPagesWritten(firstPage, lastPage - firstPage + 1);
}

void CTextureCache::MarkAllWritten()
{
	m_pictureSources.clear();
	m_pendingWrite = false;
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
	// Only rows stored in pages written since the decode go. A texture is often
	// larger than what changes (Resident Evil 4 updates 336 rows of a 512 row
	// texture for each movie picture). Tiles holding other pages stay decoded.
	const auto& surface = texture.m_surface;
	uint32 pagesPerRow = texture.m_hasSurface ? std::max<uint32>(surface.pagesPerRow, 1) : 0;
	uint32 pageRowHeight = texture.m_hasSurface ? (surface.phMask + 1) : 0;
	uint32 tilesH = texture.m_tileCount / texture.m_tilesW;
	bool byRows = texture.m_hasSurface && !surface.nibbles && (pageRowHeight >= 8) && ((pageRowHeight % 8) == 0) &&
	              ((texture.m_key.tbp & ((1u << GS_SURFACE::PAGE_SHIFT) - 1)) == 0) &&
	              ((texture.m_key.tw >> surface.pwShift) <= pagesPerRow) && (texture.m_pageCount == pagesPerRow * ((texture.m_key.th + surface.phMask) >> surface.phShift));
	if(byRows)
	{
		uint32 tileRowsPerPageRow = pageRowHeight / 8;
		for(uint32 pageRow = 0; (pageRow * tileRowsPerPageRow) < tilesH; pageRow++)
		{
			bool written = false;
			for(uint32 page = 0; !written && (page < pagesPerRow); page++)
			{
				written = m_pageStamps[(texture.m_firstPage + pageRow * pagesPerRow + page) & (PAGE_COUNT - 1)] > texture.m_decodeStamp;
			}
			if(!written) continue;
			uint32 tileBegin = pageRow * tileRowsPerPageRow * texture.m_tilesW;
			uint32 tileEnd = std::min(tileBegin + tileRowsPerPageRow * texture.m_tilesW, texture.m_tileCount);
			for(uint32 i = tileBegin; i < tileEnd; i++) texture.m_tileValid[i].store(0, std::memory_order_relaxed);
		}
	}
	else
	{
		for(uint32 i = 0; i < texture.m_tileCount; i++)
		{
			texture.m_tileValid[i].store(0, std::memory_order_relaxed);
		}
	}
	texture.m_decodeStamp = m_stamp;
	texture.m_generation++;
}

uint64 CTextureCache::HashPages(const CTexture& texture) const
{
	//Bounded: hashing costs about as much as decoding past this size.
	if(texture.m_pageCount > 64) return 0;
	constexpr uint32 PAGE_SIZE = 1 << GS_SURFACE::PAGE_SHIFT;
	uint32 first = texture.m_firstPage & (PAGE_COUNT - 1);
	uint32 contiguous = std::min<uint32>(texture.m_pageCount, PAGE_COUNT - first);
	uint64 hash = XXH3_64bits(m_ram + first * PAGE_SIZE, contiguous * PAGE_SIZE);
	if(contiguous < texture.m_pageCount)
	{
		hash = XXH3_64bits_withSeed(m_ram, (texture.m_pageCount - contiguous) * PAGE_SIZE, hash);
	}
	return (hash != 0) ? hash : 1;
}

void CTextureCache::ApplyPendingWrite()
{
	if(!m_pendingWrite) return;
	m_pendingWrite = false;
	uint32 start = m_pendingStart, end = m_pendingStart + m_pendingSize;
	m_pictureSources.erase(std::remove_if(m_pictureSources.begin(), m_pictureSources.end(),
	                                      [&](const PICTURE_SOURCE& source) { return (source.start < end) && (start < source.end); }),
	                       m_pictureSources.end());
}

void CTextureCache::AddPictureSource(uint32 dbp, uint32 dbw, uint32 x, uint32 y, uint32 w, uint32 h, std::shared_ptr<const uint8> data)
{
	GS_SURFACE surface;
	if(!data || (w == 0) || (h == 0) || !surface.Init(CGSHandler::PSMCT32, dbp, dbw)) return;
	PICTURE_SOURCE added;
	added.dbp = dbp;
	added.dbw = dbw;
	added.x = x;
	added.y = y;
	added.w = w;
	added.h = h;
	added.start = surface.RowBase(y);
	added.end = surface.RowBase(y + h - 1) + (surface.pagesPerRow << GS_SURFACE::PAGE_SHIFT);
	added.data = std::move(data);
	// The transfer that wrote it (pending) overwrote only its own rectangle among the
	// sources on the same buffer; others it may overlap go.
	if(m_pendingWrite)
	{
		m_pendingWrite = false;
		uint32 start = m_pendingStart, end = m_pendingStart + m_pendingSize;
		m_pictureSources.erase(std::remove_if(m_pictureSources.begin(), m_pictureSources.end(),
		                                      [&](const PICTURE_SOURCE& source) {
			                                      if(!((source.start < end) && (start < source.end))) return false;
			                                      if((source.dbp != dbp) || (source.dbw != dbw)) return true;
			                                      return (source.x < x + w) && (x < source.x + source.w) && (source.y < y + h) && (y < source.y + source.h);
		                                      }),
		                       m_pictureSources.end());
	}
	// The same rectangle sent again (an unchanged transfer marks nothing written)
	m_pictureSources.erase(std::remove_if(m_pictureSources.begin(), m_pictureSources.end(),
	                                      [&](const PICTURE_SOURCE& source) {
		                                      return (source.dbp == dbp) && (source.dbw == dbw) && (source.x == x) && (source.y == y) &&
		                                             (source.w == w) && (source.h == h);
	                                      }),
	                       m_pictureSources.end());
	if(m_pictureSources.size() >= MAX_PICTURE_SOURCES) m_pictureSources.erase(m_pictureSources.begin());
	m_pictureSources.push_back(std::move(added));
}

void CTextureCache::FillFromPictures(CTexture& texture)
{
	ApplyPendingWrite();
	const auto& key = texture.m_key;
	// Spans of invalid tiles a source covers, per tile row; written row by row so that
	// the texels are written in order (tile by tile, each 32 byte piece lands 2 KB from
	// the previous one: as slow as de-swizzling).
	struct SPAN
	{
		const uint32* texels;
		uint32 sourceX, sourceY, sourceW;
		uint32 tx0, tx1;
	};
	uint32 tilesH = texture.m_tileCount / texture.m_tilesW;
	std::vector<std::vector<SPAN>> rows(tilesH);
	for(const auto& source : m_pictureSources)
	{
		if((source.dbp != key.tbp) || (source.dbw != key.tbw)) continue;
		// Whole 8x8 tiles inside both the source and the texture
		uint32 tx0 = (source.x + 7) / 8, ty0 = (source.y + 7) / 8;
		uint32 tx1 = std::min(source.x + source.w, key.tw) / 8, ty1 = std::min(std::min(source.y + source.h, key.th) / 8, tilesH);
		auto texels = reinterpret_cast<const uint32*>(source.data.get());
		for(uint32 ty = ty0; ty < ty1; ty++)
		{
			for(uint32 tx = tx0; tx < tx1;)
			{
				if(texture.m_tileValid[ty * texture.m_tilesW + tx].load(std::memory_order_relaxed))
				{
					tx++;
					continue;
				}
				uint32 end = tx + 1;
				while((end < tx1) && !texture.m_tileValid[ty * texture.m_tilesW + end].load(std::memory_order_relaxed)) end++;
				rows[ty].push_back(SPAN{texels, source.x, source.y, source.w, tx, end});
				tx = end;
			}
		}
	}
	for(uint32 ty = 0; ty < tilesH; ty++)
	{
		auto& spans = rows[ty];
		if(spans.empty()) continue;
		std::sort(spans.begin(), spans.end(), [](const SPAN& a, const SPAN& b) { return a.tx0 < b.tx0; });
		for(uint32 r = 0; r < 8; r++)
		{
			uint32 v = ty * 8 + r;
			uint32* row = texture.m_texels.data() + v * key.tw;
			for(const auto& span : spans)
			{
				std::memcpy(row + span.tx0 * 8, span.texels + (v - span.sourceY) * span.sourceW + (span.tx0 * 8 - span.sourceX), (span.tx1 - span.tx0) * 8 * 4);
			}
		}
		for(const auto& span : spans)
		{
			for(uint32 tx = span.tx0; tx < span.tx1; tx++)
			{
				texture.m_tileValid[ty * texture.m_tilesW + tx].store(1, std::memory_order_release);
				m_decodedTiles++;
			}
		}
	}
}

CTextureCache::CTexture* CTextureCache::Get(const KEY& key)
{
	m_useCounter++;
	ApplyPendingWrite();
	for(auto& texture : m_textures)
	{
		if(texture->m_key == key)
		{
			texture->m_lastUse = m_useCounter;
			if(texture->m_decodeStamp != m_stamp)
			{
				if(!IsValid(*texture))
				{
					uint64 hash = HashPages(*texture);
					if((hash != 0) && (hash == texture->m_contentHash))
					{
						//Pages were rewritten with the same contents.
						texture->m_decodeStamp = m_stamp;
						return texture.get();
					}
					Reset(*texture);
					texture->m_contentHash = hash;
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

	//Evict the least recently used textures
	size_t newBytes = static_cast<size_t>(key.tw) * key.th * 4;
	while(!m_textures.empty() && ((m_textures.size() >= MAX_TEXTURES) || (m_textureBytes + newBytes > MAX_TEXTURE_BYTES)))
	{
		auto lru = std::min_element(m_textures.begin(), m_textures.end(),
		                            [](const auto& a, const auto& b) { return a->m_lastUse < b->m_lastUse; });
		m_textureBytes -= static_cast<size_t>((*lru)->m_key.tw) * (*lru)->m_key.th * 4;
		if(m_deferredRelease) m_retired.push_back(std::move(*lru));
		m_textures.erase(lru);
	}
	m_textureBytes += newBytes;

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
	texture->m_contentHash = HashPages(*texture);

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
	// Texel (x, y) lies in page row y / page height, page x / page width of it.
	// A texture starting inside a page or wider than its buffer reaches into
	// pages beyond that. (The margin pulled a render target under Silent Hill
	// 2's title texture back to GS memory every frame.)
	bool pageAligned = (key.tbp & ((1u << GS_SURFACE::PAGE_SHIFT) - 1)) == 0;
	uint32 margin = (pageAligned && (widthPages <= pagesPerRow)) ? 0 : widthPages + 1;
	pageCount = std::min<uint32>(pagesPerRow * pageRows + margin, PAGE_COUNT);
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
		if(m_hasSurface)
		{
			DecodeSpan(row, x0, x1, v);
			continue;
		}
		for(uint32 u = x0; u < x1; u++)
		{
			row[u] = DecodeTexel(u, v);
		}
	}
	m_tileValid[tileY * m_tilesW + tileX].store(1, std::memory_order_release);
	m_cache->m_decodedTiles++;
}

void CTextureCache::CTexture::DecodeSpan(uint32* row, uint32 x0, uint32 x1, uint32 v) const
{
	// Same results as DecodeTexel, with the format switch and the row's
	// page table lookup hoisted out of the texel loop.
	const uint8* ram = m_cache->m_ram;
	const uint32* clut = m_clut.data();
	if(m_surface.nibbles)
	{
		// NibbleOffset with the row's page and table lookups done once.
		const uint32 nibbleMask = GS_SURFACE::RAM_MASK * 2 + 1;
		uint32 rowPageStart = m_surface.base + (((v >> m_surface.phShift) * m_surface.pagesPerRow) << GS_SURFACE::PAGE_SHIFT);
		const uint32* rowTable = m_surface.table + ((v & m_surface.phMask) << m_surface.pwShift);
		for(uint32 u = x0; u < x1; u++)
		{
			uint32 pageStart = (rowPageStart + ((u >> m_surface.pwShift) << GS_SURFACE::PAGE_SHIFT)) * 2;
			uint32 nibble = (pageStart + rowTable[u & m_surface.pwMask]) & nibbleMask;
			row[u] = clut[(ram[nibble >> 1] >> ((nibble & 1) * 4)) & 0xF];
		}
		return;
	}
	uint32 rowBase = m_surface.RowBase(v);
	const uint32* rowTable = m_surface.RowTable(v);
	auto read32 = [&](uint32 u) { return *reinterpret_cast<const uint32*>(ram + m_surface.Offset(rowBase, rowTable, u)); };
	switch(m_key.tpsm)
	{
	case CGSHandler::PSMT8:
		for(uint32 u = x0; u < x1; u++) row[u] = clut[ram[m_surface.Offset(rowBase, rowTable, u)]];
		break;
	case CGSHandler::PSMCT16:
	case CGSHandler::PSMCT16S:
	case CGSHandler::PSMZ16:
	case CGSHandler::PSMZ16S:
	{
		uint32 ta0 = (m_key.texa & 0xFF) << 24;
		uint32 ta1 = ((m_key.texa >> 8) & 0xFF) << 24;
		bool aem = (m_key.texa >> 16) & 1;
		for(uint32 u = x0; u < x1; u++)
		{
			uint32 color16 = *reinterpret_cast<const uint16*>(ram + m_surface.Offset(rowBase, rowTable, u));
			uint32 rgb = ((color16 & 0x001F) << 3) | ((color16 & 0x03E0) << 6) | ((color16 & 0x7C00) << 9);
			uint32 alpha = (color16 & 0x8000) ? ta1 : ((aem && ((color16 & 0x7FFF) == 0)) ? 0 : ta0);
			row[u] = rgb | alpha;
		}
	}
	break;
	case CGSHandler::PSMCT24:
	case CGSHandler::PSMCT24_UNK:
	case CGSHandler::PSMZ24:
	{
		uint32 ta0 = (m_key.texa & 0xFF) << 24;
		bool aem = (m_key.texa >> 16) & 1;
		for(uint32 u = x0; u < x1; u++)
		{
			uint32 rgb = read32(u) & 0x00FFFFFF;
			row[u] = rgb | ((aem && (rgb == 0)) ? 0 : ta0);
		}
	}
	break;
	case CGSHandler::PSMT8H:
		for(uint32 u = x0; u < x1; u++) row[u] = clut[read32(u) >> 24];
		break;
	case CGSHandler::PSMT4HL:
		for(uint32 u = x0; u < x1; u++) row[u] = clut[(read32(u) >> 24) & 0xF];
		break;
	case CGSHandler::PSMT4HH:
		for(uint32 u = x0; u < x1; u++) row[u] = clut[read32(u) >> 28];
		break;
	default:
		for(uint32 u = x0; u < x1; u++) row[u] = read32(u);
		break;
	}
}

void CTextureCache::CTexture::DecodeCt32PageRow(uint32 pageRow)
{
	// In memory order, one page after the other. A row of a CT32 page jumps all
	// over its 8 KB, so going row by row misses the cache at every few texels
	// (about 40 cycles per texel on the Vita, with large textures).
	const uint8* ram = m_cache->m_ram;
	uint32 tw = m_key.tw;
	if(m_ct32Width != tw)
	{
		m_ct32Width = tw;
		m_ct32Texel.resize(2048);
		for(uint32 y = 0; y < 32; y++)
		{
			for(uint32 x = 0; x < 64; x++)
			{
				m_ct32Texel[m_surface.table[(y << 6) + x] / 4] = y * tw + x;
			}
		}
	}
	const uint32* positions = m_ct32Texel.data();
	uint32 pagesWide = tw >> 6;
	uint32 rowBase = m_surface.base + ((pageRow * m_surface.pagesPerRow) << GS_SURFACE::PAGE_SHIFT);
	uint32* rowTexels = m_texels.data() + pageRow * 32 * tw;
	for(uint32 page = 0; page < pagesWide; page++)
	{
		auto source = reinterpret_cast<const uint32*>(ram + ((rowBase + (page << GS_SURFACE::PAGE_SHIFT)) & GS_SURFACE::RAM_MASK));
		uint32* destination = rowTexels + page * 64;
		for(uint32 i = 0; i < 2048; i += 8)
		{
			destination[positions[i + 0]] = source[i + 0];
			destination[positions[i + 1]] = source[i + 1];
			destination[positions[i + 2]] = source[i + 2];
			destination[positions[i + 3]] = source[i + 3];
			destination[positions[i + 4]] = source[i + 4];
			destination[positions[i + 5]] = source[i + 5];
			destination[positions[i + 6]] = source[i + 6];
			destination[positions[i + 7]] = source[i + 7];
		}
	}
}

void CTextureCache::CTexture::DecodeAll()
{
	if((m_key.tpsm == CGSHandler::PSMCT32) && !m_cache->m_pictureSources.empty()) m_cache->FillFromPictures(*this);
	uint32 tilesH = m_tileCount / m_tilesW;
	bool ct32Pages = m_hasSurface && ((m_key.tpsm == CGSHandler::PSMCT32) || (m_key.tpsm == CGSHandler::PSMCT32_UNK)) &&
	                 ((m_key.tbp & ((1u << GS_SURFACE::PAGE_SHIFT) - 1)) == 0) && ((m_key.tw & 63) == 0) && ((m_key.th & 31) == 0) &&
	                 ((m_key.tw >> 6) <= m_surface.pagesPerRow);
	if(ct32Pages)
	{
		for(uint32 pageRow = 0; pageRow < (m_key.th >> 5); pageRow++)
		{
			uint32 firstTile = pageRow * 4 * m_tilesW, lastTile = firstTile + 4 * m_tilesW;
			bool allInvalid = true;
			for(uint32 i = firstTile; allInvalid && (i < lastTile); i++)
			{
				allInvalid = !m_tileValid[i].load(std::memory_order_acquire);
			}
			if(!allInvalid) continue;
			DecodeCt32PageRow(pageRow);
			for(uint32 i = firstTile; i < lastTile; i++) m_tileValid[i].store(1, std::memory_order_release);
			m_cache->m_decodedTiles += lastTile - firstTile;
		}
	}
	for(uint32 ty = 0; ty < tilesH; ty++)
	{
		// A whole row of invalid tiles: one span per texel row, not one per tile.
		bool wholeRow = m_hasSurface;
		for(uint32 tx = 0; wholeRow && (tx < m_tilesW); tx++)
		{
			wholeRow = !m_tileValid[ty * m_tilesW + tx].load(std::memory_order_acquire);
		}
		if(wholeRow)
		{
			uint32 v1 = std::min((ty + 1) * 8, m_key.th);
			for(uint32 v = ty * 8; v < v1; v++) DecodeSpan(m_texels.data() + v * m_key.tw, 0, m_key.tw, v);
			for(uint32 tx = 0; tx < m_tilesW; tx++) m_tileValid[ty * m_tilesW + tx].store(1, std::memory_order_release);
			m_cache->m_decodedTiles += m_tilesW;
			continue;
		}
		for(uint32 tx = 0; tx < m_tilesW; tx++)
		{
			if(!m_tileValid[ty * m_tilesW + tx].load(std::memory_order_acquire)) DecodeTile(tx, ty);
		}
	}
}

uint32 CTextureCache::GetPictureBand(const CTexture& texture, std::vector<PICTURE_RECT>& rects)
{
	ApplyPendingWrite();
	rects.clear();
	const auto& key = texture.m_key;
	if(key.tpsm != CGSHandler::PSMCT32) return 0;
	uint32 height = 0;
	for(const auto& source : m_pictureSources)
	{
		if((source.dbp != key.tbp) || (source.dbw != key.tbw)) continue;
		if((source.y != 0) || (source.x + source.w > key.tw) || (source.h > key.th)) return 0;
		if((height != 0) && (source.h != height)) return 0;
		height = source.h;
		PICTURE_RECT rect;
		rect.x = source.x;
		rect.y = source.y;
		rect.w = source.w;
		rect.h = source.h;
		rect.data = source.data;
		rects.push_back(std::move(rect));
	}
	if(rects.empty()) return 0;
	std::sort(rects.begin(), rects.end(), [](const PICTURE_RECT& a, const PICTURE_RECT& b) { return a.x < b.x; });
	uint32 x = 0;
	for(const auto& rect : rects)
	{
		if(rect.x != x) return 0;
		x += rect.w;
	}
	if(x != key.tw) return 0;
	return height;
}

void CTextureCache::CTexture::DecodeRows(uint32 v0, uint32 v1)
{
	uint32 tilesH = m_tileCount / m_tilesW;
	uint32 ty1 = std::min((v1 + 7) / 8, tilesH);
	for(uint32 ty = v0 / 8; ty < ty1; ty++)
	{
		for(uint32 tx = 0; tx < m_tilesW; tx++)
		{
			if(!m_tileValid[ty * m_tilesW + tx].load(std::memory_order_acquire)) DecodeTile(tx, ty);
		}
	}
}
