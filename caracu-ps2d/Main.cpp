// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-FileCopyrightText: 2026 Caracu contributors
// SPDX-License-Identifier: GPL-3.0+

#include "pcsx2/PrecompiledHeader.h"
#include "pcsx2/DebugTools/DebugInterface.h"
#include "pcsx2/DebugTools/Breakpoints.h"
#include "pcsx2/DebugTools/Step.h"
#include "pcsx2/GS.h"
#include "pcsx2/Host.h"
#include "pcsx2/ImGui/FullscreenUI.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/MTGS.h"
#include "pcsx2/SIO/Pad/Pad.h"
#include "pcsx2/VMManager.h"
#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Image.h"
#include "common/MemorySettingsInterface.h"
#include "common/Path.h"
#include "common/RedtapeWindows.h"
#include "common/StringUtil.h"
#include "svnrev.h"

#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

#include <atomic>
#include <charconv>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <future>
#include <map>
#include <io.h>
#include <objbase.h>
#include <thread>

namespace
{
	namespace fs = std::filesystem;
	using Json = rapidjson::Value;
	using Allocator = rapidjson::Document::AllocatorType;
	constexpr size_t MAX_LINE = 1024 * 1024;
	constexpr size_t MAX_QUEUE = 64;
	constexpr u32 MAX_MEMORY = 16384;

	MemorySettingsInterface s_settings;
	std::atomic<bool> s_exit{false};
	std::mutex s_queue_mutex;
	std::condition_variable s_queue_wake;
	std::deque<std::function<void()>> s_queue;
	std::thread::id s_cpu_thread;
	std::mutex s_output_mutex;
	FILE* s_protocol = nullptr;
	std::string s_session;
	std::string s_root;
	std::string s_media;
	u32 s_generation = 0;
	u64 s_vsyncs = 0;
	// A queue interrupt is a temporary CPU barrier, not an explicit user pause.
	bool s_resume_after_dispatch = false;
	std::unique_ptr<rapidjson::Document> s_frame_request;
	u64 s_frame_start = 0;
	u32 s_frame_count = 0;
	u32 s_frame_generation = 0;
	u32 s_artifact_index = 0;
	std::map<std::string, std::string> s_states;
	const std::vector<std::string_view> s_methods = {"hello", "status", "boot", "pause", "resume", "reset", "stop", "shutdown",
		"memory.read", "memory.write", "registers.read", "frame.advance", "input.set", "input.reset", "input.bindings",
		"breakpoint.add", "breakpoint.remove", "breakpoint.list", "state.save", "state.load", "capture", "ee.step"};

	std::string Utf8(const fs::path& path)
	{
		const auto value = path.u8string();
		return {value.begin(), value.end()};
	}

	void Add(Json& object, const char* key, std::string_view value, Allocator& alloc)
	{
		object.AddMember(Json(key, alloc), Json(value.data(), static_cast<rapidjson::SizeType>(value.size()), alloc), alloc);
	}

	void Respond(const Json& id, Json value, Allocator& alloc, bool error = false)
	{
		rapidjson::Document response;
		response.SetObject();
		// Use the caller's allocator until serialization has finished.
		response.AddMember("jsonrpc", "2.0", alloc);
		response.AddMember("id", Json(id, alloc), alloc);
		response.AddMember(rapidjson::StringRef(error ? "error" : "result"), value, alloc);
		rapidjson::StringBuffer buffer;
		rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
		response.Accept(writer);
		std::lock_guard lock(s_output_mutex);
		if (std::fwrite(buffer.GetString(), 1, buffer.GetSize(), s_protocol) != buffer.GetSize() ||
			std::fputc('\n', s_protocol) == EOF || std::fflush(s_protocol) != 0)
			s_exit.store(true);
	}

	void Fail(const Json& id, int code, std::string_view message, Allocator& alloc)
	{
		Json error(rapidjson::kObjectType);
		error.AddMember("code", code, alloc);
		Add(error, "message", message, alloc);
		Respond(id, std::move(error), alloc, true);
	}

	const char* StateName()
	{
		if (s_resume_after_dispatch)
			return "running";
		switch (VMManager::GetState())
		{
			case VMState::Shutdown:
				return "shutdown";
			case VMState::Initializing:
				return "initializing";
			case VMState::Running:
				return "running";
			case VMState::Paused:
				return "paused";
			case VMState::Resetting:
				return "resetting";
			case VMState::Stopping:
				return "stopping";
			default:
				return "unknown";
		}
	}

