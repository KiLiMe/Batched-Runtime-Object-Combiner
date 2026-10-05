#include "Combine/Gather.h"

#include "Engine/Engine.h"
#include "Settings.h"

namespace RC::Combine::Gather
{
	namespace
	{
		using namespace Engine;

		constexpr std::uint32_t kFormDeleted = 1u << 5;
		constexpr std::uint32_t kFormDisabled = 1u << 11;

		std::atomic<std::uint64_t> g_agree{ 0 };
		std::atomic<std::uint64_t> g_disagree{ 0 };

		[[nodiscard]] bool IsIdentity(const RE::NiTransform& a_transform) noexcept
		{
			for (int i = 0; i < 3; ++i) {
				for (int j = 0; j < 3; ++j) {
					if (std::abs(a_transform.rotate.entry[i].pt[j] - (i == j ? 1.0F : 0.0F)) > 1e-4F) {
						return false;
					}
				}
			}
			return std::abs(a_transform.scale - 1.0F) <= 1e-4F;
		}

		// FO4 rotates a point by the transpose of the stored rows: x'_i = s * sum_j R[j][i] x_j + t_i
		// (NiBound::Update 0x141BB19A0, FO4-ENGINE-NOTES 7.2). The other reading is only used to tell them apart.
		void ToWorld(const RE::NiTransform& a_t, const float a_p[3], float a_out[3], bool a_transposed) noexcept
		{
			const auto& r = a_t.rotate.entry;
			const float t[3]{ a_t.translate.x, a_t.translate.y, a_t.translate.z };
			for (int i = 0; i < 3; ++i) {
				const float rotated = a_transposed ?
				                          r[0].pt[i] * a_p[0] + r[1].pt[i] * a_p[1] + r[2].pt[i] * a_p[2] :
				                          r[i].pt[0] * a_p[0] + r[i].pt[1] * a_p[1] + r[i].pt[2] * a_p[2];
				a_out[i] = a_t.scale * rotated + t[i];
			}
		}

		[[nodiscard]] float Distance(const float a_a[3], const RE::NiPoint3& a_b) noexcept
		{
			const float dx = a_a[0] - a_b.x;
			const float dy = a_a[1] - a_b.y;
			const float dz = a_a[2] - a_b.z;
			return std::sqrt(dx * dx + dy * dy + dz * dz);
		}

		[[nodiscard]] Vertex::Affine ModelToWorld(const RE::NiTransform& a_t) noexcept
		{
			Vertex::Affine affine;
			const auto&    r = a_t.rotate.entry;
			for (int i = 0; i < 3; ++i) {
				for (int j = 0; j < 3; ++j) {
					affine.rotation[i][j] = r[j].pt[i];
					affine.linear[i][j] = a_t.scale * r[j].pt[i];
				}
			}
			affine.translate[0] = a_t.translate.x;
			affine.translate[1] = a_t.translate.y;
			affine.translate[2] = a_t.translate.z;
			return affine;
		}

		[[nodiscard]] std::uint64_t Mix(std::uint64_t a_hash, std::uint64_t a_value) noexcept
		{
			return a_hash ^ (a_value + 0x9E3779B97F4A7C15ull + (a_hash << 6) + (a_hash >> 2));
		}

		[[nodiscard]] bool CulledByGame(const RE::NiAVObject* a_object, const HiddenSet& a_ours)
		{
			return a_object->GetAppCulled() && !a_ours.contains(a_object);
		}

		// A mesh that passed every check, not yet in a bucket.
		struct Candidate
		{
			RE::NiAVObject* shape{ nullptr };
			const void*     rendererData{ nullptr };
			std::uint64_t   desc{ 0 };
			Vertex::Layout  layout;
			std::uint64_t   chunkDesc{ 0 };
			Vertex::Layout  chunkLayout;
			void*           property{ nullptr };
			std::uint64_t   shaderFlags{ 0 };
			std::uint32_t   alphaKey{ 0 };
			std::uint64_t   objectKey{ 0 };
			bool            cloneOnly{ false };  // drawn by a clone of it (a solo chunk), never merged
			Bake::Member    member;
		};

