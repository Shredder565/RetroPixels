// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Header level H.264 parsing (ITU-T H.264 7.3). Only the fields needed to locate pictures and
// compute their order are kept; everything else is read and discarded.
// Unsupported stream features (pic_order_cnt_type 1 slices, 4:4:4 scaling lists, explicit
// scaling list lookups) stop the emulator with an assert, as before.
#include "Cafe/OS/libs/h264_avc/parser/H264Parser.h"

namespace
{
	using Reader = RBSPInputBitstream;

	// profiles whose SPS carries the chroma format / bit depth / scaling list block
	bool HasHighProfileFields(uint8 profile)
	{
		static constexpr uint8 kProfiles[] = { 100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135 };
		return std::find(std::begin(kProfiles), std::end(kProfiles), profile) != std::end(kProfiles);
	}

	// scaling_list(): 'size' entries, delta coded. 'list' and the default flag are only written when present
	template<typename TMatrix, size_t TSize>
	void ReadScalingList(Reader& r, TMatrix& m)
	{
		m.isPresent = r.readBit();
		if (!m.isPresent)
			return;
		cemu_assert_debug(false); // untested
		sint32 last = 8;
		sint32 next = 8;
		for (size_t j = 0; j < TSize; j++)
		{
			if (next != 0)
			{
				next = (last + r.readSV_E() + 256) % 256;
				m.UseDefaultScalingMatrix = (j == 0 && next == 0);
			}
			m.list[j] = next == 0 ? last : next;
			last = m.list[j];
		}
	}

	void Read4x4(Reader& r, h264_scaling_matrix4x4_t& m) { ReadScalingList<h264_scaling_matrix4x4_t, 16>(r, m); }
	void Read8x8(Reader& r, h264_scaling_matrix8x8_t& m) { ReadScalingList<h264_scaling_matrix8x8_t, 64>(r, m); }

	// hrd_parameters(); nothing is kept. The counter is 8 bits wide, as in the original parser
	void SkipHrd(Reader& r)
	{
		const uint32 cpbCount = r.readUV_E();
		r.readBits(4 + 4); // bit_rate_scale, cpb_size_scale
		for (uint8 i = 0; i <= cpbCount; i++)
		{
			r.readUV_E(); // bit_rate_value_minus1
			r.readUV_E(); // cpb_size_value_minus1
			r.readBit(); // cbr_flag
		}
		r.readBits(5 * 4); // the four delay / offset lengths
	}

	// vui_parameters(); nothing is kept
	void SkipVui(Reader& r)
	{
		if (r.readBit()) // aspect_ratio_info_present_flag
		{
			if (r.readBits(8) == 255) // Extended_SAR
				r.readBits(32); // sar_width, sar_height
		}
		if (r.readBit()) // overscan_info_present_flag
			cemu_assert_debug(false); // overscan_appropriate_flag is not consumed
		if (r.readBit()) // video_signal_type_present_flag
		{
			r.readBits(3 + 1); // video_format, video_full_range_flag
			if (r.readBit()) // colour_description_present_flag
				r.readBits(24);
		}
		if (r.readBit()) // chroma_loc_info_present_flag
		{
			r.readUV_E();
			r.readUV_E();
		}
		if (r.readBit()) // timing_info_present_flag
		{
			r.readBits(32); // num_units_in_tick
			r.readBits(32); // time_scale
			r.readBit(); // fixed_frame_rate_flag
		}
		const bool nalHrd = r.readBit() != 0;
		if (nalHrd)
			SkipHrd(r);
		const bool vclHrd = r.readBit() != 0;
		if (vclHrd)
			SkipHrd(r);
		if (nalHrd || vclHrd)
			r.readBit(); // low_delay_hrd_flag
		r.readBit(); // pic_struct_present_flag
		if (r.readBit()) // bitstream_restriction_flag
		{
			r.readBit();
			for (int i = 0; i < 6; i++)
				r.readUV_E();
		}
	}

