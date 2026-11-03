#pragma once

// Native MPEG-2 video decoder for movie playback (HLE of libmpeg's
// sceMpegGetPicture, see docs/HLE_MPEG.md).
//
// Decodes a video elementary stream (MP@ML, 4:2:0, frame pictures) into planar
// YCbCr frames and returns pictures in display order, like libmpeg. Field
// pictures and dual-prime prediction are reported as unsupported: the caller
// then leaves the movie to the emulated IPU path.
//
// The stream is given as a contiguous buffer; Decode() consumes whole pictures
// only, so a caller can retry with more data when a picture is incomplete.

#include <cstddef>
#include <cstdint>
#include <vector>

class CMpeg2Decoder
{
public:
	struct FRAME
	{
		uint32_t width = 0, height = 0; //in pixels, multiples of 16
		std::vector<uint8_t> y, cb, cr; //planes: width x height, (width/2) x (height/2)
		int pictureType = 0;            //1 I, 2 P, 3 B
		uint32_t temporalReference = 0;
	};

	enum RESULT
	{
		RESULT_PICTURE,     //a picture is ready for display (GetPicture)
		RESULT_NEED_DATA,   //the buffer ends inside a picture: call again with more
		RESULT_END,         //sequence end code reached, no more pictures
		RESULT_UNSUPPORTED, //field pictures, dual prime, 4:2:2...: use the emulated path
		RESULT_ERROR,       //corrupt stream
	};

	CMpeg2Decoder();

	void Reset();

	// Decodes from data[0, size) until a picture is ready for display.
	// consumed: bytes used (whole pictures and headers only), valid for every
	// result except RESULT_NEED_DATA (nothing consumed then). endOfStream:
	// no more data will come, so the last reference picture can be returned.
	RESULT Decode(const uint8_t* data, size_t size, bool endOfStream, size_t& consumed);

	// The picture returned by the last RESULT_PICTURE.
	const FRAME& GetPicture() const
	{
		return *m_display;
	}

	// libmpeg's sceIpuRGB32 layout: 16x16 RGBA macroblocks in raster order,
	// alpha 0x80 (IPU CSC with zero thresholds).
	static void ConvertToRgb32Macroblocks(const FRAME&, uint32_t* out, uint32_t maxMacroblocks);

private:
	struct BITS;

	bool ParseSequenceHeader(BITS&);
	bool ParseExtension(BITS&);
	bool ParsePictureHeader(BITS&);
	RESULT DecodeSlice(BITS&, uint32_t verticalPosition);
	RESULT DecodeMacroblock(BITS&, uint32_t& mbAddress, uint32_t mbWidth);
	bool DecodeMotionVectors(BITS&, int direction, int motionVectorCount, int mvFormat, bool dmv);
	bool DecodeBlock(BITS&, int component, bool intra, int16_t* block);
	void PredictMacroblock(uint32_t mbx, uint32_t mby, int mbType, int motionType);
	void AddBlock(const int16_t* block, uint32_t mbx, uint32_t mby, int index, bool intra, bool fieldDct);
	void FinishPicture();

	// Sequence
	uint32_t m_width = 0, m_height = 0;
	uint8_t m_intraMatrix[64], m_nonIntraMatrix[64]; //raster order
	bool m_haveSequence = false;
	bool m_isMpeg2 = false;

	// Picture
	int m_pictureType = 0;
	uint32_t m_temporalReference = 0;
	int m_fCode[2][2] = {};
	int m_intraDcPrecision = 0;
	int m_pictureStructure = 3;
	bool m_framePredFrameDct = true;
	bool m_concealmentVectors = false;
	bool m_qScaleType = false;
	bool m_intraVlcFormat = false;
	bool m_alternateScan = false;
	bool m_inPicture = false;

	// Slice / macroblock state
	int m_quantiserScale = 0;
	int m_dcPredictor[3] = {};
	int m_pmv[2][2][2] = {};    //[r: first/second vector][s: forward/backward][t: x/y]
	int m_mv[2][2][2] = {};     //vectors of the current macroblock
	int m_fieldSelect[2][2] = {};
	int m_lastMbType = 0, m_lastMotionType = 0;

	// Frames: two references (forward = older) and the B/current frame
	FRAME m_frames[3];
	FRAME* m_forward = nullptr;
	FRAME* m_backward = nullptr;
	FRAME* m_current = nullptr;
	const FRAME* m_display = nullptr;
	bool m_pendingReference = false; //m_backward not displayed yet
};
