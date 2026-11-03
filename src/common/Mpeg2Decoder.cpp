#include "Mpeg2Decoder.h"
#include <algorithm>
#include <cstring>
#include "BitStream.h"
#include "idct/IEEE1180.h"
#include "mpeg2/CodedBlockPatternTable.h"
#include "mpeg2/DcSizeChrominanceTable.h"
#include "mpeg2/DcSizeLuminanceTable.h"
#include "mpeg2/DctCoefficientTable0.h"
#include "mpeg2/DctCoefficientTable1.h"
#include "mpeg2/InverseScanTable.h"
#include "mpeg2/MacroblockAddressIncrementTable.h"
#include "mpeg2/MacroblockTypeBTable.h"
#include "mpeg2/MacroblockTypeITable.h"
#include "mpeg2/MacroblockTypePTable.h"
#include "mpeg2/MotionCodeTable.h"
#include "mpeg2/QuantiserScaleTable.h"
#include "ee/IPU.h"

using namespace MPEG2;

namespace
{
	enum MB_TYPE
	{
		MB_INTRA = 0x01,
		MB_PATTERN = 0x02,
		MB_BACKWARD = 0x04,
		MB_FORWARD = 0x08,
		MB_QUANT = 0x10,
	};

	enum MOTION_TYPE
	{
		MOTION_FIELD = 1,
		MOTION_FRAME = 2,
		MOTION_DUAL_PRIME = 3,
	};

	enum PICTURE_TYPE
	{
		PICTURE_I = 1,
		PICTURE_P = 2,
		PICTURE_B = 3,
	};

	const uint8_t g_defaultIntraMatrix[64] = {
	    8, 16, 19, 22, 26, 27, 29, 34,
	    16, 16, 22, 24, 27, 29, 34, 37,
	    19, 22, 26, 27, 29, 34, 34, 38,
	    22, 22, 26, 27, 29, 34, 37, 40,
	    22, 26, 27, 29, 32, 35, 40, 48,
	    26, 27, 29, 32, 35, 40, 48, 58,
	    26, 27, 29, 34, 38, 46, 56, 69,
	    27, 29, 35, 38, 46, 56, 69, 83};

	struct DECODE_ERROR
	{
		CMpeg2Decoder::RESULT result;
	};

	[[noreturn]] void Fail(CMpeg2Decoder::RESULT result)
	{
		throw DECODE_ERROR{result};
	}

	inline uint8_t Clamp8(int value)
	{
		return static_cast<uint8_t>(std::clamp(value, 0, 255));
	}

	// Raster position of the i-th coefficient in zigzag scan order
	// (CInverseScanTable::m_nTable0 maps raster -> scan).
	struct ZIGZAG
	{
		uint8_t rasterOfScan[2][64];
		ZIGZAG()
		{
			for(unsigned int raster = 0; raster < 64; raster++)
			{
				rasterOfScan[0][CInverseScanTable::m_nTable0[raster]] = static_cast<uint8_t>(raster);
				rasterOfScan[1][CInverseScanTable::m_nTable1[raster]] = static_cast<uint8_t>(raster);
			}
		}
	};
	const ZIGZAG& Zigzag()
	{
		static const ZIGZAG zigzag;
		return zigzag;
	}
}

// MSB-first reader over the stream for the Framework VLC tables. Running out
// of data throws NEED_DATA (pictures are decoded whole or not at all).
struct CMpeg2Decoder::BITS : public Framework::CBitStream
{
	const uint8_t* data;
	size_t size;
	size_t bitPosition = 0;

	BITS(const uint8_t* data, size_t size)
	    : data(data)
	    , size(size)
	{
	}

	size_t BytePosition() const
	{
		return (bitPosition + 7) / 8;
	}

	void Advance(uint8 bits) override
	{
		bitPosition += bits;
	}

	uint8 GetBitIndex() const override
	{
		return static_cast<uint8>(bitPosition & 7);
	}

	bool TryPeekBits_LSBF(uint8, uint32&) override
	{
		return false;
	}

	bool TryPeekBits_MSBF(uint8 count, uint32& result) override
	{
		if(bitPosition + count > size * 8) return false;
		uint32_t value = 0;
		size_t position = bitPosition;
		for(uint8 i = 0; i < count; i++, position++)
		{
			value = (value << 1) | ((data[position >> 3] >> (7 - (position & 7))) & 1);
		}
		result = value;
		return true;
	}