	Json Identity(Allocator& alloc)
	{
		Json result(rapidjson::kObjectType);
		Add(result, "session", s_session, alloc);
		Add(result, "build", GIT_HASH, alloc);
		result.AddMember("source_dirty_at_configure", CARACU_SOURCE_DIRTY != 0, alloc);
		Add(result, "profile", s_root, alloc);
		Add(result, "state", StateName(), alloc);
		Add(result, "media", s_media, alloc);
		Add(result, "serial", VMManager::GetDiscSerial(), alloc);
		Add(result, "elf", VMManager::GetCurrentELF(), alloc);
		Add(result, "pcsx2_elf_id", fmt::format("{:08X}", VMManager::GetCurrentCRC()), alloc);
		result.AddMember("generation", s_generation, alloc);
		result.AddMember("vsyncs", s_vsyncs, alloc);
		result.AddMember("pid", static_cast<u32>(GetCurrentProcessId()), alloc);
		result.AddMember("elf_booted", VMManager::Internal::HasBootedELF(), alloc);
		result.AddMember("breakpoint_triggered", CBreakPoints::GetBreakpointTriggered(), alloc);
		return result;
	}

	bool StringParam(const Json& params, const char* key, std::string& value)
	{
		if (!params.HasMember(key) || !params[key].IsString())
			return false;
		const Json& item = params[key];
		value.assign(item.GetString(), item.GetStringLength());
		return value.find('\0') == std::string::npos;
	}

	bool UintParam(const Json& params, const char* key, u32& value)
	{
		if (!params.HasMember(key) || !params[key].IsUint())
			return false;
		value = params[key].GetUint();
		return true;
	}

	bool HexAddress(const Json& params, u32& address)
	{
		std::string text;
		if (!StringParam(params, "address", text) || text.size() != 10 || text.substr(0, 2) != "0x")
			return false;
		const auto [end, ec] = std::from_chars(text.data() + 2, text.data() + text.size(), address, 16);
		return ec == std::errc() && end == text.data() + text.size();
	}

	bool RamRange(u32 address, u32 length, bool ee)
	{
		if (address >= 0x80000000u && address < 0xC0000000u)
			address -= address < 0xA0000000u ? 0x80000000u : 0xA0000000u;
		const u32 size = ee ? 32u * 1024 * 1024 : 2u * 1024 * 1024;
		return length > 0 && length <= MAX_MEMORY && address < size && length <= size - address;
	}

	bool Queue(std::function<void()> function)
	{
		std::lock_guard lock(s_queue_mutex);
		if (s_exit.load() || s_queue.size() >= MAX_QUEUE)
			return false;
		s_queue.push_back(std::move(function));
		s_queue_wake.notify_one();
		return true;
	}

	void SetPausedIfNeeded(bool paused)
	{
		// Repeating SetPaused(true) accumulates play time again in the core.
		if ((VMManager::GetState() == VMState::Paused) != paused)
			VMManager::SetPaused(paused);
	}

	void CompleteFrameRequest()
	{
		if (!s_frame_request || (VMManager::GetState() == VMState::Running && !s_exit.load()) || s_resume_after_dispatch)
			return;
		auto request = std::move(s_frame_request);
		auto& alloc = request->GetAllocator();
		if (s_exit.load() || s_generation != s_frame_generation || VMManager::GetState() != VMState::Paused ||
			s_vsyncs - s_frame_start != s_frame_count)
			return Fail((*request)["id"], -32024, "frame advance interrupted; requested count not certified", alloc);
		Json result = Identity(alloc);
		result.AddMember("advanced", s_frame_count, alloc);
		Add(result, "clock", "guest VSync callbacks, not game logic ticks", alloc);
		Respond((*request)["id"], std::move(result), alloc);
	}

