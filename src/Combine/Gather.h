#pragma once

#include "Combine/Job.h"

#include <unordered_set>

namespace RC::Combine::Gather
{
	// Objects this plugin hid itself: a rebuild treats them as visible.
	using HiddenSet = std::unordered_set<const RE::NiAVObject*>;
	// References that must keep their root visible (something was attached under it while it was hidden).
	using FormSet = std::unordered_set<std::uint32_t>;
	// Nodes this plugin parks hidden roots under, and the parent each stands for (Manager's Parking).
	using ParkedMap = std::unordered_map<const RE::NiNode*, RE::NiNode*>;

	// A root's parent, or the parent a parking node stands for.
	[[nodiscard]] inline RE::NiNode* HomeOf(RE::NiNode* a_parent, const ParkedMap& a_parked)
	{
		const auto found = a_parked.find(a_parent);
		return found != a_parked.end() ? found->second : a_parent;
	}

	// Captures a cell's combinable static meshes into a job (main thread). Reads the scene graph only.
	void Cell(RE::TESObjectCELL* a_cell, const HiddenSet& a_ours, const ParkedMap& a_parked, const FormSet& a_meshByMesh, Job& a_job);

	// Rotation-convention votes from the world-bound check (FO4-ENGINE-NOTES 7.2: a point is rotated by R^T).
	struct ConventionVotes
	{
		std::uint64_t agree{ 0 };
		std::uint64_t disagree{ 0 };
	};
	[[nodiscard]] ConventionVotes Votes() noexcept;
}
