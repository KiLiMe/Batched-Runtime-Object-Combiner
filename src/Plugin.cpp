#include "Combine/Manager.h"
#include "Engine/Engine.h"
#include "Settings.h"

namespace
{
	bool g_installed = false;

	void OnMessage(F4SE::MessagingInterface::Message* a_msg)
	{
		if (!g_installed) {
			return;
		}
		switch (a_msg->type) {
		case F4SE::MessagingInterface::kGameDataReady:
			RC::Combine::Manager::OnGameDataReady();
			break;
		case F4SE::MessagingInterface::kPreLoadGame:
			RC::Combine::Manager::OnPreLoadGame();
			break;
		default:
			break;
		}
	}

	bool InitializeLogger()
	{
		auto path = F4SE::log::log_directory();
		if (!path) {
			return false;
		}
		*path /= "RuntimeCombiner.log"sv;
		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
		auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));
		log->set_level(spdlog::level::info);
		log->flush_on(spdlog::level::info);
		spdlog::set_default_logger(std::move(log));
		spdlog::set_pattern("[%H:%M:%S.%e] [%t] [%L] %v"s);
		return true;
	}

	[[nodiscard]] constexpr std::uint32_t PackVersion(std::uint32_t a_major, std::uint32_t a_minor, std::uint32_t a_patch) noexcept
	{
		return ((a_major & 0xFF) << 24) | ((a_minor & 0xFF) << 16) | ((a_patch & 0xFFF) << 4);
	}

	[[nodiscard]] constexpr F4SE::PluginVersionData MakeVersionData() noexcept
	{
		F4SE::PluginVersionData data{};
		data.pluginVersion = PackVersion(RC::Version::MAJOR, RC::Version::MINOR, RC::Version::PATCH);
		for (std::size_t i = 0; i < RC::Version::PROJECT.size() && i < std::size(data.name) - 1; ++i) {
			data.name[i] = RC::Version::PROJECT[i];
		}
		data.addressIndependence = F4SE::PluginVersionData::kAddressIndependence_Signatures;
		data.structureIndependence =
			F4SE::PluginVersionData::kStructureIndependence_1_10_980Layout |
			F4SE::PluginVersionData::kStructureIndependence_1_11_137Layout;
		return data;
	}

	[[nodiscard]] bool RuntimeDatabasePresent(const REL::Version& a_version)
	{
		const auto plugins = std::filesystem::path("Data/F4SE/Plugins");
		std::error_code error;
		return std::filesystem::exists(plugins / "f4rd-runtime.bin", error) ||
		       std::filesystem::exists(plugins / std::format("f4rd-runtime-{}.bin", a_version.string()), error);
	}
}

extern "C" __declspec(dllexport) constinit F4SE::PluginVersionData F4SEPlugin_Version = MakeVersionData();

extern "C" __declspec(dllexport) bool F4SEAPI F4SEPlugin_Query(const F4SE::QueryInterface* a_f4se, F4SE::PluginInfo* a_info)
{
	if (!a_f4se || !a_info) {
		return false;
	}
	a_info->infoVersion = F4SE::PluginInfo::kVersion;
	a_info->name = F4SEPlugin_Version.name;
	a_info->version = F4SEPlugin_Version.pluginVersion;
	return !a_f4se->IsEditor();
}

extern "C" __declspec(dllexport) bool F4SEAPI F4SEPlugin_Load(const F4SE::LoadInterface* a_f4se)
{
	if (!a_f4se || !InitializeLogger()) {
		return false;
	}
	const auto runtime = a_f4se->RuntimeVersion();
	logger::info("Runtime Combiner v{} loading: game {}, F4SE {}", RC::Version::NAME, runtime.string(), a_f4se->F4SEVersion().string());
	if (a_f4se->IsEditor()) {
		return false;
	}

	auto& settings = RC::Settings::Get();
	settings.Load();
	if (!settings.enabled) {
		logger::info("disabled by RuntimeCombiner.ini [General] bEnabled=0");
		return true;
	}
	if (!RuntimeDatabasePresent(runtime)) {
		logger::error("Data/F4SE/Plugins/f4rd-runtime.bin (the CommonLibF4RD Runtime Database) is missing: engine ids can't be resolved; installing nothing");
		return true;
	}

	F4SE::Init(a_f4se);
	const auto& module = REL::Module::get();
	const auto  caps = RC::Engine::Init();

	// Combining needs ids and class layouts that were read from 1.10.163. On a runtime that offers them
	// (so far only OG), or once the AE ids and layouts are verified, Install() runs. Elsewhere the plugin
	// still loads: it reads its ini, logs what is missing and leaves the scene exactly as the engine built
	// it. Nothing below touches the game when a capability is absent.
	if (!caps.CombineReady()) {
		logger::warn(
			"combining is not enabled on {}: its engine ids are verified for 1.10.163 (OG) only; "
			"the plugin loads, reads RuntimeCombiner.ini and changes nothing in the scene",
			module.version().string());
		logger::info(
			"precombine switch: {}",
			caps.precombines ? "available (bDisablePrecombines would work once combining is enabled)" : "unavailable");
		return true;
	}

	RC::Combine::Manager::Install();
	g_installed = true;
	if (const auto messaging = F4SE::GetMessagingInterface()) {
		messaging->RegisterListener(OnMessage);
	}
	return true;
}
