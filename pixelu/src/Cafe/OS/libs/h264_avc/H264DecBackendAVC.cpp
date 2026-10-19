// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Decoder backend on top of libavc's ih264d (dependencies/ih264d).
//
// ih264d runs in shared display buffer mode: we hand it a set of NV12 frame buffers, it reports
// which one holds the next frame in display order, and we copy that frame into the guest buffer
// and give the buffer back. The first picture after Init (or after a resolution change) is first
// run in header-only mode to learn the buffer sizes.
#include "H264DecInternal.h"
#include "util/highresolutiontimer/HighResolutionTimer.h"

extern "C"
{
#include "../dependencies/ih264d/common/ih264_typedefs.h"
#include "../dependencies/ih264d/decoder/ih264d.h"
};

namespace H264
{
	bool H264_IsBotW();

	namespace
	{
		void* AlignedAlloc(void* ctxt, WORD32 alignment, WORD32 size)
		{
#ifdef _WIN32
			return _aligned_malloc(size, alignment);
#else
			// posix_memalign wants a power of two that is at least the pointer size
			size_t align = sizeof(void*);
			while (align < (size_t)alignment)
				align <<= 1;
			void* p = nullptr;
			if (posix_memalign(&p, align, (size_t)size) != 0)
				return nullptr;
			return p;
#endif
		}

		void AlignedFree(void* ctxt, void* buf)
		{
#ifdef _WIN32
			_aligned_free(buf);
#else
			free(buf);
#endif
		}

		// zero initialized ih264d argument with its size field set
		template<typename T>
		T ApiStruct()
		{
			T s{};
			memset(&s, 0, sizeof(T));
			s.u4_size = sizeof(T);
			return s;
		}

		// BotW plays 1920x1080 videos stored as 1920x1088; the guest expects the cropped height
		void ApplyBotWCrop(ivd_video_decode_op_t& op)
		{
			if (H264_IsBotW() && op.s_disp_frm_buf.u4_y_wd == 1920 && op.s_disp_frm_buf.u4_y_ht == 1088)
				op.s_disp_frm_buf.u4_y_ht = 1080;
		}
	}

	class H264AVCDecoder : public H264DecoderBackend
	{
	public:
		H264AVCDecoder()
		{
			m_decoderThread = std::thread(&H264AVCDecoder::WorkerMain, this);
		}

		~H264AVCDecoder()
		{
			m_threadShouldExit = true;
			m_decodeSem.increment();
			if (m_decoderThread.joinable())
				m_decoderThread.join();
		}

		void Init(bool isBufferedMode) override
		{
			ih264d_create_ip_t createIn{};
			ih264d_create_op_t createOut{};
			ivd_create_ip_t& in = createIn.s_ivd_create_ip_t;
			in.u4_size = sizeof(ih264d_create_ip_t);
			in.e_cmd = IVD_CMD_CREATE;
			in.u4_share_disp_buf = 1;
			in.e_output_format = IV_YUV_420SP_UV;
			in.pf_aligned_alloc = AlignedAlloc;
			in.pf_aligned_free = AlignedFree;
			in.pv_mem_ctxt = nullptr;
			createOut.s_ivd_create_op_t.u4_size = sizeof(ih264d_create_op_t);
			cemu_assert(Call(&createIn, &createOut) == 0);
			m_codecCtx = (iv_obj_t*)createOut.s_ivd_create_op_t.pv_handle;
			m_codecCtx->pv_fxns = (void*)&ih264d_api_function;
			m_codecCtx->u4_size = sizeof(iv_obj_t);

			ih264d_ctl_set_num_cores_ip_t coresIn = ApiStruct<ih264d_ctl_set_num_cores_ip_t>();
			ih264d_ctl_set_num_cores_op_t coresOut = ApiStruct<ih264d_ctl_set_num_cores_op_t>();
			coresIn.e_cmd = IVD_CMD_VIDEO_CTL;
			coresIn.e_sub_cmd = (IVD_CONTROL_API_COMMAND_TYPE_T)IH264D_CMD_CTL_SET_NUM_CORES;
			coresIn.u4_num_cores = 1;
			cemu_assert(Call(&coresIn, &coresOut) == IV_SUCCESS);

			m_isBufferedMode = isBufferedMode;
			SetDecodeMode(false);
			m_numDecodedFrames = 0;
			m_hasBufferSizeInfo = false;
		}

		void Destroy() override
		{
			if (!m_codecCtx)
				return;
			ih264d_delete_ip_t deleteIn{};
			ih264d_delete_op_t deleteOut{};
			deleteIn.s_ivd_delete_ip_t.u4_size = sizeof(ih264d_delete_ip_t);
			deleteIn.s_ivd_delete_ip_t.e_cmd = IVD_CMD_DELETE;
			deleteOut.s_ivd_delete_op_t.u4_size = sizeof(ih264d_delete_op_t);
			const WORD32 status = Call(&deleteIn, &deleteOut);
			cemu_assert_debug(status == 0);
			m_codecCtx = nullptr;
		}

