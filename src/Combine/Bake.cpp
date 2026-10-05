#include "Combine/Bake.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <tuple>

#include <excpt.h>

namespace RC::Bake
{
	namespace
	{
		// Engine memory is read under SEH: a mesh freed or changed behind our back fails the copy, not the game.
		bool SafeCopy(void* a_dst, const void* a_src, std::size_t a_bytes) noexcept
		{
			__try {
				std::memcpy(a_dst, a_src, a_bytes);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		struct Key
		{
			std::int32_t x, y, z;
			auto         operator<=>(const Key&) const = default;
		};

		Chunk SoloOf(std::span<const Member> a_members, std::uint32_t a_index)
		{
			Chunk solo;
			solo.members.push_back(a_index);
			solo.solo = true;
			std::copy_n(a_members[a_index].center, 3, solo.origin);
			return solo;
		}

		// One greedy pass: a seed takes every free candidate whose bound centre lies within a_radius of its own, at most
		// kMaxVertices vertices. Clusters of at least a_minMembers go to a_chunks; the members of smaller ones are
		// returned. The candidates are bucketed in cells of the radius, so the 3x3x3 cells around a seed's hold every
		// one in reach.
		std::vector<std::uint32_t> ClusterPass(std::span<const Member> a_members, std::vector<std::uint32_t> a_candidates, float a_radius,
			std::uint32_t a_minMembers, std::vector<Chunk>& a_chunks)
		{
			const auto cellOf = [&](const Member& a_m) {
				return Key{ static_cast<std::int32_t>(std::floor(a_m.center[0] / a_radius)), static_cast<std::int32_t>(std::floor(a_m.center[1] / a_radius)),
					static_cast<std::int32_t>(std::floor(a_m.center[2] / a_radius)) };
			};
			std::map<Key, std::vector<std::uint32_t>> cells;
			for (const auto index : a_candidates) {
				cells[cellOf(a_members[index])].push_back(index);
			}
			// Seeds in cell order, along x inside a cell: deterministic, and neighbours seed neighbours.
			std::ranges::sort(a_candidates, [&](std::uint32_t a_lhs, std::uint32_t a_rhs) {
				const auto lhs = cellOf(a_members[a_lhs]);
				const auto rhs = cellOf(a_members[a_rhs]);
				return lhs != rhs ? lhs < rhs : a_members[a_lhs].center[0] < a_members[a_rhs].center[0];
			});

			std::vector<std::uint32_t> alone;
			std::vector<bool>          taken(a_members.size(), false);
			for (const auto seed : a_candidates) {
				if (taken[seed]) {
					continue;
				}
				const auto&   s = a_members[seed];
				const auto    home = cellOf(s);
				Chunk         chunk;
				std::uint32_t vertices = 0;
				for (std::int32_t dx = -1; dx <= 1; ++dx) {
					for (std::int32_t dy = -1; dy <= 1; ++dy) {
						for (std::int32_t dz = -1; dz <= 1; ++dz) {
							const auto found = cells.find(Key{ home.x + dx, home.y + dy, home.z + dz });
							if (found == cells.end()) {
								continue;
							}
							for (const auto index : found->second) {
								const auto& m = a_members[index];
								const float d[3]{ m.center[0] - s.center[0], m.center[1] - s.center[1], m.center[2] - s.center[2] };
								if (taken[index] || d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > a_radius * a_radius || vertices + m.vertexCount > kMaxVertices) {
									continue;
								}
								taken[index] = true;
								chunk.members.push_back(index);
								vertices += m.vertexCount;
							}
						}
					}
				}
				if (chunk.members.size() >= a_minMembers) {
					std::ranges::sort(chunk.members);
					a_chunks.push_back(std::move(chunk));
				} else {
					alone.insert(alone.end(), chunk.members.begin(), chunk.members.end());
				}
			}
			return alone;
		}
	}

	std::vector<Chunk> Solos(std::span<const Member> a_members)
	{
		std::vector<Chunk> solos;
		for (std::uint32_t i = 0; i < a_members.size(); ++i) {
			solos.push_back(SoloOf(a_members, i));
		}
		return solos;
	}

	std::vector<Chunk> Cluster(std::span<const Member> a_members, float a_chunkSize, std::uint32_t a_minMembers)
	{
		std::vector<std::uint32_t> candidates;
		for (std::uint32_t i = 0; i < a_members.size(); ++i) {
			const auto& m = a_members[i];
			if (m.vertexCount != 0 && m.vertexCount <= kMaxVertices && m.triangleCount != 0) {
				candidates.push_back(i);
			}
		}
		// Compact clusters first, within half the chunk size: about the precombined chunks' typical size. The stragglers
		// get a second pass within twice the chunk size (about a cell; precombined chunks reach bound radius 5,200): left
		// as solo clones they were 10,461 of Runtime Combiner's 22,473 objects against 18,971 precombined chunks (run 20).
		std::vector<Chunk> chunks;
		auto               alone = ClusterPass(a_members, std::move(candidates), a_chunkSize * 0.5F, a_minMembers, chunks);
		alone = ClusterPass(a_members, std::move(alone), a_chunkSize * 2.0F, a_minMembers, chunks);

		std::vector<Chunk> solos;
		for (const auto index : alone) {
			solos.push_back(SoloOf(a_members, index));
		}

		// The origin is the centre of the members' bounding box: the smallest offsets, the finest position steps.
		for (auto& chunk : chunks) {
			float lo[3]{ std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
			float hi[3]{ -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max() };
			for (const auto index : chunk.members) {
				const auto& m = a_members[index];
				for (int i = 0; i < 3; ++i) {
					lo[i] = std::min(lo[i], m.center[i] - m.radius);
					hi[i] = std::max(hi[i], m.center[i] + m.radius);
				}
			}
			for (int i = 0; i < 3; ++i) {
				chunk.origin[i] = 0.5F * (lo[i] + hi[i]);
			}
		}
		std::ranges::move(solos, std::back_inserter(chunks));
		return chunks;
	}

	bool Build(const Vertex::Layout& a_from, const Vertex::Layout& a_to, std::span<const Member> a_members, Chunk& a_chunk, std::uint32_t a_minMembers)
	{
		if (a_chunk.solo) {
			a_chunk.baked = a_chunk.members;
			return true;
		}
		if (!Vertex::Convertible(a_from, a_to)) {
			return false;
		}
		std::uint32_t vertexTotal = 0;
		std::uint32_t triangleTotal = 0;
		for (const auto index : a_chunk.members) {
			vertexTotal += a_members[index].vertexCount;
			triangleTotal += a_members[index].triangleCount;
		}
		if (vertexTotal > kMaxVertices) {
			return false;
		}

		a_chunk.vertices.assign(static_cast<std::size_t>(vertexTotal) * a_to.stride, std::byte{ 0 });
		a_chunk.indices.resize(static_cast<std::size_t>(triangleTotal) * 3);
		a_chunk.baked.clear();

		std::vector<std::byte>     source;
		std::vector<std::uint16_t> sourceIndices;
		float                      lo[3]{ std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
		float                      hi[3]{ -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max() };
		std::uint32_t              vertexBase = 0;
		std::uint32_t              indexBase = 0;
		for (const auto index : a_chunk.members) {
			const auto& m = a_members[index];
			const auto  vertexBytes = static_cast<std::size_t>(m.vertexCount) * a_from.stride;
			const auto  indexCount = static_cast<std::size_t>(m.triangleCount) * 3;
			source.resize(vertexBytes);
			sourceIndices.resize(indexCount);
			if (!SafeCopy(source.data(), m.vertices, vertexBytes) || !SafeCopy(sourceIndices.data(), m.indices, indexCount * sizeof(std::uint16_t))) {
				continue;
			}
			if (std::ranges::any_of(sourceIndices, [&](std::uint16_t a_index) { return a_index >= m.vertexCount; })) {
				continue;
			}

			// Model space -> chunk space: world, minus the chunk origin.
			auto affine = m.toWorld;
			for (int i = 0; i < 3; ++i) {
				affine.translate[i] -= a_chunk.origin[i];
			}
			Vertex::Transform(a_from, source.data(), a_to, a_chunk.vertices.data() + static_cast<std::size_t>(vertexBase) * a_to.stride, m.vertexCount, affine, lo, hi);
			for (std::size_t i = 0; i < indexCount; ++i) {
				a_chunk.indices[indexBase + i] = static_cast<std::uint16_t>(sourceIndices[i] + vertexBase);
			}
			vertexBase += m.vertexCount;
			indexBase += static_cast<std::uint32_t>(indexCount);
			a_chunk.baked.push_back(index);
		}
		if (a_chunk.baked.size() < a_minMembers || vertexBase == 0) {
			a_chunk.vertices.clear();
			a_chunk.indices.clear();
			return false;
		}
		a_chunk.vertices.resize(static_cast<std::size_t>(vertexBase) * a_to.stride);
		a_chunk.indices.resize(indexBase);
		a_chunk.vertexCount = vertexBase;
		a_chunk.triangleCount = indexBase / 3;

		// Bound: the box centre, radius to the farthest written vertex.
		for (int i = 0; i < 3; ++i) {
			a_chunk.boundCenter[i] = 0.5F * (lo[i] + hi[i]);
		}
		float radius = 0.0F;
		for (int i = 0; i < 3; ++i) {
			const float half = 0.5F * (hi[i] - lo[i]);
			radius += half * half;
		}
		a_chunk.boundRadius = std::sqrt(radius);
		return true;
	}
}