		// One reference's 3D, scanned before anything is committed.
		struct Scan
		{
			std::vector<Candidate> candidates;
			bool                   blocked{ false };  // a part renders on its own: the root can't be hidden
		};

		// How a mesh can be replaced.
		enum class Fit
		{
			kKeep,   // it renders on its own: it stays visible, and its reference can't be hidden whole
			kClone,  // a clone of it, sharing its data and properties, draws it in its place
			kMerge,  // its vertices can go into a combined mesh
		};

		Fit Check(RE::NiAVObject* a_shape, Stats& a_stats, const Settings& a_settings, Candidate& a_out)
		{
			if (*Field<void*>(a_shape, Offset::kSkinInstance)) {
				a_stats.Count(Skip::kSkinned);
				return Fit::kKeep;
			}
			if (a_shape->collisionObject) {
				a_stats.Count(Skip::kCollision);
				return Fit::kKeep;
			}
			const auto rendererData = *Field<std::byte*>(a_shape, Offset::kRendererData);
			if (!rendererData) {
				a_stats.Count(Skip::kNoData);
				return Fit::kKeep;
			}
			// The previs feeds route by bit 40, and Addictol's drops pool records that have it (FO4-ENGINE-NOTES 5.5b):
			// a chunk of such meshes would not be drawn while previs feeds the main view.
			if ((a_shape->GetFlags() >> 40) & 1) {
				a_stats.Count(Skip::kNonCaster);
				return Fit::kKeep;
			}
			const auto property = *Field<void*>(a_shape, Offset::kShaderProperty);
			if (!property || !(IsLightingShader(property) || IsEffectShader(property))) {
				a_stats.Count(Skip::kShader);
				return Fit::kKeep;
			}
			const auto shaderFlags = *Field<std::uint64_t>(property, Offset::kShaderFlags);
			if (shaderFlags & ShaderFlag::kNotCombinable) {
				a_stats.Count(Skip::kShaderFlags);
				return Fit::kKeep;
			}
			const auto& worldBound = a_shape->worldBound;
			if (!std::isfinite(worldBound.fRadius) || worldBound.fRadius > a_settings.maxShapeRadius ||
				!std::isfinite(worldBound.center.x) || !std::isfinite(worldBound.center.y) || !std::isfinite(worldBound.center.z)) {
				a_stats.Count(Skip::kTooBig);
				return Fit::kKeep;
			}

			// The engine's own world bound must come out of the transform the bake will use.
			const auto& modelBound = *Field<RE::NiBound>(a_shape, Offset::kModelBound);
			const float center[3]{ modelBound.center.x, modelBound.center.y, modelBound.center.z };
			float       transposed[3];
			float       straight[3];
			ToWorld(a_shape->world, center, transposed, true);
			ToWorld(a_shape->world, center, straight, false);
			const float tolerance = 0.05F + 1e-4F * (std::abs(worldBound.center.x) + std::abs(worldBound.center.y) + std::abs(worldBound.center.z));
			const bool  matches = Distance(transposed, worldBound.center) <= tolerance;
			const bool  matchesOther = Distance(straight, worldBound.center) <= tolerance;
			if (!matches) {
				if (matchesOther) {
					g_disagree.fetch_add(1, std::memory_order_relaxed);
				}
				a_stats.Count(Skip::kTransform);
				return Fit::kKeep;
			}
			if (!matchesOther) {
				g_agree.fetch_add(1, std::memory_order_relaxed);
			}

			a_out.shape = a_shape;
			a_out.rendererData = rendererData;
			a_out.property = property;
			a_out.shaderFlags = shaderFlags;
			a_out.objectKey = (a_shape->GetFlags() >> 40) & 0x3;
			auto& member = a_out.member;
			member.toWorld = ModelToWorld(a_shape->world);
			member.center[0] = worldBound.center.x;
			member.center[1] = worldBound.center.y;
			member.center[2] = worldBound.center.z;
			member.radius = worldBound.fRadius;
			member.triangleCount = *Field<std::uint32_t>(a_shape, Offset::kTriangles);

			// From here a mesh that can't be merged is drawn by a clone of it (a solo chunk), so its reference can still
			// be hidden whole and leave previs's dynamic objects (FO4-ENGINE-NOTES 5.5d). Not when a property animates:
			// the clone gets a copy of it (Engine::Clone), and nothing would run the copy's controller.
			const auto alpha = *Field<std::byte*>(a_shape, Offset::kAlphaProperty);
			const auto clone = [&](Skip a_skip) {
				if (*Field<void*>(property, Offset::kControllers) || (alpha && *Field<void*>(alpha, Offset::kControllers))) {
					a_stats.Count(a_skip);
					return Fit::kKeep;
				}
				a_stats.CountCloned(a_skip);
				a_out.cloneOnly = true;
				return Fit::kClone;
			};
			if (!IsLightingShader(property)) {
				return clone(Skip::kShader);
			}
			const auto     desc = *Field<std::uint64_t>(rendererData, 0);
			Vertex::Layout layout;
			std::uint64_t  chunkDesc = 0;
			Vertex::Layout chunkLayout;
			if (!Vertex::Decode(desc, layout)) {
				return clone(Skip::kFormat);
			}
			if (!Vertex::FullPrecision(desc, chunkDesc) || !Vertex::Decode(chunkDesc, chunkLayout) || !Vertex::Convertible(layout, chunkLayout)) {
				return clone(Skip::kStride);
			}
			const auto vertexBuffer = *Field<std::byte*>(rendererData, Offset::kVertexBuffer);
			const auto indexBuffer = *Field<std::byte*>(rendererData, Offset::kIndexBuffer);
			if (!vertexBuffer || !indexBuffer) {
				return clone(Skip::kNoCpuCopy);
			}
			const auto vertices = *Field<std::byte*>(vertexBuffer, Offset::kBufferData);
			const auto vertexBytes = *Field<std::uint32_t>(vertexBuffer, Offset::kBufferSize);
			const auto indices = *Field<std::uint16_t*>(indexBuffer, Offset::kBufferData);
			const auto indexBytes = *Field<std::uint32_t>(indexBuffer, Offset::kBufferSize);
			const auto vertexCount = static_cast<std::uint32_t>(*Field<std::uint16_t>(a_shape, Offset::kVertices));
			auto       triangleCount = member.triangleCount;
			if (IsExactMeshLODTriShape(a_shape)) {
				// Full detail, what it draws near the camera: the first lod0 + lod1 + lod2 triangles (FO4-ENGINE-NOTES 7.11).
				const auto lodSizes = Field<std::uint32_t>(a_shape, Offset::kLODSizes);
				const auto full = static_cast<std::uint64_t>(lodSizes[0]) + lodSizes[1] + lodSizes[2];
				triangleCount = full <= triangleCount ? static_cast<std::uint32_t>(full) : 0;
			}
			// The buffer must hold exactly this mesh's vertices, and at least the triangles it draws.
			if (!vertices || !indices || vertexCount == 0 || triangleCount == 0 ||
				static_cast<std::uint64_t>(vertexCount) * layout.stride != vertexBytes ||
				static_cast<std::uint64_t>(triangleCount) * 6 > indexBytes) {
				return clone(Skip::kNoCpuCopy);
			}
			// The persistent alpha, not the property's +0x28: that is a draw-time cache of material alpha x fade,
			// stale for any mesh last drawn mid-fade (FO4-ENGINE-NOTES 7.8). A load fade-in is no reason to leave a
			// mesh out: the chunk is drawn at full alpha at once, or with bChunkFadeNodes its fade node carries the
			// fade-in on (Manager's Apply).
			const auto material = *Field<std::byte*>(property, Offset::kShaderMaterial);
			if (!material || *Field<float>(material, Offset::kMaterialAlpha) < 1.0F || *Field<float>(property, Offset::kShaderLODFade) != 1.0F) {
				return clone(Skip::kShaderAlpha);
			}
			std::uint32_t alphaKey = 0;
			if (alpha) {
				const auto alphaFlags = *Field<std::uint16_t>(alpha, Offset::kAlphaFlags);
				if (alphaFlags & 1) {
					return clone(Skip::kAlphaBlend);
				}
				alphaKey = 0x80000000u | (static_cast<std::uint32_t>(alphaFlags) << 8) | *Field<std::uint8_t>(alpha, Offset::kAlphaThreshold);
			}

			a_out.desc = desc;
			a_out.layout = layout;
			a_out.chunkDesc = chunkDesc;
			a_out.chunkLayout = chunkLayout;
			a_out.alphaKey = alphaKey;
			member.vertices = vertices;
			member.vertexCount = vertexCount;
			member.indices = indices;
			member.triangleCount = triangleCount;
			return Fit::kMerge;
		}