	uint32_t Peek(uint8 count)
	{
		uint32 value = 0;
		if(!TryPeekBits_MSBF(count, value)) Fail(RESULT_NEED_DATA);
		return value;
	}

	uint32_t Get(uint8 count)
	{
		uint32_t value = Peek(count);
		bitPosition += count;
		return value;
	}

	uint32_t Symbol(CVLCTable& table)
	{
		uint32 value = 0;
		auto status = table.TryGetSymbol(this, value);
		if(status == CVLCTable::DECODE_STATUS_NOTENOUGHDATA) Fail(RESULT_NEED_DATA);
		if(status != CVLCTable::DECODE_STATUS_SUCCESS) Fail(RESULT_ERROR);
		return value;
	}

	void ByteAlign()
	{
		bitPosition = (bitPosition + 7) & ~size_t(7);
	}

	// Byte offset of the next start code at or after the current (aligned)
	// position, or size if there is none.
	size_t NextStartCode() const
	{
		size_t position = BytePosition();
		while(position + 3 <= size)
		{
			if((data[position] == 0) && (data[position + 1] == 0) && (data[position + 2] == 1)) return position;
			position++;
		}
		return size;
	}
};

CMpeg2Decoder::CMpeg2Decoder()
{
	Reset();
}

void CMpeg2Decoder::Reset()
{
	m_haveSequence = false;
	m_isMpeg2 = false;
	m_inPicture = false;
	m_forward = nullptr;
	m_backward = nullptr;
	m_current = nullptr;
	m_display = nullptr;
	m_pendingReference = false;
	std::memcpy(m_intraMatrix, g_defaultIntraMatrix, 64);
	std::memset(m_nonIntraMatrix, 16, 64);
}

CMpeg2Decoder::RESULT CMpeg2Decoder::Decode(const uint8_t* data, size_t size, bool endOfStream, size_t& consumed)
{
	consumed = 0;
	BITS bits(data, size);
	size_t pictureStart = 0; //start code of the picture being decoded
	try
	{
		while(true)
		{
			bits.ByteAlign();
			size_t start = bits.NextStartCode();
			if(start + 4 > size)
			{
				if(!endOfStream) Fail(RESULT_NEED_DATA);
				// Stream ends: finish the picture, then show the last reference.
				if(m_inPicture)
				{
					FinishPicture();
					consumed = size;
					if(m_display) return RESULT_PICTURE;
				}
				consumed = size;
				if(m_pendingReference && m_backward)
				{
					m_pendingReference = false;
					m_display = m_backward;
					return RESULT_PICTURE;
				}
				return RESULT_END;
			}
			uint8_t code = data[start + 3];
			bool isSlice = (code >= 0x01) && (code <= 0xAF);
			if(m_inPicture && !isSlice && (code != 0xB5) && (code != 0xB2))
			{
				// Any other start code ends the picture.
				m_inPicture = false;
				FinishPicture();
				consumed = start;
				if(m_display) return RESULT_PICTURE;
			}
			bits.bitPosition = (start + 4) * 8;
			switch(code)
			{
			case 0xB3:
				if(!ParseSequenceHeader(bits)) Fail(RESULT_ERROR);
				break;
			case 0xB5:
				if(!ParseExtension(bits)) Fail(RESULT_UNSUPPORTED);
				break;
			case 0xB7: //sequence end
				consumed = start + 4;
				if(m_pendingReference && m_backward)
				{
					m_pendingReference = false;
					m_display = m_backward;
					return RESULT_PICTURE;
				}
				return RESULT_END;
			case 0x00:
				if(!m_haveSequence) break; //joined mid-stream: wait for a sequence header
				pictureStart = start;
				if(!ParsePictureHeader(bits)) Fail(RESULT_ERROR);
				break;
			default:
				if(isSlice && m_inPicture)
				{
					RESULT result = DecodeSlice(bits, code);
					if(result != RESULT_PICTURE) Fail(result);
				}
				break;
			}
			if(!m_inPicture) consumed = bits.BytePosition();
		}
	}
	catch(const DECODE_ERROR& error)
	{
		// The picture is dropped (need data: restarted with more; errors:
		// skipped). References are untouched: pictures only rotate them when
		// complete. consumed ends at the last completed picture or header.
		m_inPicture = false;
		m_current = nullptr;
		return error.result;
	}
	catch(const CVLCTable::CVLCTableException&)
	{
		m_inPicture = false;
		m_current = nullptr;
		return RESULT_ERROR;
	}
}