	// seq_parameter_set_rbsp(). The SPS counts as received only when the trailing bits are valid
	bool ParseSps(h264ParserState_t& state, h264ParserOutput_t* output, Reader& r)
	{
		h264State_seq_parameter_set_t& sps = state.sps;
		memset(&sps, 0, sizeof(sps));
		sps.profile_idc = r.readU8();
		sps.constraint = r.readU8();
		sps.level_idc = r.readU8();
		sps.chroma_format_idc = 1; // 4:2:0 when not signalled
		r.readUV_E(); // seq_parameter_set_id (only one SPS is tracked)
		if (HasHighProfileFields(sps.profile_idc))
		{
			sps.chroma_format_idc = r.readUV_E();
			if (sps.chroma_format_idc == 3)
				sps.separate_colour_plane_flag = r.readBit();
			sps.bit_depth_luma_minus8 = r.readUV_E();
			sps.bit_depth_chroma_minus8 = r.readUV_E();
			sps.qpprime_y_zero_transform_bypass_flag = r.readBit();
			sps.seq_scaling_matrix_present_flag = r.readBit();
			if (sps.seq_scaling_matrix_present_flag)
			{
				for (auto& m : sps.ScalingList4x4)
					Read4x4(r, m);
				Read8x8(r, sps.ScalingList8x8[0]);
				Read8x8(r, sps.ScalingList8x8[1]);
				if (sps.chroma_format_idc == 3)
					cemu_assert(false); // 4:4:4 needs six 8x8 lists
			}
		}
		sps.log2_max_frame_num_minus4 = r.readUV_E();
		sps.pic_order_cnt_type = r.readUV_E();
		if (sps.pic_order_cnt_type == 0)
			sps.log2_max_pic_order_cnt_lsb_minus4 = r.readUV_E();
		else if (sps.pic_order_cnt_type != 2)
			cemu_assert_debug(false); // type 1 fields are not read
		sps.num_ref_frames = r.readUV_E();
		sps.gaps_in_frame_num_value_allowed_flag = r.readBit();
		sps.pic_width_in_mbs_minus1 = r.readUV_E();
		sps.pic_height_in_map_units_minus1 = r.readUV_E();
		sps.frame_mbs_only_flag = r.readBit();
		sps.mb_adaptive_frame_field_flag = sps.frame_mbs_only_flag ? 0 : r.readBit();
		cemu_assert_debug(sps.frame_mbs_only_flag);
		sps.direct_8x8_inference_flag = r.readBit();
		sps.frame_cropping_flag = r.readBit();
		if (sps.frame_cropping_flag)
		{
			sps.frame_crop_left_offset = r.readUV_E();
			sps.frame_crop_right_offset = r.readUV_E();
			sps.frame_crop_top_offset = r.readUV_E();
			sps.frame_crop_bottom_offset = r.readUV_E();
		}
		if (r.readBit()) // vui_parameters_present_flag
			SkipVui(r);
		if (r.readTrailingRBSPBits())
		{
			state.hasSPS = true;
			if (output)
				output->hasSPS = true;
		}
		return true;
	}

