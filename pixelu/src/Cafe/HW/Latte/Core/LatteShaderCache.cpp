// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/CafeSystem.h"
#include "Cafe/HW/Latte/Core/LatteConst.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/Core/LatteShaderCache.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"
#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cemu/FileCache/FileCache.h"
#include "Cafe/GameProfile/GameProfile.h"
#include "WindowSystem.h"

#include "Cafe/HW/Latte/Renderer/Renderer.h"
#ifdef ENABLE_OPENGL
#include "Cafe/HW/Latte/Renderer/OpenGL/RendererShaderGL.h"
#endif
#ifdef ENABLE_VULKAN
#include "Cafe/HW/Latte/Renderer/Vulkan/RendererShaderVk.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineStableCache.h"
#endif

#include <imgui.h>
#include "imgui/imgui_extension.h"

#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"
#include "Cafe/TitleList/GameInfo.h"

#include "Cafe/HW/Latte/Common/RegisterSerializer.h"
#include "Cafe/HW/Latte/Common/ShaderSerializer.h"
#include "util/helpers/Serializer.h"
#include "util/helpers/helpers.h"

#include <audio/IAudioAPI.h>
#include <util/bootSound/BootSoundReader.h>
#include <thread>
#include <deque>

#if BOOST_OS_WINDOWS
#include <psapi.h>
#endif

// Transferable shader cache: one file per title with every shader program the game used,
// together with the register state it was translated under. On boot all entries are
// translated again and handed to the backend, which has its own precompiled cache.
//
// Entry layout (big endian):
//   u8   version (low nibble, always 1) | stage (high nibble: 0 vertex, 1 geometry, 2 pixel)
//   u64  base hash
//   u64  aux hash
//   vertex:   u8 uses geometry shader, register state, fetch program, vertex program
//   geometry: u16 vertex ring parameter count, register state, copy program, geometry program
//   pixel:    u8 uses geometry shader, register state, pixel program
// The entry name is (base hash with the stage in the top three bits, aux hash).

namespace
{
	enum : uint8
	{
		kEntryVertex = 0,
		kEntryGeometry = 1,
		kEntryPixel = 2,
	};
	constexpr uint8 kEntryVersion = 1;
	// extra version of cache files written by 1.25.0 - 1.25.1b, still accepted when opening
	constexpr uint32 kLegacyExtraVersion = 2;
	constexpr size_t kMaxPendingCompiles = 32;

	FileCache* s_cacheFile = nullptr;

	// numbers shown on the loading screen
	struct
	{
		sint32 translated;
		sint32 vertex;
		sint32 geometry;
		sint32 pixel;
		sint32 shaderEntriesRead;
		sint32 shaderEntryCount;
		uint32 pipelinesLoaded;
		sint32 pipelineCount;
		ImTextureID tvBackground;
		ImTextureID drcBackground;
	} s_loading{};

	// shaders whose backend compile was started but not waited for
	std::deque<LatteDecompilerShader*> s_pendingCompiles;

	void QueueCompile(LatteDecompilerShader* shader)
	{
		cemu_assert(s_pendingCompiles.size() < kMaxPendingCompiles);
		s_pendingCompiles.push_back(shader);
	}

	// waits for queued compiles until at most `keep` are left
	void DrainCompiles(size_t keep)
	{
		while (s_pendingCompiles.size() > keep)
		{
			LatteDecompilerShader* shader = s_pendingCompiles.front();
			if (shader)
				LatteShader_FinishCompilation(shader);
			s_pendingCompiles.pop_front();
		}
	}

	uint64 EntryName(uint64 baseHash, uint8 stage)
	{
		constexpr uint64 kStageBits = 7ULL << 61;
		return (baseHash & ~kStageBits) | ((uint64)stage << 61);
	}

	void BeginEntry(MemStreamWriter& out, uint8 stage, uint64 baseHash, uint64 auxHash)
	{
		out.writeBE<uint8>((uint8)(kEntryVersion | (stage << 4)));
		out.writeBE<uint64>(baseHash);
		out.writeBE<uint64>(auxHash);
	}