	void Dispatch(rapidjson::Document& request)
	{
		Allocator& alloc = request.GetAllocator();
		const Json& id = request["id"];
		const std::string_view method(request["method"].GetString(), request["method"].GetStringLength());
		Json empty(rapidjson::kObjectType);
		const Json& params = request.HasMember("params") ? request["params"] : empty;
		if (!params.IsObject())
			return Fail(id, -32602, "params must be an object", alloc);
		if (std::find(s_methods.begin(), s_methods.end(), method) == s_methods.end())
			return Fail(id, -32601, "method not implemented; consult hello.methods", alloc);
		if (method == "hello")
		{
			Json result = Identity(alloc);
			result.AddMember("protocol_version", 1, alloc);
			result.AddMember("qt", false, alloc);
			Add(result, "renderer", "D3D11 surfaceless", alloc);
			Add(result, "audio", "emulated, nullout sink", alloc);
			Json methods(rapidjson::kArrayType);
			for (std::string_view name : s_methods)
				methods.PushBack(Json(name.data(), static_cast<rapidjson::SizeType>(name.size()), alloc), alloc);
			result.AddMember("methods", methods, alloc);
			return Respond(id, std::move(result), alloc);
		}
		std::string session;
		u32 generation;
		if (!StringParam(params, "session", session) || !UintParam(params, "generation", generation))
			return Fail(id, -32602, "session and generation from hello/status are required", alloc);
		if (session != s_session || generation != s_generation)
			return Fail(id, -32010, "stale session or boot generation; obtain a fresh hello", alloc);
		if (method == "status")
			return Respond(id, Identity(alloc), alloc);
		if (method == "shutdown")
		{
			s_resume_after_dispatch = false;
			if (VMManager::HasValidVM())
				VMManager::Shutdown(false);
			s_media.clear();
			++s_generation;
			Respond(id, Identity(alloc), alloc);
			s_exit.store(true);
			return;
		}
		if (method == "boot")
		{
			if (VMManager::GetState() != VMState::Shutdown)
				return Fail(id, -32011, "stop the current VM before boot", alloc);
			std::string media;
			if (!StringParam(params, "media", media) || media.empty() || !FileSystem::FileExists(media.c_str()))
				return Fail(id, -32602, "media must name an existing local image or ELF", alloc);
			if (s_settings.GetStringValue("Filenames", "BIOS", "").empty())
				return Fail(id, -32012, "start this host with --bios pointing to your own BIOS", alloc);
			VMBootParameters boot;
			boot.filename = media;
			boot.fast_boot = true;
			boot.fullscreen = false;
			boot.disable_achievements_hardcore_mode = true;
			Error error;
			++s_generation; // Even a failed attempt invalidates references to the old VM.
			s_vsyncs = 0;
			if (VMManager::Initialize(boot, &error) != VMBootResult::StartupSuccess)
				return Fail(id, -32013, error.GetDescription(), alloc);
			s_media = media;
			s_resume_after_dispatch = false;
			SetPausedIfNeeded(true);
			return Respond(id, Identity(alloc), alloc);
		}
		if (!VMManager::HasValidVM())
			return Fail(id, -32011, "no VM; boot a local image first", alloc);
		if (method == "pause" || method == "resume")
		{
			s_resume_after_dispatch = false;
			if (method == "resume")
			{
				CBreakPoints::SetSkipFirst(BREAKPOINT_EE, DebugInterface::get(BREAKPOINT_EE).getPC());
				CBreakPoints::SetSkipFirst(BREAKPOINT_IOP, DebugInterface::get(BREAKPOINT_IOP).getPC());
				CBreakPoints::SetBreakpointTriggered(false);
			}
			SetPausedIfNeeded(method == "pause");
			return Respond(id, Identity(alloc), alloc);
		}
		if (method == "stop")
		{
			s_resume_after_dispatch = false;
			CBreakPoints::ClearAllBreakPoints();
			CBreakPoints::ClearAllMemChecks();
			CBreakPoints::SetBreakpointTriggered(false);
			VMManager::Shutdown(false);
			s_media.clear();
			++s_generation;
			return Respond(id, Identity(alloc), alloc);
		}
		if (method == "reset")
		{
			s_resume_after_dispatch = false;
			SetPausedIfNeeded(true);
			if (!VMManager::RequestReset())
				return Fail(id, -32014, "reset rejected by the VM", alloc);
			CBreakPoints::ClearAllBreakPoints();
			CBreakPoints::ClearAllMemChecks();
			CBreakPoints::SetBreakpointTriggered(false);
			Pad::ResetAllControllerInputs();
			SetPausedIfNeeded(true);
			++s_generation;
			s_vsyncs = 0;
			return Respond(id, Identity(alloc), alloc);
		}
		if (VMManager::GetState() != VMState::Paused || s_resume_after_dispatch)
			return Fail(id, -32015, "explicit pause required for debugger access", alloc);
		if (method == "ee.step")
		{
			auto& debug = DebugInterface::get(BREAKPOINT_EE);
			const u32 before = debug.getPC();
			if (!VMManager::Internal::HasBootedELF() || before % 4 || !RamRange(before, 8, true))
				return Fail(id, -32025, "EE step requires booted ELF and an aligned RAM PC", alloc);
			CBreakPoints::SetSkipFirst(BREAKPOINT_EE, before);
			CBreakPoints::SetBreakpointTriggered(false);
			const auto step = ExecuteEEDebugStep();
			VMManager::Internal::ClearCPUExecutionCaches();
			Json result = Identity(alloc);
			Add(result, "pc_before", fmt::format("0x{:08X}", before), alloc);
			Add(result, "pc_after", fmt::format("0x{:08X}", debug.getPC()), alloc);
			result.AddMember("dispatched_instructions", step.dispatched_instructions, alloc);
			result.AddMember("cancelled", step.cancelled, alloc);
			result.AddMember("event_exit", step.event_exit, alloc);
			Add(result, "scope", "real EE interpreter dispatch; delay slot may be grouped; events may advance IOP/VU", alloc);
			return Respond(id, std::move(result), alloc);
		}
		if (method == "frame.advance")
		{
			u32 count;
			if (!UintParam(params, "count", count) || count == 0 || count > 600 || s_frame_request)
				return Fail(id, -32602, "count must be 1..600 and no frame operation may be pending", alloc);
			s_frame_request = std::make_unique<rapidjson::Document>();
			s_frame_request->CopyFrom(request, s_frame_request->GetAllocator());
			s_frame_start = s_vsyncs;
			s_frame_count = count;
			s_frame_generation = s_generation;
			VMManager::FrameAdvance(count);
			return; // Terminal response is sent only after execution returns paused.
		}
		if (method == "input.set" || method == "input.reset" || method == "input.bindings")
		{
			u32 port;
			if (!UintParam(params, "port", port) || port > 1)
				return Fail(id, -32602, "port must be 0 or 1 (no multitap)", alloc);
			const auto* controller = Pad::GetControllerInfo(Pad::ControllerType::DualShock2);
			Json result = Identity(alloc);
			if (method == "input.reset")
				Pad::ResetControllerInputs(port);
			else if (method == "input.set")
			{
				std::string binding;
				if (!StringParam(params, "binding", binding) || !params.HasMember("value") || !params["value"].IsNumber())
					return Fail(id, -32602, "binding and normalized value are required", alloc);
				const double value = params["value"].GetDouble();
				const auto index = controller->GetBindIndex(binding);
				if (!index || !std::isfinite(value) || value < 0 || value > 1)
					return Fail(id, -32602, "unknown binding or value outside [0,1]", alloc);
				Pad::SetControllerState(port, index.value(), static_cast<float>(value));
				Add(result, "binding", binding, alloc);
				result.AddMember("value", value, alloc);
			}
			else
			{
				Json bindings(rapidjson::kArrayType);
				for (const auto& binding : controller->bindings)
					bindings.PushBack(Json(binding.name, alloc), alloc);
				result.AddMember("bindings", bindings, alloc);
			}
			Add(result, "scope", "emulated pad input; game consumption not yet observed", alloc);
			return Respond(id, std::move(result), alloc);
		}
		if (method == "state.save" || method == "state.load")
		{
			std::string token;
			if (method == "state.save")
			{
				token = fmt::format("state-{}", ++s_artifact_index);
				const std::string path = Path::Combine(EmuFolders::Savestates, token + ".p2s");
				if (FileSystem::FileExists(path.c_str()))
					return Fail(id, -32021, "refusing to overwrite a state artifact", alloc);
				std::string error;
				VMManager::SaveState(path.c_str(), false, false, [&error](const std::string& text) { error = text; });
				if (!error.empty() || !FileSystem::FileExists(path.c_str()))
					return Fail(id, -32021, error.empty() ? "no state file produced" : error, alloc);
				s_states.emplace(token, s_media);
			}
			else
			{
				if (!StringParam(params, "token", token) || !s_states.contains(token) || s_states[token] != s_media)
					return Fail(id, -32602, "state token must belong to this session and media", alloc);
				Error error;
				const std::string path = Path::Combine(EmuFolders::Savestates, token + ".p2s");
				if (!VMManager::LoadState(path.c_str(), &error))
					return Fail(id, -32022, error.GetDescription(), alloc);
				++s_generation;
				s_vsyncs = 0;
				CBreakPoints::ClearAllBreakPoints();
				CBreakPoints::ClearAllMemChecks();
				CBreakPoints::SetBreakpointTriggered(false);
				Pad::ResetAllControllerInputs();
			}
			Json result = Identity(alloc);
			Add(result, "token", token, alloc);
			return Respond(id, std::move(result), alloc);
		}
		if (method == "capture")
		{
			MTGS::WaitGS();
			u32 width = 0, height = 0;
			std::vector<u32> pixels;
			if (!MTGS::SaveMemorySnapshot(640, 480, false, false, &width, &height, &pixels) ||
				width == 0 || height == 0 || pixels.empty())
				return Fail(id, -32023, "GS did not produce a frame; no capture artifact", alloc);
			const bool uniform = std::all_of(pixels.begin(), pixels.end(), [first = pixels.front()](u32 pixel) { return pixel == first; });
			const std::string path = Path::Combine(EmuFolders::Snapshots, fmt::format("capture-{}.png", ++s_artifact_index));
			RGBA8Image image(width, height, std::move(pixels));
			if (FileSystem::FileExists(path.c_str()) || !image.SaveToFile(path.c_str()))
				return Fail(id, -32023, "could not create new capture artifact", alloc);
			Json result = Identity(alloc);
			Add(result, "path", path, alloc);
			Add(result, "alignment", "CPU paused, GS queue drained; per-game frame/logic alignment uncalibrated", alloc);
			result.AddMember("width", width, alloc);
			result.AddMember("height", height, alloc);
			result.AddMember("uniform", uniform, alloc);
			return Respond(id, std::move(result), alloc);
		}
		std::string cpu;
		if (!StringParam(params, "cpu", cpu) || (cpu != "ee" && cpu != "iop"))
			return Fail(id, -32602, "cpu must be ee or iop", alloc);
		DebugInterface& debug = DebugInterface::get(cpu == "ee" ? BREAKPOINT_EE : BREAKPOINT_IOP);
		Json result = Identity(alloc);
		Add(result, "cpu", cpu, alloc);
		if (method == "breakpoint.add" || method == "breakpoint.remove" || method == "breakpoint.list")
		{
			const BreakPointCpu target = cpu == "ee" ? BREAKPOINT_EE : BREAKPOINT_IOP;
			if (method != "breakpoint.list")
			{
				u32 address, length = 1;
				std::string kind;
				if (!HexAddress(params, address) || !StringParam(params, "kind", kind) ||
					(kind != "execute" && kind != "read" && kind != "write" && kind != "readwrite"))
					return Fail(id, -32602, "address and kind execute/read/write/readwrite required", alloc);
				if (kind != "execute" && !UintParam(params, "length", length))
					return Fail(id, -32602, "watchpoint length required", alloc);
				if (!RamRange(address, length, cpu == "ee") || (kind == "execute" && address % 4))
					return Fail(id, -32602, "breakpoint must be in RAM; execution addresses must be aligned", alloc);
				if (kind == "execute")
				{
					if (method == "breakpoint.add")
						CBreakPoints::AddBreakPoint(target, address);
					else
						CBreakPoints::RemoveBreakPoint(target, address);
				}
				else if (method == "breakpoint.add")
					CBreakPoints::AddMemCheck(target, address, address + length,
						kind == "read" ? MEMCHECK_READ : kind == "write" ? MEMCHECK_WRITE :
																		   MEMCHECK_READWRITE,
						MEMCHECK_BREAK);
				else
					CBreakPoints::RemoveMemCheck(target, address, address + length);
			}
			Json entries(rapidjson::kArrayType);
			for (const auto& point : CBreakPoints::GetBreakpoints(target, false))
			{
				Json entry(rapidjson::kObjectType);
				Add(entry, "kind", "execute", alloc);
				Add(entry, "address", fmt::format("0x{:08X}", point.addr), alloc);
				entries.PushBack(entry, alloc);
			}
			for (const auto& point : CBreakPoints::GetMemChecks(target))
			{
				Json entry(rapidjson::kObjectType);
				Add(entry, "kind", point.memCond == MEMCHECK_READ ? "read" : point.memCond == MEMCHECK_WRITE ? "write" :
																											   "readwrite",
					alloc);
				Add(entry, "address", fmt::format("0x{:08X}", point.start), alloc);
				entry.AddMember("length", point.end - point.start, alloc);
				entry.AddMember("hits", point.numHits, alloc);
				entries.PushBack(entry, alloc);
			}
			result.AddMember("breakpoints", entries, alloc);
			Add(result, "coverage", "CPU recompiler accesses only; not DMA/VU or a universal watchpoint", alloc);
			return Respond(id, std::move(result), alloc);
		}
		if (method == "registers.read")
		{
			Json categories(rapidjson::kArrayType);
			for (int cat = 0; cat < debug.getRegisterCategoryCount(); ++cat)
			{
				Json category(rapidjson::kObjectType);
				Add(category, "name", debug.getRegisterCategoryName(cat), alloc);
				category.AddMember("bits", debug.getRegisterSize(cat), alloc);
				Json registers(rapidjson::kArrayType);
				for (int reg = 0; reg < debug.getRegisterCount(cat); ++reg)
				{
					const u128 value = debug.getRegister(cat, reg);
					Json item(rapidjson::kObjectType);
					Add(item, "name", debug.getRegisterName(cat, reg), alloc);
					Add(item, "hex", fmt::format("0x{:016X}{:016X}", value.hi, value.lo), alloc);
					registers.PushBack(item, alloc);
				}
				category.AddMember("registers", registers, alloc);
				categories.PushBack(category, alloc);
			}
			result.AddMember("categories", categories, alloc);
			Add(result, "pc", fmt::format("0x{:08X}", debug.getPC()), alloc);
			Add(result, "cycles_u32", fmt::format("0x{:08X}", debug.getCycles()), alloc);
			return Respond(id, std::move(result), alloc);
		}
		u32 address = 0, length = 0;
		std::string data;
		if (!HexAddress(params, address))
			return Fail(id, -32602, "address must be 0x plus eight hexadecimal digits", alloc);
		std::vector<u8> bytes;
		if (method == "memory.write")
		{
			if (!StringParam(params, "hex", data) || data.empty() || data.size() % 2 || data.size() > MAX_MEMORY * 2)
				return Fail(id, -32602, "hex must contain 1..16384 bytes", alloc);
			length = static_cast<u32>(data.size() / 2);
			bytes.resize(length);
			for (u32 i = 0; i < length; ++i)
			{
				unsigned value;
				const auto [end, ec] = std::from_chars(data.data() + i * 2, data.data() + i * 2 + 2, value, 16);
				if (ec != std::errc() || end != data.data() + i * 2 + 2)
					return Fail(id, -32602, "invalid hexadecimal byte", alloc);
				bytes[i] = static_cast<u8>(value);
			}
		}
		else if (!UintParam(params, "length", length))
			return Fail(id, -32602, "length must be an unsigned integer", alloc);
		if (!RamRange(address, length, cpu == "ee"))
			return Fail(id, -32602, "only bounded main RAM and its KSEG0/KSEG1 aliases are permitted; no MMIO", alloc);
		if (method == "memory.write")
		{
			std::vector<u8> previous(length);
			if (!debug.ReadBytes(address, previous.data(), length))
				return Fail(id, -32016, "could not read pre-write bytes; nothing written", alloc);
			std::string old;
			for (u8 byte : previous)
				old += fmt::format("{:02X}", byte);
			Add(result, "previous_hex", old, alloc);
			if (!debug.WriteBytes(address, bytes.data(), length))
				return Fail(id, -32017, "write failed; outcome may be partial, do not retry blindly", alloc);
			VMManager::Internal::ClearCPUExecutionCaches();
		}
		bytes.resize(length);
		if (!debug.ReadBytes(address, bytes.data(), length))
			return Fail(id, -32016, "RAM read failed", alloc);
		std::string hex;
		hex.reserve(length * 2);
		for (u8 byte : bytes)
			hex += fmt::format("{:02X}", byte);
		Add(result, "hex", hex, alloc);
		Add(result, "address", fmt::format("0x{:08X}", address), alloc);
		return Respond(id, std::move(result), alloc);
	}