	// pic_parameter_set_rbsp(). Slice groups are not supported and abort the PPS early
	bool ParsePps(h264ParserState_t& state, h264ParserOutput_t* output, Reader& r)
	{
		h264State_pic_parameter_set_t& pps = state.pps;
		memset(&pps, 0, sizeof(pps));
		pps.pic_parameter_set_id = r.readUV_E();
		pps.seq_parameter_set_id = r.readUV_E();
		pps.entropy_coding_mode_flag = r.readBit();
		pps.bottom_field_pic_order_in_frame_present_flag = r.readBit();
		pps.num_slice_groups_minus1 = r.readUV_E();
		if (pps.num_slice_groups_minus1 != 0)
			return false;
		pps.num_ref_idx_l0_default_active_minus1 = r.readUV_E();
		pps.num_ref_idx_l1_default_active_minus1 = r.readUV_E();
		pps.weighted_pred_flag = r.readBit();
		pps.weighted_bipred_idc = r.readBits(2);
		pps.pic_init_qp_minus26 = r.readSV_E();
		pps.pic_init_qs_minus26 = r.readSV_E();
		pps.chroma_qp_index_offset = r.readSV_E();
		pps.deblocking_filter_control_present_flag = r.readBit();
		pps.constrained_intra_pred_flag = r.readBit();
		pps.redundant_pic_cnt_present_flag = r.readBit();
		if (r.more_rbsp_data())
		{
			pps.transform_8x8_mode_flag = r.readBit();
			pps.pic_scaling_matrix_present_flag = r.readBit();
			if (pps.pic_scaling_matrix_present_flag)
			{
				for (auto& m : pps.ScalingList4x4)
					Read4x4(r, m);
				if (pps.transform_8x8_mode_flag)
				{
					Read8x8(r, pps.ScalingList8x8[0]);
					Read8x8(r, pps.ScalingList8x8[1]);
					if (state.sps.chroma_format_idc == 3)
						cemu_assert(false);
				}
			}
			pps.second_chroma_qp_index_offset = r.readSV_E();
		}
		if (r.readTrailingRBSPBits())
		{
			state.hasPPS = true;
			if (output)
				output->hasPPS = true;
		}
		return true;
	}

	// ref_pic_list_modification(). Returning early also skips the list 1 part
	void ReadListModification(Reader& r, nal_slice_header_t& h)
	{
		if (!h.slice_type.isSliceTypeI() && !h.slice_type.isSliceTypeSI() && r.readBit())
		{
			auto& count = h.pic_list_modification0Count;
			for (count = 0;; count++)
			{
				if (count >= 32)
					return;
				auto& entry = h.pic_list_modification0Array[count];
				const uint32 idc = r.readUV_E();
				entry.type = idc;
				if (idc == 3)
					break;
				if (idc > 3)
					return;
				if (idc == 2)
					entry.long_term_pic_num = r.readUV_E();
				else
					entry.abs_diff_pic_num_minus1 = r.readUV_E();
			}
		}
		if (h.slice_type.isSliceTypeB() && r.readBit())
		{
			cemu_assert_debug(false); // untested
			// list 1 entries all land in slot 0 and are counted in the list 0 counter; the list 1
			// counter stays zero (behaviour kept from the original parser)
			auto& entry = h.pic_list_modification1Array[h.pic_list_modification1Count];
			while (true)
			{
				const uint32 idc = r.readUV_E();
				entry.type = idc;
				if (idc >= 3)
					break;
				if (idc == 2)
					entry.long_term_pic_num = r.readUV_E();
				else
					entry.abs_diff_pic_num_minus1 = r.readUV_E();
				h.pic_list_modification0Count++;
			}
		}
	}

	// pred_weight_table(); nothing is kept. For list 1 only the luma flags are consumed
	void SkipWeightTable(const h264State_seq_parameter_set_t& sps, Reader& r, const nal_slice_header_t& h)
	{
		r.readUV_E(); // luma_log2_weight_denom
		const uint32 chromaArrayType = sps.separate_colour_plane_flag ? 0 : sps.chroma_format_idc;
		if (chromaArrayType != 0)
			r.readUV_E(); // chroma_log2_weight_denom
		for (uint32 i = 0; i <= h.num_ref_idx_l0_active_minus1; i++)
		{
			if (r.readBit())
			{
				r.readSV_E();
				r.readSV_E();
			}
			if (chromaArrayType != 0 && r.readBit())
			{
				for (int k = 0; k < 4; k++)
					r.readSV_E();
			}
		}
		if (!h.slice_type.isSliceTypeB())
			return;
		for (uint32 i = 0; i <= h.num_ref_idx_l1_active_minus1; i++)
		{
			if (r.readBit())
				cemu_assert_debug(false);
			if (chromaArrayType != 0)
				cemu_assert_debug(false);
		}
	}