	void WriteRegisters(MemStreamWriter& out, uint32* contextRegisters)
	{
		Latte::GPUCompactedRegisterState compact;
		Latte::StoreGPURegisterState(*(LatteContextRegister*)contextRegisters, compact);
		Latte::SerializeRegisterState(compact, out);
	}

	void StoreEntry(MemStreamWriter& out, uint8 stage, uint64 baseHash, uint64 auxHash)
	{
		std::span<uint8> blob = out.getResult();
		s_cacheFile->AddFileAsync({ EntryName(baseHash, stage), auxHash }, blob.data(), (sint32)blob.size());
	}

	bool ReadRegisters(MemStreamReader& in, LatteContextRegister& regs)
	{
		Latte::GPUCompactedRegisterState compact;
		if (!Latte::DeserializeRegisterState(compact, in))
			return false;
		Latte::LoadGPURegisterState(regs, compact);
		return !in.hasError();
	}

	bool ReadProgram(MemStreamReader& in, std::vector<uint8>& program)
	{
		return Latte::DeserializeShaderProgram(program, in) && !in.hasError();
	}

	// starts the backend compile of a shader read from the cache
	void SubmitCachedShader(LatteDecompilerShader* shader)
	{
		switch (shader->shaderType)
		{
		case LatteConst::ShaderType::Vertex:
			s_loading.vertex++;
			break;
		case LatteConst::ShaderType::Geometry:
			s_loading.geometry++;
			break;
		case LatteConst::ShaderType::Pixel:
			s_loading.pixel++;
			break;
		default:
			break;
		}
		s_loading.translated++;
		LatteShader_CreateRendererShader(shader, true);
		if (shader->shader)
			QueueCompile(shader);
	}

	void FinishCachedShader(LatteDecompilerShader* shader, uint64 baseHash, uint64 auxHash, uint32 dumpType, std::vector<uint8>& program)
	{
		LatteShader_DumpShader(baseHash, auxHash, shader);
		LatteShader_DumpRawShader(baseHash, auxHash, dumpType, program.data(), (uint32)program.size());
		SubmitCachedShader(shader);
		LatteSHRC_RegisterShader(shader, baseHash, auxHash);
	}

	bool LoadVertexEntry(MemStreamReader& in)
	{
		const uint64 baseHash = in.readBE<uint64>();
		const uint64 auxHash = in.readBE<uint64>();
		const bool usesGeometryShader = in.readBE<uint8>() != 0;
		auto regs = std::make_unique<LatteContextRegister>();
		std::vector<uint8> fetchProgram, vertexProgram;
		if (!ReadRegisters(in, *regs) || !ReadProgram(in, fetchProgram) || !ReadProgram(in, vertexProgram) || !in.isEndOfStream())
			return false;
		uint32* rawRegs = regs->GetRawView();
		// the pixel shader inputs decide which outputs the vertex shader has
		LatteShader_UpdatePSInputs(rawRegs);
		const LatteFetchShader::CacheHash fetchHash = LatteFetchShader::CalculateCacheHash((uint32*)fetchProgram.data(), (uint32)fetchProgram.size());
		LatteFetchShader* fetchShader = LatteShaderRecompiler_createFetchShader(fetchHash, rawRegs, (uint32*)fetchProgram.data(), (uint32)fetchProgram.size());
		LatteDecompilerOptions options;
		LatteShader_GetDecompilerOptions(options, LatteConst::ShaderType::Vertex, usesGeometryShader);
		LatteDecompilerOutput_t output{};
		LatteDecompiler_DecompileVertexShader(baseHash, rawRegs, vertexProgram.data(), (uint32)vertexProgram.size(), fetchShader, options, &output);
		LatteDecompilerShader* shader = LatteShader_CreateShaderFromDecompilerOutput(output, baseHash, false, auxHash, rawRegs);
		FinishCachedShader(shader, baseHash, auxHash, SHADER_DUMP_TYPE_VERTEX, vertexProgram);
		return true;
	}