	void AcceptLine(std::string_view line)
	{
		auto request = std::make_shared<rapidjson::Document>();
		request->Parse<rapidjson::kParseValidateEncodingFlag>(line.data(), line.size());
		Allocator& alloc = request->GetAllocator();
		if (request->HasParseError())
			return Fail(Json(), -32700, "invalid JSON", alloc);
		if (!request->IsObject() || !request->HasMember("jsonrpc") || !(*request)["jsonrpc"].IsString() ||
			std::string_view((*request)["jsonrpc"].GetString(), (*request)["jsonrpc"].GetStringLength()) != "2.0" ||
			!request->HasMember("id") || (!(*request)["id"].IsString() && !(*request)["id"].IsInt64()) ||
			!request->HasMember("method") || !(*request)["method"].IsString())
			return Fail(Json(), -32600, "expected one JSON-RPC 2.0 request with a string/integer id", alloc);
		if (!Queue([request]() { Dispatch(*request); }))
			Fail((*request)["id"], -32020, "CPU command queue full or host stopping; command not accepted", alloc);
	}

	void ReadRequests()
	{
		const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
		std::string line;
		bool oversized = false;
		while (!s_exit.load())
		{
			DWORD available = 0;
			if (!PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr))
				break;
			if (!available)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
				continue;
			}
			char buffer[4096];
			DWORD count;
			if (!ReadFile(input, buffer, std::min<DWORD>(available, sizeof(buffer)), &count, nullptr) || !count)
				break;
			for (DWORD i = 0; i < count; ++i)
			{
				if (buffer[i] == '\n')
				{
					if (oversized)
					{
						rapidjson::Document scratch;
						Fail(Json(), -32600, "request exceeds 1 MiB", scratch.GetAllocator());
					}
					else
						AcceptLine(line);
					line.clear();
					oversized = false;
				}
				else if (!oversized)
				{
					if (line.size() == MAX_LINE)
						oversized = true;
					else
						line += buffer[i];
				}
			}
		}
		s_exit.store(true);
		s_queue_wake.notify_all();
	}

	bool InitializeConfig(const std::string& root, const std::string& bios)
	{
		std::error_code ec;
		const fs::path requested = fs::u8path(root);
		if (!requested.is_absolute() || !fs::is_directory(requested.parent_path(), ec))
		{
			fmt::print(stderr, "--session-dir must be an absolute NEW directory with an existing parent\n");
			return false;
		}
		// Exclusive creation is the ownership boundary. Never adopt a prior profile.
		if (!fs::create_directory(requested, ec))
		{
			fmt::print(stderr, "Refusing existing/uncreatable session directory: {} ({})\n", root, ec.message());
			return false;
		}
		s_root = Utf8(fs::canonical(requested, ec));
		if (ec)
			return false;
		GUID guid;
		if (FAILED(CoCreateGuid(&guid)))
			return false;
		s_session = fmt::format("{:08x}-{:04x}-{:04x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
			guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
			guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
		EmuFolders::SetAppRoot();
		// Do not call SetDataDirectory: its portable/Documents fallbacks are forbidden.
		EmuFolders::DataRoot = s_root;
		EmuFolders::Settings = Path::Combine(s_root, "inis");
		if (!EmuFolders::SetResourcesDirectory())
			return false;
		Host::Internal::SetBaseSettingsLayer(&s_settings);
		VMManager::Internal::SetBlockSystemConsole(true);
		VMManager::SetDefaultSettings(s_settings, true, true, true, true, true);
		s_settings.SetIntValue("EmuCore/GS", "Renderer", static_cast<int>(GSRendererType::DX11));
		s_settings.SetStringValue("SPU2/Output", "OutputModule", "nullout");
		s_settings.SetBoolValue("Logging", "EnableSystemConsole", false);
		s_settings.SetBoolValue("Logging", "EnableFileLogging", false);
		s_settings.SetBoolValue("PINE", "Enabled", false);
		s_settings.SetBoolValue("Achievements", "Enabled", false);
		s_settings.SetBoolValue("Discord", "Enabled", false);
		s_settings.SetBoolValue("InputSources", "SDL", false);
		s_settings.SetBoolValue("InputSources", "XInput", false);
		s_settings.SetBoolValue("InputSources", "DInput", false);
		s_settings.SetBoolValue("InputSources", "ExternalOnly", true);
		s_settings.ClearSection("Hotkeys");
		for (u32 port = 0; port < 8; ++port)
			Pad::ClearPortBindings(s_settings, port);
		s_settings.SetBoolValue("MemoryCards", "Slot1_Enable", false);
		s_settings.SetBoolValue("MemoryCards", "Slot2_Enable", false);
		VMManager::Internal::LoadStartupSettings();
		if (!bios.empty())
		{
			const fs::path target = fs::u8path(EmuFolders::Bios) / "session-bios.bin";
			if (!fs::copy_file(fs::u8path(bios), target, fs::copy_options::none, ec))
			{
				fmt::print(stderr, "Unable to copy private BIOS: {}\n", ec.message());
				return false;
			}
			s_settings.SetStringValue("Filenames", "BIOS", "session-bios.bin");
		}
		const auto font = FileSystem::MapBinaryFileForRead(
			Path::Combine(EmuFolders::Resources, "fonts/Roboto-Regular.ttf").c_str());
		if (font.empty())
			return false;
		ImGuiManager::FontInfo info{};
		info.data = font;
		ImGuiManager::SetFonts({info});
		return true;
	}
} // namespace

