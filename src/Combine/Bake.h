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
	};

	// Grid cells of a_cellSize (by bound centre), each split so no chunk passes kMaxVertices; chunks with fewer
	// than a_minMembers members are dropped (their meshes stay as they are).
	[[nodiscard]] std::vector<Chunk> Cluster(std::span<const Member> a_members, float a_cellSize, std::uint32_t a_minMembers);

	// Writes the chunk's data: the members' vertices (a_from) in the chunk's format (a_to, Vertex::Convertible).
	// False when fewer than a_minMembers members could be written.
	[[nodiscard]] bool Build(const Vertex::Layout& a_from, const Vertex::Layout& a_to, std::span<const Member> a_members, Chunk& a_chunk, std::uint32_t a_minMembers);
}
