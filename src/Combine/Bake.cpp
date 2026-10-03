#include "Combine/Bake.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
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
	}

	std::vector<Chunk> Cluster(std::span<const Member> a_members, float a_cellSize, std::uint32_t a_minMembers)
	{
		std::vector<std::pair<Key, std::uint32_t>> keyed;
		keyed.reserve(a_members.size());
		for (std::uint32_t i = 0; i < a_members.size(); ++i) {
			const auto& m = a_members[i];
			if (m.vertexCount == 0 || m.vertexCount > kMaxVertices || m.triangleCount == 0) {
				continue;
			}
			keyed.push_back({ Key{ static_cast<std::int32_t>(std::floor(m.center[0] / a_cellSize)),
								  static_cast<std::int32_t>(std::floor(m.center[1] / a_cellSize)),
								  static_cast<std::int32_t>(std::floor(m.center[2] / a_cellSize)) },
				i });
		}
		std::ranges::sort(keyed, [&](const auto& a_lhs, const auto& a_rhs) {
			if (a_lhs.first != a_rhs.first) {
				return a_lhs.first < a_rhs.first;
			}
			// Within a grid cell, neighbours along x end up in the same split.
			return a_members[a_lhs.second].center[0] < a_members[a_rhs.second].center[0];
		});

		std::vector<Chunk> chunks;
		Chunk              current;
		std::uint32_t      vertices = 0;
		Key                currentKey{};
		const auto         flush = [&]() {
            if (current.members.size() >= a_minMembers) {
                chunks.push_back(std::move(current));
            }
            current = Chunk{};
            vertices = 0;
		};
		for (const auto& [key, index] : keyed) {
			const auto count = a_members[index].vertexCount;
			if (!current.members.empty() && (key != currentKey || vertices + count > kMaxVertices)) {
				flush();
			}
			currentKey = key;
			current.members.push_back(index);
			vertices += count;
		}
		if (!current.members.empty()) {
			flush();
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
		return chunks;
	}

	bool Build(const Vertex::Layout& a_from, const Vertex::Layout& a_to, std::span<const Member> a_members, Chunk& a_chunk, std::uint32_t a_minMembers)
	{
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
