// Original code for RetroPixels (PixelU). Not derived from Cemu.
// H.264 Annex B helpers: a bit reader over one NAL payload, a splitter that walks start codes,
// and a small header parser (SPS, PPS, slice headers) with its state structs.
#pragma once

// Reads bits MSB first from one NAL unit payload. Emulation prevention bytes (00 00 03) are skipped
// unless disabled. Reads past the end return zero bits.
class RBSPInputBitstream
{
public:
	RBSPInputBitstream() {};

	RBSPInputBitstream(uint8* stream, uint32 length, bool removeEmulationByte = true)
		: streamPtr(stream), streamLength(length), m_removeEmulationByte(removeEmulationByte)
	{
		currentRBSPByte = length > 0 ? stream[0] : 0;
	}

	bool hasError() const { return errorFlag; }
	bool isByteAligned() const { return bitIndex == 0; }
	bool isEndOfStream() { return readIndex >= streamLength; }

	uint8* getBasePtr() { return streamPtr; }
	sint32 getBaseLength() { return streamLength; }

	uint8 readU8()
	{
		if (isEndOfStream())
			return 0;
		cemu_assert_debug(bitIndex == 0);
		const uint8 v = currentRBSPByte;
		advanceByte();
		return v;
	}

	uint8 readBit()
	{
		if (isEndOfStream())
			return 0;
		const uint8 bit = (currentRBSPByte >> (7 - bitIndex)) & 1;
		if (++bitIndex == 8)
		{
			bitIndex = 0;
			advanceByte();
		}
		return bit;
	}

	uint32 readBits(sint32 count)
	{
		uint32 v = 0;
		while (count-- > 0)
			v = (v << 1) | readBit();
		return v;
	}

	template<int N>
	uint32 readBits()
	{
		return readBits(N);
	}

	// unsigned Exp-Golomb code; more than 30 leading zeros yields 0
	uint32 readUV_E()
	{
		for (sint32 zeros = 0; zeros < 31; zeros++)
		{
			if (readBit() == 0)
				continue;
			if (zeros == 0)
				return 0;
			return ((1u << zeros) - 1) + readBits(zeros);
		}
		cemu_assert_debug(false);
		return 0;
	}

	// signed Exp-Golomb code: 1, 2, 3, 4 ... map to 1, -1, 2, -2 ...
	sint32 readSV_E()
	{
		const uint32 code = readUV_E();
		const sint32 magnitude = (sint32)((code + 1) / 2);
		return (code & 1) ? magnitude : -magnitude;
	}

	// rbsp_trailing_bits: a one bit, zero bits up to the byte boundary, and nothing after that
	bool readTrailingRBSPBits()
	{
		if (readBit() != 1)
			return false;
		while (bitIndex != 0)
		{
			if (readBit() != 0)
				return false;
		}
		return isEndOfStream();
	}

	bool more_rbsp_data()
	{
		const uint32 savedIndex = readIndex;
		const sint32 savedBit = bitIndex;
		const bool atTrailingBits = readTrailingRBSPBits();
		readIndex = savedIndex;
		bitIndex = savedBit;
		currentRBSPByte = readIndex < streamLength ? streamPtr[readIndex] : 0;
		return !atTrailingBits;
	}

private:
	void advanceByte()
	{
		readIndex++;
		// the window is checked on the raw bytes, so a skipped 03 can itself start the next pattern
		if (m_removeEmulationByte && readIndex >= 2 && readIndex < streamLength &&
			streamPtr[readIndex] == 0x03 && streamPtr[readIndex - 1] == 0x00 && streamPtr[readIndex - 2] == 0x00)
			readIndex++;
		currentRBSPByte = readIndex < streamLength ? streamPtr[readIndex] : 0;
	}

	uint8* streamPtr{};
	uint32 streamLength{};
	uint32 readIndex{};
	uint8 currentRBSPByte{};
	sint32 bitIndex{};
	bool errorFlag{};
	bool m_removeEmulationByte{};
};

// Splits an Annex B byte stream into NAL units at 00 00 01 / 00 00 00 01 start codes.
class NALInputBitstream
{
public:
	NALInputBitstream(uint8* stream, uint32 length) : nalStreamPtr(stream), nalStreamLength((sint32)length) {}

	bool hasError() { return errorFlag; }
	bool isEndOfStream() { return readIndex >= nalStreamLength; }