bool CMpeg2Decoder::ParseSequenceHeader(BITS& bits)
{
	uint32_t width = bits.Get(12);
	uint32_t height = bits.Get(12);
	bits.Get(4);  //aspect ratio
	bits.Get(4);  //frame rate
	bits.Get(18); //bit rate
	bits.Get(1);  //marker
	bits.Get(10); //vbv buffer size
	bits.Get(1);  //constrained parameters
	const auto& zigzag = Zigzag();
	if(bits.Get(1))
	{
		for(unsigned int i = 0; i < 64; i++) m_intraMatrix[zigzag.rasterOfScan[0][i]] = static_cast<uint8_t>(bits.Get(8));
	}
	else
	{
		std::memcpy(m_intraMatrix, g_defaultIntraMatrix, 64);
	}
	if(bits.Get(1))
	{
		for(unsigned int i = 0; i < 64; i++) m_nonIntraMatrix[zigzag.rasterOfScan[0][i]] = static_cast<uint8_t>(bits.Get(8));
	}
	else
	{
		std::memset(m_nonIntraMatrix, 16, 64);
	}
	if((width == 0) || (height == 0) || (width > 1024) || (height > 1024)) return false;
	uint32_t alignedWidth = (width + 15) & ~15u, alignedHeight = (height + 15) & ~15u;
	if((alignedWidth != m_width) || (alignedHeight != m_height))
	{
		// New size: drop references (a new sequence starts with an I picture).
		m_width = alignedWidth;
		m_height = alignedHeight;
		for(auto& frame : m_frames)
		{
			frame.width = m_width;
			frame.height = m_height;
			frame.y.assign(m_width * m_height, 16);
			frame.cb.assign((m_width / 2) * (m_height / 2), 128);
			frame.cr.assign((m_width / 2) * (m_height / 2), 128);
		}
		m_forward = m_backward = nullptr;
		m_pendingReference = false;
	}
	m_haveSequence = true;
	return true;
}

bool CMpeg2Decoder::ParseExtension(BITS& bits)
{
	uint32_t id = bits.Get(4);
	switch(id)
	{
	case 1: //sequence extension
	{
		m_isMpeg2 = true;
		bits.Get(8);                     //profile and level
		bool progressive = bits.Get(1);  //progressive sequence
		uint32_t chroma = bits.Get(2);   //chroma format
		(void)progressive;
		return chroma == 1;              //4:2:0 only
	}
	case 3: //quant matrix extension
	{
		const auto& zigzag = Zigzag();
		if(bits.Get(1))
		{
			for(unsigned int i = 0; i < 64; i++) m_intraMatrix[zigzag.rasterOfScan[0][i]] = static_cast<uint8_t>(bits.Get(8));
		}
		if(bits.Get(1))
		{
			for(unsigned int i = 0; i < 64; i++) m_nonIntraMatrix[zigzag.rasterOfScan[0][i]] = static_cast<uint8_t>(bits.Get(8));
		}
		return true; //chroma matrices only matter for 4:2:2/4:4:4
	}
	case 8: //picture coding extension
	{
		m_fCode[0][0] = bits.Get(4);
		m_fCode[0][1] = bits.Get(4);
		m_fCode[1][0] = bits.Get(4);
		m_fCode[1][1] = bits.Get(4);
		m_intraDcPrecision = bits.Get(2);
		m_pictureStructure = bits.Get(2);
		bits.Get(1); //top field first
		m_framePredFrameDct = bits.Get(1);
		m_concealmentVectors = bits.Get(1);
		m_qScaleType = bits.Get(1);
		m_intraVlcFormat = bits.Get(1);
		m_alternateScan = bits.Get(1);
		return m_pictureStructure == 3; //frame pictures only
	}
	default:
		return true; //display and other extensions: nothing needed
	}
}