	// dec_ref_pic_marking()
	void ReadRefPicMarking(Reader& r, nal_slice_header_t& h)
	{
		h.memory_management_control_operation_num = 0;
		if (h.IdrPicFlag)
		{
			r.readBit(); // no_output_of_prior_pics_flag
			r.readBit(); // long_term_reference_flag
			h.adaptive_ref_pic_marking_mode_flag = 1;
			return;
		}
		h.adaptive_ref_pic_marking_mode_flag = r.readBit();
		if (!h.adaptive_ref_pic_marking_mode_flag)
			return;
		while (true)
		{
			const uint32 op = r.readUV_E();
			if (op == nal_slice_header_t::MEMOP_END)
				return;
			sint32& n = h.memory_management_control_operation_num;
			if (n >= 16)
				return;
			auto& e = h.memory_management_control_operation[n];
			e.op = op;
			switch (op)
			{
			case nal_slice_header_t::MEMOP_REMOVE_REF_FROM_SHORT_TERM:
				e.difference_of_pic_nums_minus1 = r.readUV_E();
				break;
			case nal_slice_header_t::MEMOP_REMOVE_REF_FROM_LONG_TERM:
				e.long_term_pic_num = r.readUV_E();
				break;
			case nal_slice_header_t::MEMOP_MAKE_LONG_TERM_REF:
				e.difference_of_pic_nums_minus1 = r.readUV_E();
				e.long_term_frame_idx = r.readUV_E();
				break;
			case nal_slice_header_t::MEMOP_MAX_LONG_TERM_INDEX:
				e.max_long_term_frame_idx_plus1 = r.readUV_E();
				break;
			case nal_slice_header_t::MEMOP_MAKE_CURRENT_LONG_TERM_REF:
				e.long_term_frame_idx = r.readUV_E();
				break;
			default:
				break;
			}
			n++;
		}
	}

	// slice_header()
	void ReadSliceHeader(const h264State_seq_parameter_set_t& sps, const h264State_pic_parameter_set_t& pps, Reader& r, uint8 nalType, uint8 nalRefIdc, nal_slice_header_t& h)
	{
		memset(&h, 0, sizeof(h));
		h.nal_ref_idc = nalRefIdc;
		h.nal_unit_type = nalType;
		h.first_mb_in_slice = r.readUV_E();
		h.slice_type = H264SliceType(r.readUV_E());
		h.pic_parameter_set_id = r.readUV_E();
		if (sps.separate_colour_plane_flag)
			return; // colour_plane_id is not supported
		h.frame_num = r.readBits(sps.log2_max_frame_num_minus4 + 4);
		if (!sps.frame_mbs_only_flag)
		{
			h.field_pic_flag = r.readBit();
			if (h.field_pic_flag)
				h.bottom_field_flag = r.readBit();
		}
		const bool isIdr = nalType == 5;
		h.IdrPicFlag = isIdr ? 1 : 0;
		if (isIdr)
			h.idr_pic_id = r.readUV_E();
		if (sps.pic_order_cnt_type == 0)
		{
			h.pic_order_cnt_lsb = r.readBits(sps.log2_max_pic_order_cnt_lsb_minus4 + 4);
			if (pps.bottom_field_pic_order_in_frame_present_flag && !h.field_pic_flag)
				h.delta_pic_order_cnt_bottom = r.readSV_E();
		}
		else if (sps.pic_order_cnt_type == 1)
			cemu_assert(false); // delta_pic_order_cnt[] not supported
		if (pps.redundant_pic_cnt_present_flag)
			h.redundant_pic_cnt = r.readUV_E();
		const H264SliceType type = h.slice_type;
		if (type.isSliceTypeB())
			h.direct_spatial_mv_pred_flag = r.readBit();
		h.num_ref_idx_l0_active_minus1 = pps.num_ref_idx_l0_default_active_minus1;
		h.num_ref_idx_l1_active_minus1 = pps.num_ref_idx_l1_default_active_minus1;
		if (type.isSliceTypeP() || type.isSliceTypeSP() || type.isSliceTypeB())
		{
			h.num_ref_idx_active_override_flag = r.readBit();
			if (h.num_ref_idx_active_override_flag)
			{
				h.num_ref_idx_l0_active_minus1 = r.readUV_E();
				if (type.isSliceTypeB())
					h.num_ref_idx_l1_active_minus1 = r.readUV_E();
			}
		}
		if (nalType != 20 && nalType != 21) // MVC lists are not supported
			ReadListModification(r, h);
		const bool explicitWeights = (pps.weighted_pred_flag && (type.isSliceTypeP() || type.isSliceTypeSP())) || (pps.weighted_bipred_idc == 1 && type.isSliceTypeB());
		if (explicitWeights)
			SkipWeightTable(sps, r, h);
		if (h.nal_ref_idc != 0)
			ReadRefPicMarking(r, h);
		// the remaining fields are read for completeness only
		if (pps.entropy_coding_mode_flag && !type.isSliceTypeI() && !type.isSliceTypeSI())
			r.readUV_E(); // cabac_init_idc
		r.readSV_E(); // slice_qp_delta
		if (type.isSliceTypeSP() || type.isSliceTypeSI())
		{
			if (type.isSliceTypeSP())
				r.readBit(); // sp_for_switch_flag
			r.readSV_E(); // slice_qs_delta
		}
		if (pps.deblocking_filter_control_present_flag && r.readUV_E() != 1)
		{
			r.readSV_E(); // slice_alpha_c0_offset_div2
			r.readSV_E(); // slice_beta_offset_div2
		}
	}