	// Returns the payload between the current position and the next start code. When no further
	// start code exists the unit runs to the end of the buffer, unless mustEndAtStartCode is set.
	bool getNextRBSP(RBSPInputBitstream& rbspInputBitstream, bool mustEndAtStartCode = false)
	{
		sint32 unitEnd = FindNextStartCode();
		if (unitEnd < 0)
		{
			if (mustEndAtStartCode)
				return false;
			unitEnd = nalStreamLength;
		}
		if (unitEnd <= readIndex)
			return false;
		// step over the start code of this unit. Like the console library, the four byte form is
		// tested whenever three bytes remain
		if (nalStreamLength - readIndex >= 3 && IsShortStartCode(readIndex))
			readIndex += 3;
		else if (nalStreamLength - readIndex >= 3 && IsLongStartCode(readIndex))
			readIndex += 4;
		cemu_assert(readIndex <= unitEnd);
		cemu_assert_debug(readIndex != unitEnd);
		rbspInputBitstream = RBSPInputBitstream(nalStreamPtr + readIndex, unitEnd - readIndex);
		readIndex = unitEnd;
		return true;
	}

private:
	bool IsShortStartCode(sint32 i) const { return nalStreamPtr[i] == 0 && nalStreamPtr[i + 1] == 0 && nalStreamPtr[i + 2] == 1; }
	bool IsLongStartCode(sint32 i) const { return nalStreamPtr[i] == 0 && nalStreamPtr[i + 1] == 0 && nalStreamPtr[i + 2] == 0 && nalStreamPtr[i + 3] == 1; }

	// offset of the start code that follows the unit at readIndex, or -1
	sint32 FindNextStartCode() const
	{
		if (readIndex >= nalStreamLength)
			return -1;
		sint32 pos = readIndex;
		if (pos + 3 <= nalStreamLength && IsShortStartCode(pos))
			pos += 3;
		else if (pos + 4 <= nalStreamLength && IsLongStartCode(pos))
			pos += 4;
		for (; pos < nalStreamLength - 3; pos++)
		{
			if (IsShortStartCode(pos) || IsLongStartCode(pos))
				return pos;
		}
		return -1;
	}

	uint8* nalStreamPtr;
	sint32 nalStreamLength;
	sint32 readIndex{};
	bool errorFlag{};
};

/* parsed header data */

struct h264_scaling_matrix4x4_t
{
	uint8 isPresent;
	uint8 UseDefaultScalingMatrix;
	sint32 list[4*4];
};

struct h264_scaling_matrix8x8_t
{
	uint8 isPresent;
	uint8 UseDefaultScalingMatrix;
	sint32 list[8*8];
};

struct h264State_pic_parameter_set_t
{
	uint32 pic_parameter_set_id;
	uint32 seq_parameter_set_id;
	uint8 entropy_coding_mode_flag;
	uint8 bottom_field_pic_order_in_frame_present_flag;
	uint32 num_slice_groups_minus1;
	uint32 slice_group_map_type; // slice groups are not parsed
	uint32 slice_group_change_rate_minus1;
	uint32 num_ref_idx_l0_default_active_minus1;
	uint32 num_ref_idx_l1_default_active_minus1;
	uint8 weighted_pred_flag;
	uint8 weighted_bipred_idc;
	sint32 pic_init_qp_minus26;
	sint32 pic_init_qs_minus26;
	sint32 chroma_qp_index_offset;
	uint8 deblocking_filter_control_present_flag;
	uint8 constrained_intra_pred_flag;
	uint8 redundant_pic_cnt_present_flag;
	uint8 transform_8x8_mode_flag;
	uint8 pic_scaling_matrix_present_flag;
	h264_scaling_matrix4x4_t ScalingList4x4[6];
	h264_scaling_matrix8x8_t ScalingList8x8[6];
	sint32 second_chroma_qp_index_offset;
};

struct h264State_seq_parameter_set_t
{
	uint8 profile_idc;
	uint8 constraint; // constraint_set flags and reserved bits
	uint8 level_idc;
	uint32 seq_parameter_set_id; // not stored by the parser
	uint32 chroma_format_idc;
	uint8 separate_colour_plane_flag;
	uint32 bit_depth_luma_minus8;
	uint32 bit_depth_chroma_minus8;
	uint8 qpprime_y_zero_transform_bypass_flag;
	uint8 seq_scaling_matrix_present_flag;
	h264_scaling_matrix4x4_t ScalingList4x4[6];
	h264_scaling_matrix8x8_t ScalingList8x8[6];
	uint32 log2_max_frame_num_minus4;
	uint32 pic_order_cnt_type;
	uint32 log2_max_pic_order_cnt_lsb_minus4; // pic_order_cnt_type 0 only
	uint32 num_ref_frames;
	uint32 gaps_in_frame_num_value_allowed_flag;
	uint32 pic_width_in_mbs_minus1;
	uint32 pic_height_in_map_units_minus1;
	uint8 frame_mbs_only_flag;
	uint8 mb_adaptive_frame_field_flag;
	uint8 direct_8x8_inference_flag;
	uint8 frame_cropping_flag;
	uint32 frame_crop_left_offset;
	uint32 frame_crop_right_offset;
	uint32 frame_crop_top_offset;
	uint32 frame_crop_bottom_offset;

	uint32 getMaxFrameNum() const
	{
		return 1 << (log2_max_frame_num_minus4 + 4);
	}
};