bool CMpeg2Decoder::ParsePictureHeader(BITS& bits)
{
	m_temporalReference = bits.Get(10);
	m_pictureType = bits.Get(3);
	bits.Get(16); //vbv delay
	if((m_pictureType == PICTURE_P) || (m_pictureType == PICTURE_B))
	{
		bits.Get(1);                   //full pel forward vector (MPEG-1)
		m_fCode[0][0] = m_fCode[0][1] = bits.Get(3);
	}
	if(m_pictureType == PICTURE_B)
	{
		bits.Get(1);
		m_fCode[1][0] = m_fCode[1][1] = bits.Get(3);
	}
	if((m_pictureType < PICTURE_I) || (m_pictureType > PICTURE_B)) return false;
	// MPEG-1 defaults; a picture coding extension overrides them.
	m_intraDcPrecision = 0;
	m_pictureStructure = 3;
	m_framePredFrameDct = true;
	m_concealmentVectors = false;
	m_qScaleType = false;
	m_intraVlcFormat = false;
	m_alternateScan = false;
	// P and B pictures need their references.
	if((m_pictureType == PICTURE_P) && !m_backward) return true; //skipped below
	if((m_pictureType == PICTURE_B) && (!m_forward || !m_backward)) return true;
	// Decode into the buffer neither reference uses.
	for(auto& frame : m_frames)
	{
		if((&frame != m_forward) && (&frame != m_backward))
		{
			m_current = &frame;
			break;
		}
	}
	m_current->pictureType = m_pictureType;
	m_current->temporalReference = m_temporalReference;
	m_inPicture = true;
	return true;
}

CMpeg2Decoder::RESULT CMpeg2Decoder::DecodeSlice(BITS& bits, uint32_t verticalPosition)
{
	uint32_t mbWidth = m_width / 16;
	if((verticalPosition - 1) * 16 >= m_height) return RESULT_ERROR;
	int quantCode = bits.Get(5);
	m_quantiserScale = m_qScaleType ? CQuantiserScaleTable::m_nTable1[quantCode] : CQuantiserScaleTable::m_nTable0[quantCode];
	if(bits.Peek(1))
	{
		bits.Get(1); //intra slice flag
		bits.Get(1); //intra slice
		bits.Get(7);
		while(bits.Get(1)) bits.Get(8);
	}
	else
	{
		bits.Get(1);
	}
	int dcReset = 1 << (7 + m_intraDcPrecision);
	m_dcPredictor[0] = m_dcPredictor[1] = m_dcPredictor[2] = dcReset;
	std::memset(m_pmv, 0, sizeof(m_pmv));
	uint32_t mbAddress = (verticalPosition - 1) * mbWidth - 1;
	bool first = true;
	while(true)
	{
		// A slice ends at the next start code (23 zero bits).
		if(!first && (bits.Peek(23) == 0)) break;
		RESULT result = DecodeMacroblock(bits, mbAddress, first ? ~0u : mbWidth);
		if(result != RESULT_PICTURE) return result;
		first = false;
	}
	return RESULT_PICTURE;
}