	private:
		WORD32 Call(void* in, void* out)
		{
			return ih264d_api_function(m_codecCtx, in, out);
		}

		// decode call; data == nullptr drains frames in flush mode
		WORD32 DecodeCall(void* data, uint32 length, uint32 tag, ivd_video_decode_op_t& out)
		{
			ivd_video_decode_ip_t in = ApiStruct<ivd_video_decode_ip_t>();
			out = ApiStruct<ivd_video_decode_op_t>();
			in.e_cmd = IVD_CMD_VIDEO_DECODE;
			in.u4_ts = tag;
			in.pv_stream_buffer = data;
			in.u4_num_Bytes = length;
			return Call(&in, &out);
		}

		void ReleaseDisplayBuffer(uint32 bufferId)
		{
			ivd_rel_display_frame_ip_t in = ApiStruct<ivd_rel_display_frame_ip_t>();
			ivd_rel_display_frame_op_t out = ApiStruct<ivd_rel_display_frame_op_t>();
			in.e_cmd = IVD_CMD_REL_DISPLAY_FRAME;
			in.u4_disp_buf_id = bufferId;
			cemu_assert(Call(&in, &out) == 0);
		}

		template<typename TIn, typename TOut>
		WORD32 Control(IVD_CONTROL_API_COMMAND_TYPE_T subCommand, TIn& in, TOut& out)
		{
			in.e_cmd = IVD_CMD_VIDEO_CTL;
			in.e_sub_cmd = subCommand;
			return Call(&in, &out);
		}

		// header-only decoding is used to read the stream dimensions
		void SetDecodeMode(bool headerOnly)
		{
			ih264d_ctl_set_config_ip_t in{};
			ih264d_ctl_set_config_op_t out{};
			ivd_ctl_set_config_ip_t& cfg = in.s_ivd_ctl_set_config_ip_t;
			cfg.u4_size = sizeof(ih264d_ctl_set_config_ip_t);
			cfg.u4_disp_wd = 0;
			cfg.e_frm_skip_mode = IVD_SKIP_NONE;
			// buffered mode outputs in display order, otherwise every picture is output as soon as it is decoded
			cfg.e_frm_out_mode = m_isBufferedMode ? IVD_DISPLAY_FRAME_OUT : IVD_DECODE_FRAME_OUT;
			cfg.e_vid_dec_mode = headerOnly ? IVD_DECODE_HEADER : IVD_DECODE_FRAME;
			out.s_ivd_ctl_set_config_op_t.u4_size = sizeof(ih264d_ctl_set_config_op_t);
			cemu_assert(Control(IVD_CMD_CTL_SETPARAMS, cfg, out) == 0);
		}

		// allocates the display buffers ih264d asks for and registers them as free
		void AllocateDisplayBuffers()
		{
			ivd_ctl_getbufinfo_ip_t infoIn = ApiStruct<ivd_ctl_getbufinfo_ip_t>();
			ivd_ctl_getbufinfo_op_t info = ApiStruct<ivd_ctl_getbufinfo_op_t>();
			cemu_assert(Control(IVD_CMD_CTL_GETBUFINFO, infoIn, info) == 0);
			cemu_assert_debug(info.u4_min_num_out_bufs == 2);
			cemu_assert_debug(info.u4_min_out_buf_size[0] != 0 && info.u4_min_out_buf_size[1] != 0);
			const uint32 lumaSize = info.u4_min_out_buf_size[0];
			const uint32 chromaSize = info.u4_min_out_buf_size[1];

			m_displayBuf.clear();
			m_displayBuf.resize(info.u4_num_disp_bufs, std::vector<uint8>(lumaSize + chromaSize));

			// all entries must be zero: ih264d looks at the first buffers even when fewer are passed
			ivd_set_display_frame_ip_t setIn = ApiStruct<ivd_set_display_frame_ip_t>();
			ivd_set_display_frame_op_t setOut = ApiStruct<ivd_set_display_frame_op_t>();
			setIn.e_cmd = IVD_CMD_SET_DISPLAY_FRAME;
			setIn.num_disp_bufs = info.u4_num_disp_bufs;
			for (uint32 i = 0; i < info.u4_num_disp_bufs; i++)
			{
				ivd_out_bufdesc_t& desc = setIn.s_disp_buffer[i];
				desc.u4_num_bufs = 2;
				desc.u4_min_out_buf_size[0] = lumaSize;
				desc.u4_min_out_buf_size[1] = chromaSize;
				desc.pu1_bufs[0] = m_displayBuf[i].data();
				desc.pu1_bufs[1] = m_displayBuf[i].data() + lumaSize;
			}
			cemu_assert(Call(&setIn, &setOut) == 0);
			for (uint32 i = 0; i < info.u4_num_disp_bufs; i++)
				ReleaseDisplayBuffer(i);
		}