		void Walk(RE::NiAVObject* a_object, const HiddenSet& a_ours, Stats& a_stats, const Settings& a_settings, Scan& a_scan)
		{
			if (!a_object || CulledByGame(a_object, a_ours)) {
				return;  // hidden parts stay hidden either way
			}
			if (a_object->controllers) {
				a_stats.CountBlocked("animated (a controller)");
				a_scan.blocked = true;
				return;
			}
			if (IsExactNode(a_object)) {
				// A fade node culls through its flattened geometry list (BSFadeNode::geomArray), rebuilt when its
				// flag bit 29 is set; SetAppCulled sets that bit up the parents and the rebuild marks culled meshes,
				// so hiding mesh by mesh works there too (FO4-ENGINE-NOTES 7.6).
				for (auto& child : static_cast<RE::NiNode*>(a_object)->children) {
					Walk(child.get(), a_ours, a_stats, a_settings, a_scan);
				}
				return;
			}
			if (IsExactTriShape(a_object) || IsExactMeshLODTriShape(a_object)) {
				Candidate candidate;
				if (Check(a_object, a_stats, a_settings, candidate) != Fit::kKeep) {
					a_scan.candidates.push_back(candidate);
				} else {
					a_scan.blocked = true;
				}
				return;
			}
			a_stats.CountBlocked(ClassName(a_object));
			a_scan.blocked = true;
		}