CMpeg2Decoder::RESULT CMpeg2Decoder::DecodeMacroblock(BITS& bits, uint32_t& mbAddress, uint32_t mbWidthOrFirst)
{
	bool first = (mbWidthOrFirst == ~0u);
	uint32_t mbWidth = m_width / 16;
	uint32_t mbCount = mbWidth * (m_height / 16);

	uint32_t increment = 0;
	while(true)
	{
		uint32_t code = bits.Peek(11);
		if(code == 0x008)
		{
			bits.Get(11);
			increment += 33;
		}
		else if(code == 0x00F)
		{
			bits.Get(11); //stuffing (MPEG-1)
		}
		else
		{
			break;
		}
	}
	increment += bits.Symbol(*CMacroblockAddressIncrementTable::GetInstance());

	// Skipped macroblocks (not at the start of a slice).
	if(!first && (increment > 1))
	{
		for(uint32_t skipped = 1; skipped < increment; skipped++)
		{
			uint32_t address = mbAddress + skipped;
			if(address >= mbCount) return RESULT_ERROR;
			int dcReset = 1 << (7 + m_intraDcPrecision);
			m_dcPredictor[0] = m_dcPredictor[1] = m_dcPredictor[2] = dcReset;
			if(m_pictureType == PICTURE_P)
			{
				std::memset(m_pmv, 0, sizeof(m_pmv));
				std::memset(m_mv, 0, sizeof(m_mv));
				PredictMacroblock(address % mbWidth, address / mbWidth, MB_FORWARD, MOTION_FRAME);
			}
			else if(m_pictureType == PICTURE_B)
			{
				PredictMacroblock(address % mbWidth, address / mbWidth, m_lastMbType, m_lastMotionType);
			}
			else
			{
				return RESULT_ERROR; //no skipped macroblocks in I pictures
			}
		}
	}
	mbAddress += increment;
	if(mbAddress >= mbCount) return RESULT_ERROR;
	uint32_t mbx = mbAddress % mbWidth, mby = mbAddress / mbWidth;

	CVLCTable* typeTable = (m_pictureType == PICTURE_I) ? CMacroblockTypeITable::GetInstance() : (m_pictureType == PICTURE_P) ? CMacroblockTypePTable::GetInstance() : CMacroblockTypeBTable::GetInstance();
	int mbType = bits.Symbol(*typeTable);
	bool intra = (mbType & MB_INTRA) != 0;

	int motionType = MOTION_FRAME;
	if((mbType & (MB_FORWARD | MB_BACKWARD)) && !m_framePredFrameDct) motionType = bits.Get(2);
	if(motionType == MOTION_DUAL_PRIME) return RESULT_UNSUPPORTED;
	bool fieldDct = false;
	if(!m_framePredFrameDct && (intra || (mbType & MB_PATTERN))) fieldDct = bits.Get(1);
	if(mbType & MB_QUANT)
	{
		int quantCode = bits.Get(5);
		m_quantiserScale = m_qScaleType ? CQuantiserScaleTable::m_nTable1[quantCode] : CQuantiserScaleTable::m_nTable0[quantCode];
	}

	int vectorCount = (motionType == MOTION_FIELD) ? 2 : 1;
	int mvFormat = (motionType == MOTION_FIELD) ? 0 : 1; //0 field, 1 frame
	if((mbType & MB_FORWARD) || (intra && m_concealmentVectors))
	{
		if(!DecodeMotionVectors(bits, 0, vectorCount, mvFormat, false)) return RESULT_ERROR;
	}
	if(mbType & MB_BACKWARD)
	{
		if(!DecodeMotionVectors(bits, 1, vectorCount, mvFormat, false)) return RESULT_ERROR;
	}
	if(intra && m_concealmentVectors) bits.Get(1); //marker

	int cbp = 0;
	if(mbType & MB_PATTERN)
	{
		cbp = bits.Symbol(*CCodedBlockPatternTable::GetInstance());
	}
	else if(intra)
	{
		cbp = 0x3F;
	}

	if(!intra)
	{
		int dcReset = 1 << (7 + m_intraDcPrecision);
		m_dcPredictor[0] = m_dcPredictor[1] = m_dcPredictor[2] = dcReset;
	}
	if(intra && !m_concealmentVectors) std::memset(m_pmv, 0, sizeof(m_pmv));
	if((m_pictureType == PICTURE_P) && !intra && !(mbType & MB_FORWARD))
	{
		// "No MC": forward prediction with a zero vector.
		std::memset(m_pmv, 0, sizeof(m_pmv));
		std::memset(m_mv, 0, sizeof(m_mv));
		mbType |= MB_FORWARD;
		motionType = MOTION_FRAME;
	}

	if(!intra)
	{
		PredictMacroblock(mbx, mby, mbType, motionType);
		m_lastMbType = mbType & (MB_FORWARD | MB_BACKWARD);
		m_lastMotionType = motionType;
	}

	int16_t block[64];
	for(int i = 0; i < 6; i++)
	{
		if(!(cbp & (0x20 >> i))) continue;
		int component = (i < 4) ? 0 : (i - 3);
		if(!DecodeBlock(bits, component, intra, block)) return RESULT_ERROR;
		AddBlock(block, mbx, mby, i, intra, fieldDct);
	}
	return RESULT_PICTURE;
}