		// decodes the headers at the start of 'data'. On success the display buffers are set up and
		// 'consumed' is the number of header bytes
		bool ReadStreamHeaders(void* data, uint32 length, uint32& consumed)
		{
			consumed = 0;
			SetDecodeMode(true);
			ivd_video_decode_op_t out;
			if (DecodeCall(data, length, 0, out) != 0)
			{
				cemuLog_log(LogType::Force, "H264: Unable to determine buffer sizes for stream");
				return false;
			}
			consumed = out.u4_num_bytes_consumed;
			if (out.u4_pic_wd == 0 || out.u4_pic_ht == 0)
				return false;
			SetDecodeMode(false);
			AllocateDisplayBuffers();
			return true;
		}

		void ResetCodec()
		{
			ivd_ctl_reset_ip_t in = ApiStruct<ivd_ctl_reset_ip_t>();
			ivd_ctl_reset_op_t out = ApiStruct<ivd_ctl_reset_op_t>();
			const WORD32 status = Control(IVD_CMD_CTL_RESET, in, out);
			cemu_assert_debug(status == 0);
		}

		// writes the frame into the guest buffer as NV12 with a 256 byte aligned pitch
		static void CopyFrame(const ivd_video_decode_op_t& op, uint8* dst)
		{
			const iv_yuv_buf_t& src = op.s_disp_frm_buf;
			const uint32 width = src.u4_y_wd;
			const uint32 height = src.u4_y_ht;
			const size_t srcPitch = src.u4_y_strd;
			const size_t dstPitch = (width + 0xFF) & ~0xFF;
			const uint8* luma = (const uint8*)src.pv_y_buf;
			const uint8* chroma = (const uint8*)src.pv_u_buf; // interleaved UV, same pitch as luma
			uint8* dstChroma = dst + dstPitch * height;
			for (uint32 y = 0; y < height; y++)
				memcpy(dst + y * dstPitch, luma + y * srcPitch, width);
			for (uint32 y = 0; y < height / 2; y++)
				memcpy(dstChroma + y * dstPitch, chroma + y * srcPitch, width);
		}

		// fills in the result of the slot the frame was tagged with and queues it for display
		void PublishFrame(const ivd_video_decode_op_t& op)
		{
			// the copy is done without the lock
			CopyFrame(op, (uint8*)m_decodedSliceArray[op.u4_ts].result.imageOutput);
			{
				std::lock_guard lock(m_decodeQueueMtx);
				cemu_assert(op.u4_ts < m_decodedSliceArray.size());
				DecodedSlice& slot = m_decodedSliceArray[op.u4_ts];
				cemu_assert_debug(slot.isUsed);
				cemu_assert_debug(op.u4_output_present != 0);
				DecodeResult& r = slot.result;
				r.isDecoded = true;
				r.hasFrame = op.u4_output_present != 0;
				r.frameWidth = op.u4_pic_wd;
				r.frameHeight = op.u4_pic_ht;
				r.bytesPerRow = (op.u4_pic_wd + 0xFF) & ~0xFF;
				r.cropEnable = op.u1_frame_cropping_flag;
				r.cropTop = op.u1_frame_cropping_rect_top_ofst;
				r.cropBottom = op.u1_frame_cropping_rect_bottom_ofst;
				r.cropLeft = op.u1_frame_cropping_rect_left_ofst;
				r.cropRight = op.u1_frame_cropping_rect_right_ofst;
				m_displayQueue.push_back(op.u4_ts);
			}
			coreinit::OSSignalEvent(m_displayQueueEvt);
		}

		sint32 DisplayBufferIndexOf(const void* luma) const
		{
			for (size_t i = 0; i < m_displayBuf.size(); i++)
			{
				const uint8* begin = m_displayBuf[i].data();
				if (luma >= begin && luma < begin + m_displayBuf[i].size())
					return (sint32)i;
			}
			return -1;
		}