	// picture order count (8.2.1) for pic_order_cnt_type 0 and 2; only TopFieldOrderCnt is kept
	void ComputePictureOrder(h264ParserState_t& state, const h264State_seq_parameter_set_t& sps, nal_slice_header_t& h)
	{
		auto& po = state.picture_order;
		if (sps.pic_order_cnt_type == 0)
		{
			const uint32 prevMsb = h.IdrPicFlag ? 0 : po.prevPicOrderCntMsb;
			const uint32 prevLsb = h.IdrPicFlag ? 0 : po.prevPicOrderCntLsb;
			const uint32 maxLsb = 1 << (sps.log2_max_pic_order_cnt_lsb_minus4 + 4);
			const uint32 lsb = h.pic_order_cnt_lsb;
			const sint32 halfRange = (sint32)(maxLsb / 2);
			uint32 msb = prevMsb;
			if (lsb < prevLsb && (sint32)prevLsb - (sint32)lsb >= halfRange)
				msb = prevMsb + maxLsb; // lsb wrapped forwards
			else if (lsb > prevLsb && (sint32)lsb - (sint32)prevLsb > halfRange)
				msb = prevMsb - maxLsb;
			const uint32 top = msb + lsb;
			if (h.IdrPicFlag && top != 0)
				cemu_assert(false);
			h.calculated.TopFieldOrderCnt = top;
			po.prevPicOrderCntMsb = msb;
			po.prevPicOrderCntLsb = lsb;
		}
		else if (sps.pic_order_cnt_type == 2)
		{
			uint32 frameNumOffset = 0;
			if (!h.IdrPicFlag)
			{
				frameNumOffset = po.prevFrameNumOffset;
				if (po.prevFrameNum > h.frame_num)
					frameNumOffset += sps.getMaxFrameNum(); // frame_num wrapped
			}
			uint32 order = 0;
			if (!h.IdrPicFlag)
				order = 2 * (frameNumOffset + h.frame_num) - (h.nal_ref_idc == 0 ? 1 : 0);
			// field pictures are not supported: a bottom field reports 0
			cemu_assert_debug(!h.field_pic_flag);
			h.calculated.TopFieldOrderCnt = (h.field_pic_flag && h.bottom_field_flag) ? 0 : order;
			po.prevFrameNum = h.frame_num;
			po.prevFrameNumOffset = frameNumOffset;
		}
		else
			cemu_assert_debug(false);
	}