bool CMpeg2Decoder::DecodeMotionVectors(BITS& bits, int s, int vectorCount, int mvFormat, bool dmv)
{
	(void)dmv;
	auto decodeVector = [&](int r) {
		for(int t = 0; t < 2; t++)
		{
			int fCode = m_fCode[s][t];
			if((fCode < 1) || (fCode > 9)) return false;
			int rSize = fCode - 1;
			int code = static_cast<int16_t>(bits.Symbol(*CMotionCodeTable::GetInstance()));
			int residual = ((rSize != 0) && (code != 0)) ? static_cast<int>(bits.Get(static_cast<uint8>(rSize))) : 0;
			int f = 1 << rSize;
			int high = 16 * f - 1, low = -16 * f, range = 32 * f;
			int delta = 0;
			if(code != 0)
			{
				delta = ((std::abs(code) - 1) * f) + residual + 1;
				if(code < 0) delta = -delta;
			}
			bool fieldInFrame = (mvFormat == 0) && (t == 1);
			int prediction = fieldInFrame ? (m_pmv[r][s][t] >> 1) : m_pmv[r][s][t];
			int vector = prediction + delta;
			if(vector < low) vector += range;
			if(vector > high) vector -= range;
			m_pmv[r][s][t] = fieldInFrame ? (vector * 2) : vector;
			m_mv[r][s][t] = vector;
		}
		return true;
	};
	if(vectorCount == 1)
	{
		if(!decodeVector(0)) return false;
		m_pmv[1][s][0] = m_pmv[0][s][0];
		m_pmv[1][s][1] = m_pmv[0][s][1];
	}
	else
	{
		m_fieldSelect[0][s] = bits.Get(1);
		if(!decodeVector(0)) return false;
		m_fieldSelect[1][s] = bits.Get(1);
		if(!decodeVector(1)) return false;
	}
	return true;
}

bool CMpeg2Decoder::DecodeBlock(BITS& bits, int component, bool intra, int16_t* raster)
{
	int coefficients[64] = {}; //scan order
	unsigned int index = 0;
	CDctCoefficientTable& table = (intra && m_intraVlcFormat) ? static_cast<CDctCoefficientTable&>(CDctCoefficientTable1::GetInstance())
	                                                         : static_cast<CDctCoefficientTable&>(CDctCoefficientTable0::GetInstance());
	auto check = [](CVLCTable::DECODE_STATUS status) {
		if(status == CVLCTable::DECODE_STATUS_NOTENOUGHDATA) Fail(RESULT_NEED_DATA);
		if(status != CVLCTable::DECODE_STATUS_SUCCESS) Fail(RESULT_ERROR);
	};
	if(intra)
	{
		CVLCTable& dcTable = (component == 0) ? *CDcSizeLuminanceTable::GetInstance() : *CDcSizeChrominanceTable::GetInstance();
		uint32_t dcSize = bits.Symbol(dcTable);
		int diff = 0;
		if(dcSize != 0)
		{
			uint32_t value = bits.Get(static_cast<uint8>(dcSize));
			diff = (value & (1u << (dcSize - 1))) ? static_cast<int>(value) : static_cast<int>(value) - static_cast<int>((1u << dcSize) - 1);
		}
		m_dcPredictor[component] += diff;
		coefficients[0] = m_dcPredictor[component];
		index = 1;
	}
	else
	{
		RUNLEVELPAIR pair;
		check(table.TryGetRunLevelPairDc(&bits, &pair, m_isMpeg2));
		index = pair.run;
		if(index >= 64) return false;
		coefficients[index] = static_cast<int16_t>(pair.level);
		index++;
	}
	while(true)
	{
		bool eob = false;
		check(table.TryIsEndOfBlock(&bits, eob));
		if(eob)
		{
			check(table.TrySkipEndOfBlock(&bits));
			break;
		}
		RUNLEVELPAIR pair;
		check(table.TryGetRunLevelPair(&bits, &pair, m_isMpeg2));
		index += pair.run;
		if(index >= 64) return false;
		coefficients[index] = static_cast<int16_t>(pair.level);
		index++;
	}

	// Inverse scan, inverse quantisation, saturation, mismatch control.
	const uint8_t* rasterOfScan = Zigzag().rasterOfScan[m_alternateScan ? 1 : 0];
	const uint8_t* matrix = intra ? m_intraMatrix : m_nonIntraMatrix;
	int dequantised[64] = {};
	int sum = 0;
	for(unsigned int i = 0; i < 64; i++)
	{
		int qf = coefficients[i];
		if(qf == 0) continue;
		unsigned int position = rasterOfScan[i];
		int value;
		if(intra && (i == 0))
		{
			value = qf * (8 >> m_intraDcPrecision);
		}
		else if(intra)
		{
			value = (qf * matrix[position] * m_quantiserScale * 2) / 32;
		}
		else
		{
			int sign = (qf > 0) - (qf < 0);
			value = ((2 * qf + sign) * matrix[position] * m_quantiserScale) / 32;
		}
		if(m_ipuCompatibleQuantisation && !(intra && (i == 0)) && (value != 0) && ((value & 1) == 0))
		{
			// Play!'s IPU: MPEG-1 oddification of every AC coefficient.
			int sign = (qf > 0) ? 1 : -1;
			value = (value - sign) | 1;
		}
		value = std::clamp(value, -2048, 2047);
		dequantised[position] = value;
		sum += value;
	}
	// MPEG-2 mismatch control (the emulated IPU does not apply it).
	if(!m_ipuCompatibleQuantisation && ((sum & 1) == 0)) dequantised[63] += (dequantised[63] & 1) ? -1 : 1;

	int16_t input[64];
	for(unsigned int i = 0; i < 64; i++) input[i] = static_cast<int16_t>(dequantised[i]);
	IDCT::CIEEE1180::GetInstance()->Transform(input, raster);
	return true;
}