		void DecodePicture(DecodedSlice& slot)
		{
			DataToDecode& input = slot.dataToDecode;
			if (!m_hasBufferSizeInfo)
			{
				uint32 headerBytes = 0;
				if (!ReadStreamHeaders(input.m_data, input.m_length, headerBytes))
				{
					// the picture is dropped: it is marked done but never reaches the display queue
					cemuLog_log(LogType::Force, "H264AVC: Unable to determine picture size. Ignoring decode input");
					std::lock_guard lock(m_decodeQueueMtx);
					slot.result.isDecoded = true;
					slot.result.hasFrame = false;
					coreinit::OSSignalEvent(m_displayQueueEvt);
					return;
				}
				input.m_data += headerBytes;
				input.m_length -= headerBytes;
				m_hasBufferSizeInfo = true;
			}

			const uint32 tag = (uint32)(&slot - m_decodedSliceArray.data());
			cemu_assert_debug(tag < m_decodedSliceArray.size());
			BenchmarkTimer timer;
			timer.Start();
			ivd_video_decode_op_t out;
			const WORD32 status = DecodeCall(input.m_data, input.m_length, tag, out);
			if (status != 0)
			{
				if ((out.u4_error_code & 0xFF) == IVD_RES_CHANGED)
				{
					// new dimensions: start over with a header pass on the same picture
					ResetCodec();
					m_hasBufferSizeInfo = false;
					cemuLog_log(LogType::H264, "H264: Resolution change detected");
					DecodePicture(slot);
					return;
				}
				cemuLog_log(LogType::Force, "H264: Failed to decode frame (error 0x{:08x})", status);
				slot.result.hasFrame = false;
				cemu_assert_unimplemented();
				return;
			}
			timer.Stop();
			const double decodeMs = timer.GetElapsedMilliseconds();

			cemu_assert(out.u4_frame_decoded_flag);
			cemu_assert_debug(out.u4_num_bytes_consumed == input.m_length);
			cemu_assert_debug(m_isBufferedMode || out.u4_output_present);

			if (!out.u4_output_present)
			{
				cemuLog_log(LogType::H264, "H264Bench | DecodeTime {}ms (no frame output)", decodeMs);
			}
			else
			{
				cemu_assert(out.e_output_format == IV_YUV_420SP_UV);
				ApplyBotWCrop(out);
				timer.Start();
				PublishFrame(out);
				timer.Stop();
				const sint32 bufferId = DisplayBufferIndexOf(out.s_disp_frm_buf.pv_y_buf);
				cemu_assert_debug(bufferId == (sint32)out.u4_disp_buf_id);
				cemu_assert(bufferId >= 0);
				ReleaseDisplayBuffer(bufferId);
				cemuLog_log(LogType::H264, "H264Bench | DecodeTime {}ms CopyTime {}ms", decodeMs, timer.GetElapsedMilliseconds());
			}
			if (out.u4_frame_decoded_flag)
				m_numDecodedFrames++;
		}

		// outputs every frame ih264d still holds, then frees all display buffers
		void DrainDecoder()
		{
			ivd_ctl_flush_ip_t in = ApiStruct<ivd_ctl_flush_ip_t>();
			ivd_ctl_flush_op_t out = ApiStruct<ivd_ctl_flush_op_t>();
			const WORD32 status = Control(IVD_CMD_CTL_FLUSH, in, out);
			if (status != 0)
				cemuLog_log(LogType::Force, "H264Dec: Unexpected error during flush ({})", status);
			while (true)
			{
				ivd_video_decode_op_t frame;
				if (DecodeCall(nullptr, 0, 0, frame) != 0)
					break;
				cemu_assert_debug(frame.u4_output_present != 0);
				if (!frame.u4_output_present)
					continue;
				ApplyBotWCrop(frame);
				PublishFrame(frame);
				ReleaseDisplayBuffer(frame.u4_disp_buf_id);
			}
			for (uint32 i = 0; i < (uint32)m_displayBuf.size(); i++)
				ReleaseDisplayBuffer(i);
		}

		void WorkerMain()
		{
			while (!m_threadShouldExit)
			{
				m_decodeSem.decrementWithWait();
				uint32 entry;
				{
					std::lock_guard lock(m_decodeQueueMtx);
					if (m_decodeQueue.empty())
						continue;
					entry = m_decodeQueue.front();
					m_decodeQueue.erase(m_decodeQueue.begin());
				}
				if (entry != CMD_FLUSH)
				{
					DecodePicture(m_decodedSliceArray[entry]);
					continue;
				}
				DrainDecoder();
				{
					// the guest waits for the flush, so nothing new can have been queued
					std::lock_guard lock(m_decodeQueueMtx);
					cemu_assert_debug(m_decodeQueue.empty());
				}
				coreinit::OSSignalEvent(m_flushEvt);
			}
		}

		iv_obj_t* m_codecCtx{nullptr};
		bool m_hasBufferSizeInfo{false};
		bool m_isBufferedMode{false};
		uint32 m_numDecodedFrames{0};
		std::vector<std::vector<uint8>> m_displayBuf;
		std::thread m_decoderThread;
		std::atomic_bool m_threadShouldExit{false};
	};

	H264DecoderBackend* CreateAVCDecoder()
	{
		return new H264AVCDecoder();
	}
};