		// The reference's distance-fade class (FO4-ENGINE-NOTES 7.9). One that starts fading beyond the loaded grid's
		// reach never fades while its cell is loaded: kNeverFades, merged freely, its chunk never fades. Others: the
		// fade type and the far distance in steps of about 19% (2^(1/4)); a chunk's fade node takes the furthest
		// range of its class, so it fades out where its members would, at most a step later. 0 = no range read.
		constexpr std::uint32_t kNeverFades = 0xFFFFFFFF;

		[[nodiscard]] std::uint32_t FadeKey(const FadeRange& a_range, float a_reach) noexcept
		{
			if (!a_range.Valid()) {
				return 0;
			}
			if (FadeStartDistance(a_range) >= a_reach) {
				return kNeverFades;
			}
			const auto step = static_cast<std::uint32_t>(std::clamp(std::lround(std::log2(std::max(a_range.farDistance, 1.0F)) * 4.0F), 1L, 0xFFFFL));
			return (static_cast<std::uint32_t>(a_range.type) << 16) | step;
		}

		// Bucket: the engine's merge test (BSLightingShaderProperty::CanMerge) plus what the bake must keep equal.
		// The hash narrows the CanMerge calls to properties with the same flags and material name.
		Bucket& BucketFor(const Candidate& a_candidate, RE::NiNode* a_parent, std::uint32_t a_fadeKey, const FadeRange& a_fade, Job& a_job,
			std::unordered_map<std::uint64_t, std::vector<std::uint32_t>>& a_index)
		{
			const auto fadeKey = a_fadeKey;
			if (a_candidate.cloneOnly) {
				// Never merged: one bucket per parent and fade class, for the chunks' fade nodes.
				auto& list = a_index[Mix(Mix(reinterpret_cast<std::uintptr_t>(a_parent), fadeKey), 0xC1)];
				for (const auto index : list) {
					auto& bucket = a_job.buckets[index];
					if (bucket.cloneOnly && bucket.parent == a_parent && bucket.fadeKey == fadeKey) {
						bucket.fadeNear = std::max(bucket.fadeNear, a_fade.nearDistance);
						bucket.fadeFar = std::max(bucket.fadeFar, a_fade.farDistance);
						return bucket;
					}
				}
				list.push_back(static_cast<std::uint32_t>(a_job.buckets.size()));
				auto& bucket = a_job.buckets.emplace_back();
				bucket.parent = a_parent;
				bucket.property = a_candidate.property;
				bucket.cloneOnly = true;
				bucket.fadeKey = fadeKey;
				bucket.fadeNear = a_fade.nearDistance;
				bucket.fadeFar = a_fade.farDistance;
				bucket.fadeType = a_fade.type;
				return bucket;
			}
			const auto name = *Field<std::uintptr_t>(a_candidate.property, 0x10);
			auto       key = Mix(reinterpret_cast<std::uintptr_t>(a_parent), a_candidate.desc);
			key = Mix(key, a_candidate.shaderFlags);
			key = Mix(key, name);
			key = Mix(key, a_candidate.alphaKey);
			key = Mix(key, a_candidate.objectKey);
			key = Mix(key, fadeKey);
			auto& list = a_index[key];
			for (const auto index : list) {
				auto& bucket = a_job.buckets[index];
				// CanMerge leaves out what BSLightingShaderProperty::CopyMembers also copies at +0x70..+0x8C (eight
				// floats), and the chunk draws with one member's property: those must match too.
				if (!bucket.cloneOnly && bucket.parent == a_parent && bucket.desc == a_candidate.desc && bucket.alphaKey == a_candidate.alphaKey &&
					bucket.objectKey == a_candidate.objectKey && bucket.fadeKey == fadeKey &&
					std::memcmp(Field(bucket.property, 0x70), Field(a_candidate.property, 0x70), 0x20) == 0 &&
					CanMerge(bucket.property, a_candidate.property)) {
					bucket.fadeNear = std::max(bucket.fadeNear, a_fade.nearDistance);
					bucket.fadeFar = std::max(bucket.fadeFar, a_fade.farDistance);
					return bucket;
				}
			}
			list.push_back(static_cast<std::uint32_t>(a_job.buckets.size()));
			auto& bucket = a_job.buckets.emplace_back();
			bucket.parent = a_parent;
			bucket.desc = a_candidate.desc;
			bucket.layout = a_candidate.layout;
			bucket.chunkDesc = a_candidate.chunkDesc;
			bucket.chunkLayout = a_candidate.chunkLayout;
			bucket.property = a_candidate.property;
			bucket.alphaKey = a_candidate.alphaKey;
			bucket.objectKey = a_candidate.objectKey;
			bucket.fadeKey = fadeKey;
			bucket.fadeNear = a_fade.nearDistance;
			bucket.fadeFar = a_fade.farDistance;
			bucket.fadeType = a_fade.type;
			return bucket;
		}
	}