	bool LoadGeometryEntry(MemStreamReader& in)
	{
		const uint64 baseHash = in.readBE<uint64>();
		const uint64 auxHash = in.readBE<uint64>();
		const uint32 vsRingParameterCount = in.readBE<uint16>();
		auto regs = std::make_unique<LatteContextRegister>();
		std::vector<uint8> copyProgram, geometryProgram;
		if (!ReadRegisters(in, *regs) || !ReadProgram(in, copyProgram) || !ReadProgram(in, geometryProgram) || !in.isEndOfStream())
			return false;
		uint32* rawRegs = regs->GetRawView();
		LatteShader_UpdatePSInputs(rawRegs);
		LatteDecompilerOptions options;
		LatteShader_GetDecompilerOptions(options, LatteConst::ShaderType::Geometry, true);
		LatteDecompilerOutput_t output{};
		LatteDecompiler_DecompileGeometryShader(baseHash, rawRegs, geometryProgram.data(), (uint32)geometryProgram.size(), copyProgram.data(), (uint32)copyProgram.size(), vsRingParameterCount, options, &output);
		LatteDecompilerShader* shader = LatteShader_CreateShaderFromDecompilerOutput(output, baseHash, false, auxHash, rawRegs);
		FinishCachedShader(shader, baseHash, auxHash, SHADER_DUMP_TYPE_GEOMETRY, geometryProgram);
		return true;
	}

	bool LoadPixelEntry(MemStreamReader& in)
	{
		const uint64 baseHash = in.readBE<uint64>();
		const uint64 auxHash = in.readBE<uint64>();
		const bool usesGeometryShader = in.readBE<uint8>() != 0;
		auto regs = std::make_unique<LatteContextRegister>();
		std::vector<uint8> pixelProgram;
		if (!ReadRegisters(in, *regs) || !ReadProgram(in, pixelProgram) || !in.isEndOfStream())
			return false;
		uint32* rawRegs = regs->GetRawView();
		LatteShader_UpdatePSInputs(rawRegs);
		LatteDecompilerOptions options;
		LatteShader_GetDecompilerOptions(options, LatteConst::ShaderType::Pixel, usesGeometryShader);
		LatteDecompilerOutput_t output{};
		LatteDecompiler_DecompilePixelShader(baseHash, rawRegs, pixelProgram.data(), (uint32)pixelProgram.size(), options, &output);
		LatteDecompilerShader* shader = LatteShader_CreateShaderFromDecompilerOutput(output, baseHash, false, auxHash, rawRegs);
		FinishCachedShader(shader, baseHash, auxHash, SHADER_DUMP_TYPE_PIXEL, pixelProgram);
		return true;
	}

	// false if the entry is damaged or of an unknown kind
	bool LoadEntry(const uint8* data, size_t size)
	{
		if (size < 8)
			return false;
		MemStreamReader in(data, (sint32)size);
		const uint8 header = in.readBE<uint8>();
		if ((header & 0xF) != kEntryVersion)
			return false;
		switch ((header >> 4) & 0xF)
		{
		case kEntryVertex:
			return LoadVertexEntry(in);
		case kEntryGeometry:
			return LoadGeometryEntry(in);
		case kEntryPixel:
			return LoadPixelEntry(in);
		default:
			return false;
		}
	}

	// uncompressed 24 bit TGA (true color or grayscale type codes), converted to RGB rows
	struct BootImage
	{
		sint16 width;
		sint16 height;
		std::vector<uint8> rgb;
	};

	bool DecodeBootImage(const std::vector<uint8>& file, BootImage& image)
	{
		constexpr size_t kHeaderSize = 18;
		if (file.size() <= kHeaderSize)
			return false;
		const uint8 typeCode = file[2];
		if (typeCode != 2 && typeCode != 3)
			return false;
		image.width = (sint16)(file[12] | (file[13] << 8));
		image.height = (sint16)(file[14] | (file[15] << 8));
		if (file[16] / 8 != 3)
			return false;
		const uint32 byteCount = (uint32)(image.width * image.height * 3);
		if (byteCount + kHeaderSize >= file.size())
			return false;
		image.rgb.assign(file.begin() + kHeaderSize, file.begin() + kHeaderSize + byteCount);
		for (uint32 i = 0; i < byteCount; i += 3)
			std::swap(image.rgb[i], image.rgb[i + 2]);
		return true;
	}