namespace
{
	// One prediction from a reference plane in half-pel units. fieldStep 2
	// reads every other line (field prediction), starting at line 'field'.
	void PredictPlane(uint8_t* dst, uint32_t dstStride, uint32_t dstLineStep, const uint8_t* src, uint32_t srcWidth, uint32_t srcHeight,
	                  int x2, int y2, uint32_t width, uint32_t height, int field, bool average)
	{
		int ix = x2 >> 1, iy = y2 >> 1;
		bool hx = (x2 & 1) != 0, hy = (y2 & 1) != 0;
		int lineStep = (field >= 0) ? 2 : 1;
		int lineBase = (field >= 0) ? field : 0;
		int linesAvailable = (field >= 0) ? static_cast<int>(srcHeight / 2) : static_cast<int>(srcHeight);
		auto sample = [&](int x, int y) {
			x = std::clamp(x, 0, static_cast<int>(srcWidth) - 1);
			y = std::clamp(y, 0, linesAvailable - 1);
			return static_cast<int>(src[(lineBase + y * lineStep) * srcWidth + x]);
		};
		for(uint32_t j = 0; j < height; j++)
		{
			uint8_t* row = dst + j * dstLineStep * dstStride;
			for(uint32_t i = 0; i < width; i++)
			{
				int x = ix + static_cast<int>(i), y = iy + static_cast<int>(j);
				int value;
				if(!hx && !hy) value = sample(x, y);
				else if(hx && !hy) value = (sample(x, y) + sample(x + 1, y) + 1) >> 1;
				else if(!hx && hy) value = (sample(x, y) + sample(x, y + 1) + 1) >> 1;
				else value = (sample(x, y) + sample(x + 1, y) + sample(x, y + 1) + sample(x + 1, y + 1) + 2) >> 2;
				row[i] = average ? static_cast<uint8_t>((row[i] + value + 1) >> 1) : static_cast<uint8_t>(value);
			}
		}
	}

	// MPEG-2 chroma vector for 4:2:0: luma vector / 2, truncated toward zero.
	inline int ChromaVector(int luma)
	{
		return luma / 2;
	}
}

