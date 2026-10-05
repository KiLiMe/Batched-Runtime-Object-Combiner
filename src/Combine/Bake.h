#pragma once

// Engine-free: groups source meshes into chunks and writes each chunk's combined vertex and index data.
// Built into the offline test as well.

#include "Combine/Vertex.h"

#include <span>
#include <vector>

namespace RC::Bake
{
	inline constexpr std::uint32_t kMaxVertices = 0xFFFF;  // 16-bit indices

	// One source mesh as the main thread captured it. The pointers are the engine's CPU copies; the main thread
	// holds a reference on the mesh until the bake is finished.
	struct Member
	{
		const std::byte*     vertices{ nullptr };
		std::uint32_t        vertexCount{ 0 };
		const std::uint16_t* indices{ nullptr };
		std::uint32_t        triangleCount{ 0 };
		Vertex::Affine       toWorld;     // model space -> world space
		float                center[3]{};  // world bound
		float                radius{ 0.0F };
	};

	struct Chunk
	{
		// Input: the members drawn by this chunk (indices into the bucket's member list) and the world point its
		// vertices are written relative to (the chunk's local translate).
		std::vector<std::uint32_t> members;
		float                      origin[3]{};

		// Output.
		std::vector<std::byte>     vertices;
		std::vector<std::uint16_t> indices;
		std::uint32_t              vertexCount{ 0 };
		std::uint32_t              triangleCount{ 0 };
		float                      boundCenter[3]{};  // relative to origin
		float                      boundRadius{ 0.0F };
		std::vector<std::uint32_t> baked;  // members actually written (one whose data failed the checks is left out)
		// One member drawn as it is (a clone of its mesh sharing its data): no vertex data written. It still
		// replaces its original, so the reference can be hidden whole.
		bool solo{ false };
	};

	// Compact clusters: each chunk holds the members whose bound centres lie within a_chunkSize / 2 of its seed's,
	// at most kMaxVertices vertices. The members of clusters smaller than a_minMembers get a second pass within
	// 2 x a_chunkSize; one still alone gives a solo chunk (origin: its bound centre). Solos come after the others.
	[[nodiscard]] std::vector<Chunk> Cluster(std::span<const Member> a_members, float a_chunkSize, std::uint32_t a_minMembers);

	// One solo chunk per member, whatever its data: members that can't merge, drawn by clones of their meshes.
	[[nodiscard]] std::vector<Chunk> Solos(std::span<const Member> a_members);

	// Writes the chunk's data: the members' vertices (a_from) in the chunk's format (a_to, Vertex::Convertible).
	// False when fewer than a_minMembers members could be written. A solo chunk only gets baked = members.
	[[nodiscard]] bool Build(const Vertex::Layout& a_from, const Vertex::Layout& a_to, std::span<const Member> a_members, Chunk& a_chunk, std::uint32_t a_minMembers);
}