std::optional<WindowInfo> Host::AcquireRenderWindow(bool)
{
	WindowInfo info;
	info.type = WindowInfo::Type::Surfaceless;
	info.surface_width = 640;
	info.surface_height = 480;
	return info;
}

void Host::RunOnCPUThread(std::function<void()> function, bool block)
{
	if (std::this_thread::get_id() == s_cpu_thread)
		return function();
	if (!block)
	{
		if (!Queue(std::move(function)))
			fmt::print(stderr, "CPU callback rejected during shutdown/queue saturation\n");
		return;
	}
	auto done = std::make_shared<std::promise<void>>();
	auto future = done->get_future();
	if (!Queue([function = std::move(function), done]() {
			function();
			done->set_value();
		}))
		return;
	while (future.wait_for(std::chrono::milliseconds(10)) != std::future_status::ready && !s_exit.load())
	{
	}
}

void Host::PumpMessagesOnCPUThread()
{
	++s_vsyncs;
	std::lock_guard lock(s_queue_mutex);
	if ((s_exit.load() || !s_queue.empty()) && VMManager::GetState() == VMState::Running)
	{
		s_resume_after_dispatch = true;
		VMManager::SetPaused(true);
	}
}

void Host::RequestExitApplication(bool)
{
	s_exit.store(true);
	s_queue_wake.notify_all();
}