	ImTextureID LoadBootImage(const char* fileName)
	{
		const std::string path = fmt::format("{}/meta/{}", CafeSystem::GetMlcStoragePath(CafeSystem::GetForegroundTitleId()), fileName);
		sint32 status;
		FSCVirtualFile* file = fsc_open(path.c_str(), FSC_ACCESS_FLAG::OPEN_FILE | FSC_ACCESS_FLAG::READ_PERMISSION, &status);
		if (!file)
			return nullptr;
		ImTextureID texture = nullptr;
		const uint32 size = fsc_getFileSize(file);
		if (size > 0)
		{
			std::vector<uint8> data(size);
			fsc_readFile(file, data.data(), size);
			BootImage image;
			if (DecodeBootImage(data, image))
				texture = g_renderer->GenerateTexture(image.rgb, { image.width, image.height });
		}
		fsc_close(file);
		return texture;
	}

	// plays the title's boot jingle while shaders load and fades it out afterwards
	class BootJingle
	{
	public:
		~BootJingle()
		{
			m_stop = true;
			if (m_thread.joinable())
				m_thread.join();
		}

		void Start()
		{
			if (m_thread.joinable())
				return;
			m_fadeOut = false;
			m_stop = false;
			m_thread = std::thread([this] { Run(); });
		}

		void FadeOut()
		{
			m_fadeOut = true;
		}

	private:
		static constexpr sint32 kRate = 48000;
		static constexpr sint32 kChannels = 2;
		static constexpr sint32 kBits = 16;
		static constexpr sint32 kFramesPerBlock = kRate / 10;
		static constexpr uint64 kFadeFrames = kRate * 2;

		// -60 dB at the end of the fade
		void Attenuate(sint16* samples, size_t count, uint64& fadePos)
		{
			for (size_t i = 0; i + 1 < count; i += 2)
			{
				const float gain = std::pow(10.0f, ((float)fadePos / kFadeFrames * -60.0f) / 20.0f);
				samples[i] = (sint16)(samples[i] * gain);
				samples[i + 1] = (sint16)(samples[i + 1] * gain);
				fadePos++;
			}
		}

		void Run()
		{
			SetThreadName("bootsnd");
			AudioAPIPtr device;
			try
			{
				device = IAudioAPI::CreateDeviceFromConfig(IAudioAPI::AudioType::TV, kRate, kChannels, kFramesPerBlock, kBits);
			}
			catch (const std::runtime_error&)
			{
				cemuLog_log(LogType::Force, "Failed to initialise audio device for bootup sound");
				return;
			}
			if (!device)
				return;
			device->SetAudioDelayOverride(4);
			device->Play();

			const std::string path = fmt::format("{}/meta/bootSound.btsnd", CafeSystem::GetMlcStoragePath(CafeSystem::GetForegroundTitleId()));
			if (!fsc_doesFileExist(path.c_str()))
				return;
			sint32 status = FSC_STATUS_UNDEFINED;
			FSCVirtualFile* file = fsc_open(path.c_str(), FSC_ACCESS_FLAG::OPEN_FILE | FSC_ACCESS_FLAG::READ_PERMISSION, &status);
			if (!file)
			{
				cemuLog_log(LogType::Force, "failed to open bootSound.btsnd");
				return;
			}
			{
				BootSoundReader reader(file, kFramesPerBlock * kChannels * (kBits / 8));
				uint64 fadePos = 0;
				while (fadePos < kFadeFrames && !m_stop)
				{
					while (device->NeedAdditionalBlocks())
					{
						sint16* block = reader.getSamples();
						if (!block)
						{
							m_stop = true; // end of the file
							break;
						}
						if (m_fadeOut)
							Attenuate(block, kFramesPerBlock * kChannels, fadePos);
						device->FeedBlock(block);
					}
					std::this_thread::sleep_for(std::chrono::milliseconds(1000 * kFramesPerBlock / kRate));
				}
			}
			fsc_close(file);
		}

