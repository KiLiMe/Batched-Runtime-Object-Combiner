#pragma once

#include "Combine/Job.h"

#include <unordered_set>

namespace RC::Combine::Gather
{
	// Objects this plugin hid itself: a rebuild treats them as visible.
	using HiddenSet = std::unordered_set<const RE::NiAVObject*>;
	// References that must keep their root visible (something was attached under it while it was hidden).
	using FormSet = std::unordered_set<std::uint32_t>;

	// Captures a cell's combinable static meshes into a job (main thread). Reads the scene graph only.
	void Cell(RE::TESObjectCELL* a_cell, const HiddenSet& a_ours, const FormSet& a_meshByMesh, Job& a_job);

	// Rotation-convention votes from the world-bound check (FO4-ENGINE-NOTES 7.2: a point is rotated by R^T).
	struct ConventionVotes
	{
		std::uint64_t agree{ 0 };
		std::uint64_t disagree{ 0 };
	};
	[[nodiscard]] ConventionVotes Votes() noexcept;
}