void Host::RequestVMShutdown(bool, bool, bool)
{
	RunOnCPUThread([]() { VMManager::SetState(VMState::Stopping); });
}

int wmain(int argc, wchar_t** argv)
{
	std::string root, bios;
	for (int i = 1; i < argc; ++i)
	{
		const std::wstring_view arg(argv[i]);
		if (arg == L"--help" || arg == L"--version")
		{
			fmt::print("caracu-ps2d protocol 1 ({})\n--session-dir NEW_ABSOLUTE_PATH [--bios OWN_BIOS]\n"
					   "Windows stdio pipes only; no Qt, no personal profiles.\n",
				GIT_HASH);
			return 0;
		}
		if ((arg == L"--session-dir" || arg == L"--bios") && i + 1 < argc)
		{
			std::string& value = arg == L"--session-dir" ? root : bios;
			if (!value.empty())
				return 2;
			value = StringUtil::WideStringToUTF8String(argv[++i]);
		}
		else
		{
			fmt::print(stderr, "Unknown/missing command line argument\n");
			return 2;
		}
	}
	if (root.empty() || GetFileType(GetStdHandle(STD_INPUT_HANDLE)) != FILE_TYPE_PIPE)
	{
		fmt::print(stderr, "An explicit --session-dir and piped stdin are required\n");
		return 2;
	}
	// Reserve the original stdout pipe, then send all core/CRT/Win32 output to stderr.
	const int protocol_fd = _dup(_fileno(stdout));
	if (protocol_fd < 0 || !(s_protocol = _fdopen(protocol_fd, "wb")) || _dup2(_fileno(stderr), _fileno(stdout)) != 0)
		return 2;
	SetStdHandle(STD_OUTPUT_HANDLE, GetStdHandle(STD_ERROR_HANDLE));
	Log::SetConsoleOutputLevel(LOGLEVEL_NONE);
	Log::SetHostOutputLevel(LOGLEVEL_INFO, [](LOGLEVEL, ConsoleColors, std::string_view message) {
		fmt::print(stderr, "{}\n", message);
	});
	const char* hardware_error = nullptr;
	if (!VMManager::PerformEarlyHardwareChecks(&hardware_error))
	{
		fmt::print(stderr, "Unsupported hardware: {}\n", hardware_error ? hardware_error : "unknown");
		return 3;
	}
	if (!InitializeConfig(root, bios))
		return 3;
	s_cpu_thread = std::this_thread::get_id();
	if (!VMManager::Internal::CPUThreadInitialize())
		return 4;
	VMManager::ApplySettings();
	std::thread reader(ReadRequests);
	while (!s_exit.load())
	{
		std::deque<std::function<void()>> commands;
		{
			std::unique_lock lock(s_queue_mutex);
			if (s_queue.empty() && VMManager::GetState() != VMState::Running)
				s_queue_wake.wait_for(lock, std::chrono::milliseconds(10));
			commands.swap(s_queue);
		}
		for (auto& command : commands)
		{
			if (s_exit.load())
				break;
			command();
		}
		if (s_resume_after_dispatch && VMManager::GetState() == VMState::Paused && !s_exit.load())
			VMManager::SetPaused(false);
		s_resume_after_dispatch = false;
		switch (VMManager::GetState())
		{
			case VMState::Running:
				VMManager::Execute();
				break;
			case VMState::Resetting:
				VMManager::Reset();
				++s_generation;
				break;
			case VMState::Stopping:
				VMManager::Shutdown(false);
				++s_generation;
				break;
			default:
				VMManager::IdlePollUpdate();
				break;
		}
		CompleteFrameRequest();
	}
	reader.join();
	s_resume_after_dispatch = false;
	CompleteFrameRequest();
	if (VMManager::GetState() != VMState::Shutdown)
		VMManager::Shutdown(false);
	VMManager::Internal::CPUThreadShutdown();
	std::fclose(s_protocol);
	return 0;
}
