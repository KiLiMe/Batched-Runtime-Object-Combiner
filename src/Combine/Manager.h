#pragma once

namespace RC::Combine::Manager
{
	// Event sinks, the per-frame hook, the previs query hook and the bake thread (Plugin load, after Engine::Init).
	void Install();

	void OnGameDataReady();  // switches precombines off before any cell loads
	void OnPreLoadGame();    // undoes everything before a save replaces the world
}