		std::thread m_thread;
		std::atomic_bool m_fadeOut{ false };
		std::atomic_bool m_stop{ false };
	};
	BootJingle s_bootJingle;

	void NotifyBackendLoadingDone()
	{
		switch (g_renderer->GetType())
		{
#ifdef ENABLE_VULKAN
		case RendererAPI::Vulkan:
			RendererShaderVk::ShaderCacheLoading_end();
			break;
#endif
#ifdef ENABLE_OPENGL
		case RendererAPI::OpenGL:
			RendererShaderGL::ShaderCacheLoading_end();
			break;
#endif
		default:
			break;
		}
	}

	// full screen image, scaled to fit and centered
	void DrawBackground(ImTextureID texture, int width, int height)
	{
		constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
			ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoBringToFrontOnFocus;
		const ImVec2 display = ImGui::GetIO().DisplaySize;
		ImGui::SetNextWindowPos({ 0, 0 }, ImGuiCond_Always);
		ImGui::SetNextWindowSize(display, ImGuiCond_Always);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0, 0 });
		if (ImGui::Begin("Background texture", nullptr, kFlags) && texture)
		{
			// fit the width first, the height if the image would be too tall
			float drawW = display.x;
			float drawH = height * drawW / width;
			if (drawH > display.y)
			{
				drawH = display.y;
				drawW = width * drawH / height;
			}
			const float marginX = (display.x - drawW) / 2.0f;
			const float marginY = (display.y - drawH) / 2.0f;
			ImGui::GetWindowDrawList()->AddImage(texture, ImVec2(marginX, marginY), ImVec2(display.x - marginX, display.y - marginY), { 0, 1 }, { 1, 0 });
		}
		ImGui::End();
		ImGui::PopStyleVar(2);
	}

	void DrawProgressScreen(bool pipelines, const Vector2f& window)
	{
		constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
			ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize;
		const ImU32 kBarColor = 0xFF888888;
		DrawBackground(s_loading.tvBackground, 1280, 720);
		ImFont* largeFont = ImGui_GetFont(window.y / 32.0f);
		ImFont* smallFont = ImGui_GetFont(window.y / 48.0f);

		const uint32 done = pipelines ? s_loading.pipelinesLoaded : (uint32)s_loading.shaderEntriesRead;
		const sint32 total = pipelines ? s_loading.pipelineCount : s_loading.shaderEntryCount;
		const float fraction = total > 0 ? (float)done / (float)total : 0.0f;
		const char* caption = pipelines ? "Loading cached pipelines..." : (s_loading.translated >= 3 ? "Compiling cached shaders..." : "Loading cached shaders...");
		const std::string counter = fmt::format("{}/{} ({}%)", done, total, (int)(fraction * 100));

		ImGui::SetNextWindowPos({ window.x / 2.0f, window.y / 2.0f }, ImGuiCond_Always, { 0.5f, 0.5f });
		ImGui::SetNextWindowSize({ ImGui::GetIO().DisplaySize.x * 0.5f, 0 }, ImGuiCond_Always);
		ImGui::SetNextWindowBgAlpha(0.8f);
		ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kBarColor);
		ImGui::PushStyleColor(ImGuiCol_WindowBg, 0);
		ImGui::PushFont(largeFont);
		if (ImGui::Begin("Shader progress", nullptr, kFlags))
		{
			const float center = ImGui::GetWindowSize().x / 2.0f;
			ImGui::SetCursorPosX(center - ImGui::CalcTextSize(caption).x / 2);
			ImGui::Text("%s", caption);
			ImGui::ProgressBar(fraction, { -1, 0 }, "");
			ImGui::SetCursorPosX(center - ImGui::CalcTextSize(counter.c_str()).x / 2);
			ImGui::Text("%s", counter.c_str());
		}
		ImGui::End();
		ImGui::PopFont();
		ImGui::PopStyleColor(2);

		if (pipelines)
			return;
		// per stage counters in the lower left corner
		ImGui::SetNextWindowPos({ 10, window.y - 10 }, ImGuiCond_Always, { 0, 1 });
		ImGui::SetNextWindowBgAlpha(0.8f);
		ImGui::PushStyleColor(ImGuiCol_WindowBg, 0);
		ImGui::PushFont(smallFont);
		if (ImGui::Begin("Shader count", nullptr, kFlags))
		{
			const float column = smallFont->FallbackAdvanceX * 25.f;
			const std::pair<const char*, sint32> rows[] = {
				{ "Vertex shaders", s_loading.vertex },
				{ "Pixel shaders", s_loading.pixel },
				{ "Geometry shaders", s_loading.geometry },
			};
			for (const auto& row : rows)
			{
				ImGui::Text("%s", row.first);
				ImGui::SameLine(column);
				ImGui::Text("%d", row.second);
			}
		}
		ImGui::End();
		ImGui::PopStyleColor();
		ImGui::PopFont();
	}

	// runs step() until it returns false, redrawing the loading screen at about 20 fps
	void RunWithProgressScreen(const std::function<bool()>& step, bool pipelines)
	{
		auto lastRedraw = tick_cached();
		while (!Latte_GetStopSignal() && step())
		{
			if (tick_cached() - lastRedraw < std::chrono::milliseconds(1000 / 20))
				continue;
			int w, h;
			WindowSystem::GetWindowPhysSize(w, h);
			const Vector2f window{ (float)w, (float)h };
			// make sure both font sizes exist before the frame starts
			ImGui_GetFont(window.y / 32.0f);
			ImGui_GetFont(window.y / 48.0f);
			g_renderer->BeginFrame(true);
			if (g_renderer->ImguiBegin(true))
			{
				DrawProgressScreen(pipelines, window);
				g_renderer->ImguiEnd();
			}
			g_renderer->BeginFrame(false);
			if (g_renderer->ImguiBegin(false))
			{
				DrawBackground(s_loading.drcBackground, 854, 480);
				g_renderer->ImguiEnd();
			}
			g_renderer->SwapBuffers(true, true);
			lastRedraw = tick_cached();
		}
	}

	bool StepPipelineLoading()
	{
		uint32 missingShaders = 0;
		switch (g_renderer->GetType())
		{
#ifdef ENABLE_VULKAN
		case RendererAPI::Vulkan:
			return VulkanPipelineStableCache::GetInstance().UpdateLoading(s_loading.pipelinesLoaded, missingShaders);
#endif
		default:
			return false;
		}
	}

	void LoadPipelines(uint64 titleId)
	{
		switch (g_renderer->GetType())
		{
#ifdef ENABLE_VULKAN
		case RendererAPI::Vulkan:
			s_loading.pipelineCount = VulkanPipelineStableCache::GetInstance().BeginLoading(titleId);
			break;
#endif
		default:
			break;
		}
		s_loading.pipelinesLoaded = 0;
		RunWithProgressScreen(StepPipelineLoading, true);
		switch (g_renderer->GetType())
		{
#ifdef ENABLE_VULKAN
		case RendererAPI::Vulkan:
			VulkanPipelineStableCache::GetInstance().EndLoading();
			break;
#endif
		default:
			break;
		}
	}

	// the title id is folded in so caches cannot be swapped between titles
	uint32 TitleSalt(uint64 titleId)
	{
		return (uint32)(titleId >> 32) + (uint32)titleId * 3;
	}
}