	void ParseSlice(h264ParserState_t& state, h264ParserOutput_t* output, sint32 offset, sint32 size, Reader& r, uint8 nalRefIdc, uint8 nalType)
	{
		cemu_assert_debug(state.hasPPS && state.hasSPS);
		nal_slice_t slice = {};
		ReadSliceHeader(state.sps, state.pps, r, nalType, nalRefIdc, slice.slice_header);
		ComputePictureOrder(state, state.sps, slice.slice_header);
		if (output->sliceCount >= (sint32)output->sliceInfo.size())
			return; // slice table full
		nal_slice_info_t& info = output->sliceInfo[output->sliceCount++];
		info.header = slice.slice_header;
		info.streamSubOffset = offset;
		info.streamSubSize = size;
	}

	struct NalHeader
	{
		bool valid; // forbidden_zero_bit is clear
		uint8 refIdc;
		uint8 type;
	};

	NalHeader ReadNalHeader(Reader& r)
	{
		const uint8 b = r.readU8();
		return { (b & 0x80) == 0, (uint8)((b >> 5) & 3), (uint8)(b & 0x1F) };
	}

	// offset of the first 00 00 01 in data, or -1
	sint32 FindFirstStartCode(const uint8* data, uint32 length)
	{
		const sint32 limit = (sint32)(length - 3);
		for (sint32 i = 0; i < limit; i++)
		{
			if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
				return i;
		}
		return -1;
	}

	const uint8 kDefault4x4Intra[16] = { 6, 13, 13, 20, 20, 20, 28, 28, 28, 28, 32, 32, 32, 37, 37, 42 };
	const uint8 kDefault4x4Inter[16] = { 10, 14, 14, 20, 20, 20, 24, 24, 24, 24, 27, 27, 27, 30, 30, 34 };
	const uint8 kDefault8x8Intra[64] =
	{
		6, 10, 10, 13, 11, 13, 16, 16, 16, 16, 18, 18, 18, 18, 18, 23,
		23, 23, 23, 23, 23, 25, 25, 25, 25, 25, 25, 25, 27, 27, 27, 27,
		27, 27, 27, 27, 29, 29, 29, 29, 29, 29, 29, 31, 31, 31, 31, 31,
		31, 33, 33, 33, 33, 33, 36, 36, 36, 36, 38, 38, 38, 40, 40, 42
	};
	const uint8 kDefault8x8Inter[64] =
	{
		9, 13, 13, 15, 13, 15, 17, 17, 17, 17, 19, 19, 19, 19, 19, 21,
		21, 21, 21, 21, 21, 22, 22, 22, 22, 22, 22, 22, 24, 24, 24, 24,
		24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 27, 27, 27, 27, 27,
		27, 28, 28, 28, 28, 28, 30, 30, 30, 30, 32, 32, 32, 33, 33, 35
	};

	// Table 7-2 fall-back rule A: the PPS lists win over the SPS lists, which win over flat 16.
	// Explicitly coded lists are not supported (the copy below only runs after the assert).
	template<typename TMatrix>
	void SelectScalingMatrix(bool ppsHasLists, const TMatrix& ppsList, bool spsHasLists, const TMatrix& spsList, const uint8* defaultList, size_t count, uint8* out)
	{
		const TMatrix* source = ppsHasLists ? &ppsList : (spsHasLists ? &spsList : nullptr);
		if (!source)
		{
			memset(out, 16, count);
			return;
		}
		if (source->isPresent)
		{
			cemu_assert(false);
			memcpy(out, source->list, count);
			return;
		}
		memcpy(out, defaultList, count);
	}
}