void CMpeg2Decoder::PredictMacroblock(uint32_t mbx, uint32_t mby, int mbType, int motionType)
{
	FRAME& dst = *m_current;
	uint32_t width = m_width, chromaWidth = m_width / 2;
	bool first = true;
	for(int s = 0; s < 2; s++)
	{
		int flag = (s == 0) ? MB_FORWARD : MB_BACKWARD;
		if(!(mbType & flag)) continue;
		const FRAME* ref = (s == 0) ? ((m_pictureType == PICTURE_B) ? m_forward : m_backward) : m_backward;
		if(!ref) continue;
		bool average = !first;
		if(motionType == MOTION_FRAME)
		{
			int mvx = m_mv[0][s][0], mvy = m_mv[0][s][1];
			PredictPlane(&dst.y[(mby * 16) * width + mbx * 16], width, 1, ref->y.data(), width, m_height,
			             static_cast<int>(mbx * 32) + mvx, static_cast<int>(mby * 32) + mvy, 16, 16, -1, average);
			int cx = ChromaVector(mvx), cy = ChromaVector(mvy);
			for(int plane = 0; plane < 2; plane++)
			{
				auto& d = plane ? dst.cr : dst.cb;
				const auto& r = plane ? ref->cr : ref->cb;
				PredictPlane(&d[(mby * 8) * chromaWidth + mbx * 8], chromaWidth, 1, r.data(), chromaWidth, m_height / 2,
				             static_cast<int>(mbx * 16) + cx, static_cast<int>(mby * 16) + cy, 8, 8, -1, average);
			}
		}
		else //field prediction in a frame picture: top and bottom fields separately
		{
			for(int f = 0; f < 2; f++)
			{
				int mvx = m_mv[f][s][0], mvy = m_mv[f][s][1];
				int srcField = m_fieldSelect[f][s];
				PredictPlane(&dst.y[(mby * 16 + f) * width + mbx * 16], width, 2, ref->y.data(), width, m_height,
				             static_cast<int>(mbx * 32) + mvx, static_cast<int>(mby * 16) + mvy, 16, 8, srcField, average);
				int cx = ChromaVector(mvx), cy = ChromaVector(mvy);
				for(int plane = 0; plane < 2; plane++)
				{
					auto& d = plane ? dst.cr : dst.cb;
					const auto& r = plane ? ref->cr : ref->cb;
					PredictPlane(&d[(mby * 8 + f) * chromaWidth + mbx * 8], chromaWidth, 2, r.data(), chromaWidth, m_height / 2,
					             static_cast<int>(mbx * 16) + cx, static_cast<int>(mby * 8) + cy, 8, 4, srcField, average);
				}
			}
		}
		first = false;
	}
}

void CMpeg2Decoder::AddBlock(const int16_t* block, uint32_t mbx, uint32_t mby, int index, bool intra, bool fieldDct)
{
	FRAME& dst = *m_current;
	uint8_t* base;
	uint32_t stride;
	if(index < 4)
	{
		uint32_t x = mbx * 16 + (index & 1) * 8;
		uint32_t y, lineStep;
		if(fieldDct)
		{
			y = mby * 16 + (index >> 1);
			lineStep = 2;
		}
		else
		{
			y = mby * 16 + (index >> 1) * 8;
			lineStep = 1;
		}
		base = &dst.y[y * m_width + x];
		stride = m_width * lineStep;
	}
	else
	{
		auto& plane = (index == 4) ? dst.cb : dst.cr;
		base = &plane[(mby * 8) * (m_width / 2) + mbx * 8];
		stride = m_width / 2;
	}
	for(unsigned int j = 0; j < 8; j++)
	{
		uint8_t* row = base + j * stride;
		for(unsigned int i = 0; i < 8; i++)
		{
			int value = block[j * 8 + i];
			row[i] = Clamp8(intra ? value : (row[i] + value));
		}
	}
}

void CMpeg2Decoder::FinishPicture()
{
	m_display = nullptr;
	if(!m_current) return;
	if(m_pictureType == PICTURE_B)
	{
		m_display = m_current;
	}
	else
	{
		if(m_pendingReference && m_backward) m_display = m_backward;
		m_forward = m_backward;
		m_backward = m_current;
		m_pendingReference = true;
	}
	m_current = nullptr;
}

void CMpeg2Decoder::ConvertToRgb32Macroblocks(const FRAME& frame, uint32_t* out, uint32_t maxMacroblocks)
{
	uint32_t mbWidth = frame.width / 16, mbHeight = frame.height / 16;
	uint8_t raw8[0x180]; //256 Y, 64 Cb, 64 Cr
	uint32_t count = 0;
	for(uint32_t mby = 0; mby < mbHeight; mby++)
	{
		for(uint32_t mbx = 0; mbx < mbWidth; mbx++)
		{
			if(count++ >= maxMacroblocks) return;
			for(unsigned int j = 0; j < 16; j++) std::memcpy(raw8 + j * 16, &frame.y[(mby * 16 + j) * frame.width + mbx * 16], 16);
			for(unsigned int j = 0; j < 8; j++)
			{
				std::memcpy(raw8 + 0x100 + j * 8, &frame.cb[(mby * 8 + j) * (frame.width / 2) + mbx * 8], 8);
				std::memcpy(raw8 + 0x140 + j * 8, &frame.cr[(mby * 8 + j) * (frame.width / 2) + mbx * 8], 8);
			}
			CIPU::ConvertMacroblockToRgba32(raw8, out, 0, 0);
			out += 256;
		}
	}
}