uint32 LatteShaderCache_getShaderCacheExtraVersion(uint64 titleId)
{
	constexpr uint32 kFormatRevision = 1;
	return TitleSalt(titleId) + kFormatRevision + 0xe97af1ad;
}

uint32 LatteShaderCache_getPipelineCacheExtraVersion(uint64 titleId)
{
	constexpr uint32 kFormatRevision = 1;
	return TitleSalt(titleId) + kFormatRevision;
}

void LatteShaderCache_Load()
{
	s_loading.translated = 0;
	s_loading.vertex = 0;
	s_loading.geometry = 0;
	s_loading.pixel = 0;
	const uint64 titleId = CafeSystem::GetForegroundTitleId();
	const auto loadStart = now_cached();
#if BOOST_OS_WINDOWS
	PROCESS_MEMORY_COUNTERS memBefore;
	GetProcessMemoryInfo(GetCurrentProcess(), &memBefore, sizeof(memBefore));
#endif
	s_pendingCompiles.clear();
	std::error_code ec;
	fs::create_directories(ActiveSettings::GetCachePath("shaderCache/transferable"), ec);
	fs::create_directories(ActiveSettings::GetCachePath("shaderCache/precompiled"), ec);
	const RendererAPI api = g_renderer->GetType();
	switch (api)
	{
#ifdef ENABLE_VULKAN
	case RendererAPI::Vulkan:
		RendererShaderVk::ShaderCacheLoading_begin(titleId);
		break;
#endif
#ifdef ENABLE_OPENGL
	case RendererAPI::OpenGL:
		RendererShaderGL::ShaderCacheLoading_begin(titleId);
		break;
#endif
	default:
		break;
	}

	const fs::path cachePath = ActiveSettings::GetCachePath("shaderCache/transferable/{:016x}_shaders.bin", titleId);
	s_cacheFile = FileCache::Open(cachePath, false, kLegacyExtraVersion);
	if (!s_cacheFile)
		s_cacheFile = FileCache::Open(cachePath, true, LatteShaderCache_getShaderCacheExtraVersion(titleId));
	if (!s_cacheFile)
	{
		cemuLog_log(LogType::Force, "Unable to open or create shader cache file \"{}\"", _pathToUtf8(cachePath));
		NotifyBackendLoadingDone();
		return;
	}
	s_cacheFile->UseCompression(false);
	s_loading.shaderEntryCount = s_cacheFile->GetFileCount();
	s_loading.shaderEntriesRead = 0;

	s_loading.tvBackground = LoadBootImage("bootTvTex.tga");
	s_loading.drcBackground = LoadBootImage("bootDRCTex.tga");
	if (GetConfig().play_boot_sound)
		s_bootJingle.Start();

	sint32 entriesLoaded = 0;
	uint32 index = 0;
	auto loadNext = [&]() -> bool
	{
		if (index >= (uint32)s_cacheFile->GetMaximumFileIndex())
			return false;
		// keep a couple of free slots for the shaders of this entry
		DrainCompiles(kMaxPendingCompiles - 2);
		uint64 name1, name2;
		std::vector<uint8> data;
		const uint32 current = index++;
		if (!s_cacheFile->GetFileByIndex(current, &name1, &name2, data))
			return true; // empty slot
		s_loading.shaderEntriesRead++;
		if (!LoadEntry(data.data(), data.size()))
		{
			cemuLog_log(LogType::Force, "Shader cache entry {} invalid, deleting...", current);
			s_cacheFile->DeleteFile({ name1, name2 });
		}
		entriesLoaded++;
		return true;
	};
	RunWithProgressScreen(loadNext, false);
	DrainCompiles(0);
#if BOOST_OS_WINDOWS
	PROCESS_MEMORY_COUNTERS memAfter;
	GetProcessMemoryInfo(GetCurrentProcess(), &memAfter, sizeof(memAfter));
	const auto loadMs = std::chrono::duration_cast<std::chrono::milliseconds>(now_cached() - loadStart).count();
	cemuLog_log(LogType::Force, "Shader cache loaded with {} shaders. Commited mem {}MB. Took {}ms", entriesLoaded, (sint32)(((sint64)memAfter.PagefileUsage - (sint64)memBefore.PagefileUsage) / 1024 / 1024), loadMs);
#else
	(void)loadStart;
#endif
	NotifyBackendLoadingDone();
#ifdef ENABLE_VULKAN
	if (api == RendererAPI::Vulkan)
		LoadPipelines(titleId);
#endif

	// one last frame with only the background images, then release them
	g_renderer->BeginFrame(true);
	if (g_renderer->ImguiBegin(true))
	{
		DrawBackground(s_loading.tvBackground, 1280, 720);
		g_renderer->ImguiEnd();
	}
	g_renderer->BeginFrame(false);
	if (g_renderer->ImguiBegin(false))
	{
		DrawBackground(s_loading.drcBackground, 854, 480);
		g_renderer->ImguiEnd();
	}
	g_renderer->SwapBuffers(true, true);
	if (s_loading.tvBackground)
		g_renderer->DeleteTexture(s_loading.tvBackground);
	if (s_loading.drcBackground)
		g_renderer->DeleteTexture(s_loading.drcBackground);
	s_loading.tvBackground = nullptr;
	s_loading.drcBackground = nullptr;
	s_bootJingle.FadeOut();
	if (Latte_GetStopSignal())
		LatteThread_Exit();
}