bool h264Parser_ParseSPS(uint8* data, uint32 length, h264State_seq_parameter_set_t& sps)
{
	h264ParserState_t state{};
	Reader r(data, length);
	if (!ParseSps(state, nullptr, r) || !state.hasSPS)
		return false;
	sps = state.sps;
	return true;
}

void h264Parse(h264ParserState_t* h264ParserState, h264ParserOutput_t* output, uint8* data, uint32 length, bool parseSlices)
{
	memset(output, 0, sizeof(h264ParserOutput_t));
	NALInputBitstream nal(data, length);
	Reader r;
	while (!nal.isEndOfStream() && nal.getNextRBSP(r))
	{
		const sint32 offset = (sint32)(r.getBasePtr() - data);
		const sint32 size = r.getBaseLength();
		const NalHeader hdr = ReadNalHeader(r);
		if (!hdr.valid)
			continue;
		switch (hdr.type)
		{
		case 1: // non-IDR slice
		case 5: // IDR slice
			if (parseSlices)
				ParseSlice(*h264ParserState, output, offset, size, r, hdr.refIdc, hdr.type);
			break;
		case 7:
			ParseSps(*h264ParserState, output, r);
			break;
		case 8:
			ParsePps(*h264ParserState, output, r);
			break;
		case 6: // SEI
		case 9: // access unit delimiter
		case 10: // end of sequence
		case 12: // filler data
			break;
		default:
			cemuLog_logDebug(LogType::Force, "H264 parser: NAL unit type {} ignored", hdr.type);
			break;
		}
	}
}

sint32 h264GetUnitLength(h264ParserState_t* h264ParserState, uint8* data, uint32 length)
{
	// the NAL walk covers the whole buffer; the start code offset only feeds the result
	NALInputBitstream nal(data, length);
	const sint32 startCode = FindFirstStartCode(data, length);
	if (startCode < 0)
		return -1;
	const uint8* unitStart = data + startCode;
	Reader r;
	while (!nal.isEndOfStream() && nal.getNextRBSP(r, true))
	{
		const NalHeader hdr = ReadNalHeader(r);
		if (!hdr.valid || hdr.type == 14 || hdr.type == 20 || hdr.type == 21)
			continue;
		switch (hdr.type)
		{
		case 1:
		case 5:
			// one slice per picture is assumed; the unit ends with the first slice
			return (sint32)(r.getBasePtr() + r.getBaseLength() - unitStart) + startCode;
		case 7:
			ParseSps(*h264ParserState, nullptr, r);
			break;
		case 8:
			ParsePps(*h264ParserState, nullptr, r);
			break;
		case 6:
		case 9:
		case 10:
			break;
		default:
			cemuLog_logDebug(LogType::Force, "H264 parser: NAL unit type {} not handled", hdr.type);
			assert_dbg();
			break;
		}
	}
	return -1;
}

void h264Parser_getScalingMatrix4x4(h264State_seq_parameter_set_t* sps, h264State_pic_parameter_set_t* pps, nal_slice_header_t* sliceHeader, sint32 index, uint8* matrix4x4)
{
	SelectScalingMatrix(pps->pic_scaling_matrix_present_flag, pps->ScalingList4x4[index], sps->seq_scaling_matrix_present_flag, sps->ScalingList4x4[index],
		index < 3 ? kDefault4x4Intra : kDefault4x4Inter, 16, matrix4x4);
}

void h264Parser_getScalingMatrix8x8(h264State_seq_parameter_set_t* sps, h264State_pic_parameter_set_t* pps, nal_slice_header_t* sliceHeader, sint32 index, uint8* matrix8x8)
{
	SelectScalingMatrix(pps->pic_scaling_matrix_present_flag, pps->ScalingList8x8[index], sps->seq_scaling_matrix_present_flag, sps->ScalingList8x8[index],
		(index & 1) == 0 ? kDefault8x8Intra : kDefault8x8Inter, 64, matrix8x8);
}