	void Cell(RE::TESObjectCELL* a_cell, const HiddenSet& a_ours, const FormSet& a_meshByMesh, Job& a_job)
	{
		const auto start = std::chrono::steady_clock::now();
		const auto& settings = Settings::Get();
		auto&       stats = a_job.stats;

		std::vector<RE::NiPointer<RE::TESObjectREFR>> references;
		a_cell->spinLock.lock();
		references.assign(a_cell->references.begin(), a_cell->references.end());
		a_cell->spinLock.unlock();
		stats.references = static_cast<std::uint32_t>(references.size());

		std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> index;
		Scan                                                          scan;
		const float                                                   reach = LoadedReach();
		const auto                                                    combinedRefs = CombinedRefs(a_cell);
		for (const auto& pointer : references) {
			const auto ref = pointer.get();
			if (!ref) {
				continue;
			}
			const auto base = ref->GetObjectReference();
			if (!base || !(base->Is(RE::ENUM_FORM_ID::kSTAT) || base->Is(RE::ENUM_FORM_ID::kSCOL))) {
				stats.Count(Skip::kRefType);
				continue;
			}
			if (ref->IsCreated()) {
				stats.Count(Skip::kRefCreated);
				continue;
			}
			if (ref->GetFormFlags() & (kFormDeleted | kFormDisabled)) {
				stats.Count(Skip::kRefDisabled);
				continue;
			}
			const auto loaded = ref->loadedData;
			const auto root = loaded ? loaded->data3D.get() : nullptr;
			if (!root) {
				stats.Count(Skip::kRefNo3D);
				continue;
			}
			if (CulledByGame(root, a_ours)) {
				stats.Count(Skip::kRefHidden);
				continue;
			}
			if (root->fadeAmount != 1.0F) {
				stats.Count(Skip::kRefFaded);
				continue;
			}
			const auto parent = root->parent;
			if (!parent || !IsIdentity(parent->world)) {
				stats.Count(Skip::kRefParent);
				continue;
			}
			if (!IsPlainNode(parent)) {
				stats.Count(Skip::kRefParentType);
				continue;
			}

			scan.candidates.clear();
			scan.blocked = a_meshByMesh.contains(ref->GetFormID());
			Walk(root, a_ours, stats, settings, scan);
			if (scan.candidates.empty()) {
				continue;
			}

			const auto refIndex = static_cast<std::uint32_t>(a_job.refs.size());
			auto&      source = a_job.refs.emplace_back();
			source.ref = pointer;
			source.formID = ref->GetFormID();
			source.root.reset(root);
			source.rootWorld = root->world;
			source.parent = parent;
			source.whole = !scan.blocked;
			source.captured = static_cast<std::uint32_t>(scan.candidates.size());
			// A reference the cell's precombines would draw has no fade of its own there: its chunk fades as a
			// precombined chunk does, from uGridsToLoad (fade class 0, FO4-ENGINE-NOTES 7.9). One class for all of them
			// lets same-material meshes merge as the precombines do; per-reference classes split them into ~20% more
			// objects, the extra in the sun's cascades (run 19).
			const bool precombined = InCombinedRefs(combinedRefs, ref->GetFormID());
			auto       fade = precombined ? FadeRange{} : ReadFadeRange(root);
			const auto fadeKey = precombined ? 0 : FadeKey(fade, reach);
			if (fadeKey == kNeverFades) {
				fade = { std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), 0 };
			}
			stats.precombinedRefs += precombined ? 1 : 0;
			for (const auto& candidate : scan.candidates) {
				auto& bucket = BucketFor(candidate, parent, fadeKey, fade, a_job, index);
				bucket.shapes.push_back(static_cast<std::uint32_t>(a_job.shapes.size()));
				bucket.members.push_back(candidate.member);
				auto& shape = a_job.shapes.emplace_back();
				shape.shape.reset(candidate.shape);
				shape.ref = refIndex;
				shape.rendererData = candidate.rendererData;
				shape.world = candidate.shape->world;
			}
			++stats.candidates;
		}
		stats.capturedShapes = static_cast<std::uint32_t>(a_job.shapes.size());
		stats.gatherMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	}

	ConventionVotes Votes() noexcept
	{
		return { g_agree.load(std::memory_order_relaxed), g_disagree.load(std::memory_order_relaxed) };
	}
}
