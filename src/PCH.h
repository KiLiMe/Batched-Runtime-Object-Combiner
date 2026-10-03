#pragma once

#undef DEBUG

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

// CommonLibF4RD: the CommonLibF4 API with runtime-aware ids (engine ids resolve through
// Data/F4SE/Plugins/f4rd-runtime.bin at run time).
#pragma warning(push)
#include "F4SE/F4SE.h"
#include "RE/Fallout.h"
#include <spdlog/sinks/basic_file_sink.h>
#pragma warning(pop)

// Windows headers come after CommonLib so their macros can't break its declarations.
#include <Windows.h>
#include <DirectXPackedVector.h>

// wingdi.h's ERROR macro collides with enumerators.
#undef ERROR

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace std::literals;

// RuntimeCombiner.log (Documents\My Games\Fallout4\F4SE), set up in Plugin.cpp: "[time] [thread] [level] message".
namespace logger
{
	template <class... Args>
	void info(fmt::format_string<Args...> a_fmt, Args&&... a_args)
	{
		spdlog::info(a_fmt, std::forward<Args>(a_args)...);
	}

	template <class... Args>
	void warn(fmt::format_string<Args...> a_fmt, Args&&... a_args)
	{
		spdlog::warn(a_fmt, std::forward<Args>(a_args)...);
	}

	template <class... Args>
	void error(fmt::format_string<Args...> a_fmt, Args&&... a_args)
	{
		spdlog::error(a_fmt, std::forward<Args>(a_args)...);
	}
}

#include "Version.h"