// slice_type values 0-4 and 5-9 name the same types
struct H264SliceType
{
	H264SliceType() {};
	H264SliceType(uint32 slice_type) : m_sliceType(slice_type) {};

	bool isSliceTypeP() const { return Kind() == 0; };
	bool isSliceTypeB() const { return Kind() == 1; };
	bool isSliceTypeI() const { return Kind() == 2; };
	bool isSliceTypeSP() const { return Kind() == 3; };
	bool isSliceTypeSI() const { return Kind() == 4; };

private:
	uint32 Kind() const { return m_sliceType % 5; }
	uint32 m_sliceType{};
};

typedef struct
{
	uint8 nal_ref_idc;
	uint8 nal_unit_type;
	uint32 first_mb_in_slice;
	H264SliceType slice_type;
	uint32 pic_parameter_set_id;
	uint32 frame_num;
	uint8 field_pic_flag;
	uint8 bottom_field_flag;
	uint32 idr_pic_id;
	uint32 pic_order_cnt_lsb;
	sint32 delta_pic_order_cnt_bottom;
	uint32 redundant_pic_cnt;
	uint8 direct_spatial_mv_pred_flag; // B slices
	uint8 num_ref_idx_active_override_flag; // P, SP and B slices
	uint32 num_ref_idx_l0_active_minus1;
	uint32 num_ref_idx_l1_active_minus1;

	// ref_pic_list_modification for list 0 and list 1
	struct
	{
		uint8 type;
		uint32 abs_diff_pic_num_minus1;
		uint32 long_term_pic_num;
	}pic_list_modification0Array[32];
	sint32 pic_list_modification0Count;
	struct
	{
		uint8 type;
		uint32 abs_diff_pic_num_minus1;
		uint32 long_term_pic_num;
	}pic_list_modification1Array[32];
	sint32 pic_list_modification1Count;

	// dec_ref_pic_marking
	enum
	{
		MEMOP_END = 0,
		MEMOP_REMOVE_REF_FROM_SHORT_TERM = 1,
		MEMOP_REMOVE_REF_FROM_LONG_TERM = 2,
		MEMOP_MAKE_LONG_TERM_REF = 3,
		MEMOP_MAX_LONG_TERM_INDEX = 4,
		MEMOP_REMOVE_ALL_REFS = 5,
		MEMOP_MAKE_CURRENT_LONG_TERM_REF = 6
	};
	uint8 adaptive_ref_pic_marking_mode_flag;
	struct
	{
		uint8 op;
		uint32 difference_of_pic_nums_minus1;
		uint32 long_term_pic_num;
		uint32 long_term_frame_idx;
		uint32 max_long_term_frame_idx_plus1;
	}memory_management_control_operation[16];
	sint32 memory_management_control_operation_num;

	uint8 IdrPicFlag;
	struct
	{
		uint32 TopFieldOrderCnt;
	}calculated;
}nal_slice_header_t;

typedef struct
{
	sint32 streamSubOffset;
	sint32 streamSubSize;
	nal_slice_header_t header;
}nal_slice_info_t;

// result of one h264Parse call
typedef struct
{
	bool hasSPS;
	bool hasPPS;
	sint32 sliceCount;
	std::array<nal_slice_info_t, 32> sliceInfo;
}h264ParserOutput_t;

typedef struct
{
	nal_slice_header_t slice_header;
}nal_slice_t;

// state carried between h264Parse calls: the last SPS/PPS and picture order counters
typedef struct
{
	bool hasSPS;
	bool hasPPS;
	h264State_seq_parameter_set_t sps;
	h264State_pic_parameter_set_t pps;
	struct
	{
		uint32 prevPicOrderCntMsb;
		uint32 prevPicOrderCntLsb;
		uint32 prevFrameNum;
		uint32 prevFrameNumOffset;
	}picture_order;
}h264ParserState_t;

void h264Parse(h264ParserState_t* h264ParserState, h264ParserOutput_t* output, uint8* data, uint32 length, bool parseSlices = true);
sint32 h264GetUnitLength(h264ParserState_t* h264ParserState, uint8* data, uint32 length);

bool h264Parser_ParseSPS(uint8* data, uint32 length, h264State_seq_parameter_set_t& sps);

void h264Parser_getScalingMatrix4x4(h264State_seq_parameter_set_t* sps, h264State_pic_parameter_set_t* pps, nal_slice_header_t* sliceHeader, sint32 index, uint8* matrix4x4);
void h264Parser_getScalingMatrix8x8(h264State_seq_parameter_set_t* sps, h264State_pic_parameter_set_t* pps, nal_slice_header_t* sliceHeader, sint32 index, uint8* matrix8x8);

// output frames are NV12 with rows padded to 256 bytes
static sint32 h264GetFramePitch(sint32 width)
{
	return (width + 0xFF) & ~0xFF;
}

static sint32 h264GetFrameSize(sint32 width, sint32 height)
{
	const sint32 pitch = h264GetFramePitch(width);
	return pitch * height + pitch * (height / 2);
}