void LatteShaderCache_writeSeparableVertexShader(uint64 shaderBaseHash, uint64 shaderAuxHash, uint8* fetchShader, uint32 fetchShaderSize, uint8* vertexShader, uint32 vertexShaderSize, uint32* contextRegisters, bool usesGeometryShader)
{
	if (!s_cacheFile)
		return;
	MemStreamWriter out(128 * 1024);
	BeginEntry(out, kEntryVertex, shaderBaseHash, shaderAuxHash);
	out.writeBE<uint8>(usesGeometryShader ? 1 : 0);
	WriteRegisters(out, contextRegisters);
	Latte::SerializeShaderProgram(fetchShader, fetchShaderSize, out);
	Latte::SerializeShaderProgram(vertexShader, vertexShaderSize, out);
	StoreEntry(out, kEntryVertex, shaderBaseHash, shaderAuxHash);
}

void LatteShaderCache_writeSeparableGeometryShader(uint64 shaderBaseHash, uint64 shaderAuxHash, uint8* geometryShader, uint32 geometryShaderSize, uint8* gsCopyShader, uint32 gsCopyShaderSize, uint32* contextRegisters, uint32* hleSpecialState, uint32 vsRingParameterCount)
{
	if (!s_cacheFile)
		return;
	MemStreamWriter out(128 * 1024);
	BeginEntry(out, kEntryGeometry, shaderBaseHash, shaderAuxHash);
	cemu_assert_debug(vsRingParameterCount < 0x10000);
	out.writeBE<uint16>((uint16)vsRingParameterCount);
	WriteRegisters(out, contextRegisters);
	Latte::SerializeShaderProgram(gsCopyShader, gsCopyShaderSize, out);
	Latte::SerializeShaderProgram(geometryShader, geometryShaderSize, out);
	StoreEntry(out, kEntryGeometry, shaderBaseHash, shaderAuxHash);
}

void LatteShaderCache_writeSeparablePixelShader(uint64 shaderBaseHash, uint64 shaderAuxHash, uint8* pixelShader, uint32 pixelShaderSize, uint32* contextRegisters, bool usesGeometryShader)
{
	if (!s_cacheFile)
		return;
	MemStreamWriter out(128 * 1024);
	BeginEntry(out, kEntryPixel, shaderBaseHash, shaderAuxHash);
	out.writeBE<uint8>(usesGeometryShader ? 1 : 0);
	WriteRegisters(out, contextRegisters);
	Latte::SerializeShaderProgram(pixelShader, pixelShaderSize, out);
	StoreEntry(out, kEntryPixel, shaderBaseHash, shaderAuxHash);
}

void LatteShaderCache_Close()
{
	delete s_cacheFile;
	s_cacheFile = nullptr;
	switch (g_renderer->GetType())
	{
#ifdef ENABLE_VULKAN
	case RendererAPI::Vulkan:
		RendererShaderVk::ShaderCacheLoading_Close();
		VulkanPipelineStableCache::GetInstance().Close();
		break;
#endif
#ifdef ENABLE_OPENGL
	case RendererAPI::OpenGL:
		RendererShaderGL::ShaderCacheLoading_Close();
		break;
#endif
	default:
		break;
	}
}
