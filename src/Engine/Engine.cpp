#include "Engine/Engine.h"

#include <unordered_set>

namespace RC::Engine
{
	namespace
	{
		// Ids. Every one is declared with the runtime families that carry it:
		//   REL::ID{ og, ae }            - this codebase's two-slot form (NG shares the AE value)
		//   REL::ID{ og, INVALID_ID }    - OG only: the AE family has no id for it yet (see tools/ae/README.md)
		// Ids still marked OG-only were read from the 1.10.163 executable; the AE ones come from the AE-only
		// CommonLibF4 (D:\Sou\Fallout_4\CommonLibF4) and each was checked against the Runtime Database with
		// tools/ae/rd_probe.py before being filled in here.
		constexpr std::uint64_t kCloneID = 604942;             // NiObject::Clone(void)                 0x141B94B00
		constexpr std::uint64_t kCreateTriShapeID = 99624;     // Renderer::CreateTriShape(uint&, void*, u64, u16*, uint) 0x141D0BC70
		constexpr std::uint64_t kDecRefTriShapeID = 1039714;   // Renderer::DecRef(TriShape*)          0x141D0C5D0
		constexpr std::uint64_t kRendererID = 1378294;         // the BSGraphics::Renderer object       0x1461E0900
		constexpr std::uint64_t kSetRendererDataID = 444114;   // BSGeometry::SetRendererData(void*)   0x141BB15E0
		constexpr std::uint64_t kNodeCtorID = 20633;           // NiNode::NiNode(uint)                 0x141B98920
		constexpr std::uint64_t kFadeNodeCtorID = 668955;      // BSFadeNode::BSFadeNode()             0x1427D04C0
		constexpr std::uint64_t kFadeRangeID = 1417061;        // ConfigureFadeNodeRange(BSFadeNode*, float uGrids, bool) 0x1401CFAC0
		constexpr std::uint64_t kUGridsToLoadID = 504589;      // uGridsToLoad:General value (int)     0x1436D9B50
		constexpr std::uint64_t kFadeFrameID = 734919;         // the fade code's frame number (int)   0x1438C4E6C
		constexpr std::uint64_t kUpdateID = 121052;            // NiAVObject::Update(NiUpdateData&)    0x141BA3BE0
		constexpr std::uint64_t kUseCombinedID = 267057;       // bUseCombinedObjects:General value    0x1436E9C00
		constexpr std::uint64_t kPrevisEnabledID = 652211;     // BSPreCulledObjects::QWantEnabled          0x142809E60
		constexpr std::uint64_t kPrevisActiveID = 917969;      // BSPreCulledObjects::QEnabled             0x142809E30
		constexpr std::uint64_t kSetRangeID = 1164193;         // BSFadeNode::SetRange(near, far)          0x1427D1CF0
		constexpr std::uint64_t kFadeMultsID = 804710;         // float[]: fade-out multiplier per LOD-mult type 0x1438C4E18
		constexpr std::uint64_t kFadeDistMultID = 515159;      // float: fDistanceMultiplier:LOD           0x1438C4E64
		constexpr std::uint64_t kFadingOnID = 1220201;         // byte: fading enabled                     0x1438C4E44
		// The previs query and its dynamic objects (FO4-ENGINE-NOTES 5.5b, 5.5d).
		constexpr std::uint64_t kRenderPreUIID = 984743;       // DrawWorld::Render_PreUI                  0x142857480
		constexpr std::size_t   kPrevisQuerySite = 0x80;       //   call 1264353 (E8 rel32)
		constexpr std::uint64_t kPrevisQueryID = 1264353;      // the per-frame previs query (if active)   0x14284F1D0
		constexpr std::uint64_t kVisibilityID = 787073;        // MultiCellVisibilityData* (global)        0x1458D0AA8
		constexpr std::uint64_t kRegisterDynamicID = 272586;   // MultiCellVisibilityData::RegisterDynamicObject   0x1427AF460
		constexpr std::uint64_t kUnregisterDynamicID = 697930; // MultiCellVisibilityData::UnregisterDynamicObject 0x1427AF590
		constexpr std::uint32_t kCombinedRefsExtra = 0xC5;     // ExtraCombinedRefs (vtable 130663)
		// A save load clears every cell, then purges the cell buffer (FO4-ENGINE-NOTES 7.12).
		constexpr std::uint64_t kGameResetID = 124452;         // Main::PerformGameReset                   0x140D3B800
		constexpr std::size_t   kGameResetPurgeSite = 0x2ED;   //   call 1075115 (E8 rel32), after TES::ClearAllCells
		constexpr std::uint64_t kPurgeCellsID = 1075115;       // TES::PurgeBufferedCells                  0x1400F70A0
		constexpr std::size_t   kCanMergeSlot = 0x31;          // BSShaderProperty::CanMerge(BSShaderProperty*)

		// Ids with an AE slot. Sourced from the AE-only CommonLibF4 (RE::ID::...) and verified against the
		// Runtime Database for 1.11.240 by tools/ae/sim_init.py.
		constexpr REL::ID kTES{ 1194835, 2698044 };  // TES* singleton: RE::ID::TES::Singleton
		constexpr REL::ID kGridGet{ 1330136, 2194566 };  // GridCellArray::Get(x, y): RE::ID::GridCellArray::Get

		// TES and the exterior cell grid (layouts from CBRO's Compat.h).
		struct GridCellArray
		{
			void*         vtable;     // 00
			std::int32_t  centerX;    // 08
			std::int32_t  centerY;    // 0C
			std::uint32_t dimension;  // 10
		};
		struct TES
		{
			std::uint8_t       pad000[0x18];
			GridCellArray*     gridCells;     // 018
			std::uint8_t       pad020[0x58 - 0x20];
			RE::TESObjectCELL* interiorCell;  // 058
		};
		static_assert(offsetof(TES, gridCells) == 0x18 && offsetof(TES, interiorCell) == 0x58);

		struct Addresses
		{
			std::uintptr_t clone{ 0 };
			std::uintptr_t createTriShape{ 0 };
			std::uintptr_t decRefTriShape{ 0 };
			std::uintptr_t renderer{ 0 };
			std::uintptr_t setRendererData{ 0 };
			std::uintptr_t nodeCtor{ 0 };
			std::uintptr_t fadeNodeCtor{ 0 };
			std::uintptr_t fadeRange{ 0 };
			std::uintptr_t uGrids{ 0 };
			std::uintptr_t fadeFrame{ 0 };
			std::uintptr_t update{ 0 };
			std::uintptr_t tes{ 0 };
			std::uintptr_t gridGet{ 0 };
			std::uintptr_t useCombined{ 0 };
			bool           settingCollection{ false };  // the GameSettingCollection singleton resolved
			std::uintptr_t previsEnabled{ 0 };
			std::uintptr_t previsActive{ 0 };
			std::uintptr_t setRange{ 0 };
			std::uintptr_t fadeMults{ 0 };
			std::uintptr_t fadeDistMult{ 0 };
			std::uintptr_t fadingOn{ 0 };
			std::uintptr_t previsQuerySite{ 0 };
			std::uintptr_t previsQuery{ 0 };
			std::uintptr_t visibility{ 0 };
			std::uintptr_t registerDynamic{ 0 };
			std::uintptr_t unregisterDynamic{ 0 };
			bool           previsFeed{ false };  // every previs address resolved
			std::uintptr_t triShapeVtable{ 0 };
			std::uintptr_t meshLODTriShapeVtable{ 0 };
			std::uintptr_t nodeVtable{ 0 };
			std::uintptr_t fadeNodeVtable{ 0 };
			std::uintptr_t lightingShaderVtable{ 0 };
			std::uintptr_t effectShaderVtable{ 0 };
		};
		Addresses g;
		Capabilities g_caps;

		// Ids Init() could not resolve, in the order they were asked for. Reported as one line so a runtime
		// that carries none of them (the AE family) does not flood the log with one warning per id.
		std::vector<const char*> g_missing;

		// Looks an id up without ever failing the process: REL::ID::address() calls report_and_fail (a fatal
		// error box) when the id does not resolve, which is exactly what happens on a runtime whose slot for
		// this id is not in the database. On such a runtime the id is simply reported as absent, so the
		// plugin can carry on with fewer capabilities. A miss is recorded, not logged.
		[[nodiscard]] std::uintptr_t Lookup(const REL::ID& a_id, const char* a_name)
		{
			const auto& version = REL::Module::get().version();
			if (a_id.id(version) == REL::ID::INVALID_ID) {
				g_missing.push_back(a_name);
				return 0;
			}
			const auto result = REL::IDDatabase::get().resolve(a_id);
			if (!result.rva) {
				logger::warn("engine id {} ({}) did not resolve: {}", a_id.id(version), a_name, REL::id_resolve_status_text(result.status));
				g_missing.push_back(a_name);
				return 0;
			}
			return REL::Module::get().base() + *result.rva;
		}

		// The raw read, kept free of any object that needs unwinding so __try is allowed (C2712).
		[[nodiscard]] bool ReadSettingCollectionPresent() noexcept
		{
			__try {
				return RE::GameSettingCollection::GetSingleton() != nullptr;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		// Whether the GameSettingCollection singleton can actually be reached. This is the one probe that has
		// to run code, not just resolve an id, so it is guarded: the singleton pointer is read only after the
		// id resolved, and any fault leaves the capability off rather than taking the process down. Whether
		// bUseCombinedObjects itself is present is settled by the first read or write, which logs when not.
		[[nodiscard]] bool ProbeSettingCollection()
		{
			// The SDK declares the singleton as REL::ID(8308, 4797590); ask the database for the running
			// family's slot the same non-fatal way Lookup() does.
			constexpr REL::ID kSingleton{ 8308, 4797590 };
			const auto&       version = REL::Module::get().version();
			if (kSingleton.id(version) == REL::ID::INVALID_ID) {
				return false;
			}
			if (!REL::IDDatabase::get().resolve(kSingleton).rva) {
				return false;
			}
			const bool present = ReadSettingCollectionPresent();
			if (!present) {
				logger::warn("the GameSettingCollection singleton could not be read: the precombine switch is unavailable");
			}
			return present;
		}

		[[nodiscard]] std::uintptr_t Ask(std::uint64_t a_id, const char* a_name)
		{
			return Lookup(REL::ID{ a_id, REL::ID::INVALID_ID }, a_name);
		}

		// For ids that already carry an AE slot (REL::ID{og, ae}).
		[[nodiscard]] std::uintptr_t Ask(const REL::ID& a_id, const char* a_name)
		{
			return Lookup(a_id, a_name);
		}

		[[nodiscard]] std::uintptr_t AskVtable(std::span<const REL::ID> a_ids, const char* a_name)
		{
			if (a_ids.empty()) {
				g_missing.push_back(a_name);
				return 0;
			}
			return Lookup(a_ids[0], a_name);
		}

		[[nodiscard]] std::uintptr_t VtableOfObject(const void* a_object) noexcept
		{
			return a_object ? *static_cast<const std::uintptr_t*>(a_object) : 0;
		}
	}

	const Capabilities& Init()
	{
		// One id at a time, no all-or-nothing gate: a runtime whose ids are not in the database simply
		// reports fewer capabilities, and the plugin decides what it can do. Addresses of functions that
		// do resolve are still published, so a later port only has to fill in the missing ones.
		g.clone = Ask(kCloneID, "NiObject::Clone");
		g.createTriShape = Ask(kCreateTriShapeID, "Renderer::CreateTriShape");
		g.decRefTriShape = Ask(kDecRefTriShapeID, "Renderer::DecRef(TriShape)");
		g.renderer = Ask(kRendererID, "Renderer");
		g.setRendererData = Ask(kSetRendererDataID, "BSGeometry::SetRendererData");
		g.nodeCtor = Ask(kNodeCtorID, "NiNode::NiNode");
		g.fadeNodeCtor = Ask(kFadeNodeCtorID, "BSFadeNode::BSFadeNode");
		g.fadeRange = Ask(kFadeRangeID, "ConfigureFadeNodeRange");
		g.uGrids = Ask(kUGridsToLoadID, "uGridsToLoad");
		g.fadeFrame = Ask(kFadeFrameID, "fade frame number");
		g.update = Ask(kUpdateID, "NiAVObject::Update");
		g.tes = Ask(kTES, "TES");
		g.gridGet = Ask(kGridGet, "GridCellArray::Get");
		g.useCombined = Ask(kUseCombinedID, "bUseCombinedObjects");
		// The GameSettingCollection path for the same switch: the id resolves on every family, so on AE this
		// is what makes caps.precombines true. Probe it non-fatally; a missing singleton just leaves it false.
		g.settingCollection = ProbeSettingCollection();
		g.previsEnabled = Ask(kPrevisEnabledID, "BSPreCulledObjects::QWantEnabled");
		g.previsActive = Ask(kPrevisActiveID, "BSPreCulledObjects::QEnabled");
		g.setRange = Ask(kSetRangeID, "BSFadeNode::SetRange");
		g.fadeMults = Ask(kFadeMultsID, "fade multipliers");
		g.fadeDistMult = Ask(kFadeDistMultID, "fDistanceMultiplier");
		g.fadingOn = Ask(kFadingOnID, "fading enabled");
		// Optional: without them chunks are not drawn while previs draws the main view (the originals are shown then).
		const auto renderPreUI = Ask(kRenderPreUIID, "Render_PreUI");
		g.previsQuerySite = renderPreUI ? renderPreUI + kPrevisQuerySite : 0;
		g.previsQuery = Ask(kPrevisQueryID, "the per-frame previs query");
		g.visibility = Ask(kVisibilityID, "MultiCellVisibilityData");
		g.registerDynamic = Ask(kRegisterDynamicID, "RegisterDynamicObject");
		g.unregisterDynamic = Ask(kUnregisterDynamicID, "UnregisterDynamicObject");
		g.previsFeed = g.previsQuerySite && g.previsQuery && g.visibility && g.registerDynamic && g.unregisterDynamic;
		g.triShapeVtable = AskVtable(RE::VTABLE::BSTriShape, "BSTriShape");
		g.meshLODTriShapeVtable = AskVtable(RE::VTABLE::BSMeshLODTriShape, "BSMeshLODTriShape");
		g.nodeVtable = AskVtable(RE::VTABLE::NiNode, "NiNode");
		g.fadeNodeVtable = AskVtable(RE::VTABLE::BSFadeNode, "BSFadeNode");
		g.lightingShaderVtable = AskVtable(RE::VTABLE::BSLightingShaderProperty, "BSLightingShaderProperty");
		g.effectShaderVtable = AskVtable(RE::VTABLE::BSEffectShaderProperty, "BSEffectShaderProperty");

		g_caps.clone = g.clone != 0;
		g_caps.triShape = g.createTriShape && g.decRefTriShape && g.renderer && g.triShapeVtable && g.meshLODTriShapeVtable;
		g_caps.geometry = g.setRendererData != 0;
		g_caps.nodes = g.nodeVtable && g.fadeNodeVtable;  // NewNode/NewChunkFadeNode fall back to the SDK constructors
		g_caps.update = true;  // the virtual passes are always there (Engine::UpdateStatic falls back to them)
		g_caps.fade = g.fadeRange && g.uGrids && g.fadeFrame && g.setRange && g.fadeMults && g.fadeDistMult && g.fadingOn;
		g_caps.previs = g.previsEnabled && g.previsActive;
		g_caps.previsHook = g.previsFeed;
		g_caps.resetHook = true;  // the site check below decides; the ids are asked there
		g_caps.precombines = g.useCombined != 0 || g.settingCollection;
		g_caps.cells = g.tes && g.gridGet != 0;

		// One compact summary instead of one warning per id: on a runtime that carries none of these ids that
		// would otherwise be ~25 lines per start. The names are listed so a port knows exactly what to fill in.
		if (!g_missing.empty()) {
			std::string names;
			for (const auto* name : g_missing) {
				if (!names.empty()) {
					names += ", ";
				}
				names += name;
			}
			logger::warn(
				"{} engine ids are missing on {} (not in the Runtime Database for this runtime): {}",
				g_missing.size(), REL::Module::get().version().string(), names);
		}

		logger::info(
			"engine ids: combine {}, previs {}, precombines {} -> combining {}",
			g_caps.CombineReady() ? "ready" : "incomplete",
			g_caps.previs ? "available" : "missing",
			g_caps.precombines ? "available" : "missing",
			g_caps.CombineReady() ? "enabled" : "disabled");
		return g_caps;
	}

	const Capabilities& Caps() noexcept
	{
		return g_caps;
	}

	bool IsExactTriShape(const RE::NiAVObject* a_object) noexcept
	{
		return VtableOfObject(a_object) == g.triShapeVtable;
	}

	bool IsExactMeshLODTriShape(const RE::NiAVObject* a_object) noexcept
	{
		return VtableOfObject(a_object) == g.meshLODTriShapeVtable;
	}

	bool IsExactNode(const RE::NiAVObject* a_object) noexcept
	{
		const auto vtable = VtableOfObject(a_object);
		return vtable == g.nodeVtable || vtable == g.fadeNodeVtable;
	}

	bool IsPlainNode(const RE::NiAVObject* a_object) noexcept
	{
		return VtableOfObject(a_object) == g.nodeVtable;
	}

	bool IsLightingShader(const void* a_property) noexcept
	{
		return VtableOfObject(a_property) == g.lightingShaderVtable;
	}

	bool IsEffectShader(const void* a_property) noexcept
	{
		return VtableOfObject(a_property) == g.effectShaderVtable;
	}

	bool CanMerge(void* a_property, void* a_other)
	{
		using func_t = bool (*)(void*, void*);
		const auto vtable = *static_cast<func_t* const*>(a_property);
		return vtable[kCanMergeSlot](a_property, a_other);
	}

	RE::NiAVObject* Clone(RE::NiAVObject* a_object, bool a_plain)
	{
		using func_t = RE::NiObject* (*)(RE::NiObject*);
		const auto clone = static_cast<RE::NiAVObject*>(reinterpret_cast<func_t>(g.clone)(a_object));
		// BSMeshLODTriShape is BSTriShape plus three u32 after it; its own virtuals only stream, clone and pick the
		// drawn LOD prefix, and both free through the same allocator. Becoming the base class: vtable and type byte
		// as BSTriShape's CreateClone sets them, mesh-LOD flag cleared (FO4-ENGINE-NOTES 7.11).
		if (a_plain && clone && IsExactMeshLODTriShape(clone)) {
			*reinterpret_cast<std::uintptr_t*>(clone) = g.triShapeVtable;
			*Field<std::uint8_t>(clone, Offset::kGeometryType) = 3;
			clone->flags.flags &= ~ObjectFlag::kMeshLOD;
		}
		return clone;
	}

	void* CreateTriShape(const void* a_vertices, std::uint32_t a_vertexBytes, std::uint64_t a_desc, const std::uint16_t* a_indices, std::uint32_t a_indexCount)
	{
		using func_t = void* (*)(void*, std::uint32_t&, const void*, std::uint64_t, const std::uint16_t*, std::uint32_t);
		std::uint32_t bytes = a_vertexBytes;
		return reinterpret_cast<func_t>(g.createTriShape)(reinterpret_cast<void*>(g.renderer), bytes, a_vertices, a_desc, a_indices, a_indexCount);
	}

	void ReleaseTriShape(void* a_triShape)
	{
		if (a_triShape) {
			using func_t = void (*)(void*, void*);
			reinterpret_cast<func_t>(g.decRefTriShape)(reinterpret_cast<void*>(g.renderer), a_triShape);
		}
	}

	bool TriShapeReady(const void* a_triShape) noexcept
	{
		for (const auto offset : { Offset::kVertexBuffer, Offset::kIndexBuffer }) {
			const auto buffer = *Field<const std::byte* const>(a_triShape, offset);
			if (buffer && std::atomic_ref(*Field<std::uint32_t>(buffer, Offset::kBufferPending)).load(std::memory_order_acquire) != 0) {
				return false;
			}
		}
		return true;
	}

	void SetGeometry(RE::NiAVObject* a_shape, void* a_triShape, std::uint32_t a_vertices, std::uint32_t a_triangles)
	{
		// What BSTriShape::CopyMembers does when the renderer data changes: release the old, install the new.
		const auto old = *Field<void*>(a_shape, Offset::kRendererData);
		using func_t = void (*)(RE::NiAVObject*, void*);
		reinterpret_cast<func_t>(g.setRendererData)(a_shape, a_triShape);  // also copies the vertex desc to +0x150
		ReleaseTriShape(old);
		*Field<std::uint32_t>(a_shape, Offset::kTriangles) = a_triangles;
		*Field<std::uint16_t>(a_shape, Offset::kVertices) = static_cast<std::uint16_t>(a_vertices);
	}

	namespace
	{
		void SettlePreviousWorld(RE::NiAVObject* a_object)
		{
			a_object->previousWorld = a_object->world;
			if (const auto node = a_object->IsNode()) {
				for (const auto& child : node->children) {
					if (child) {
						SettlePreviousWorld(child.get());
					}
				}
			}
		}
	}

	namespace
	{
		// The engine's own NiAVObject::Update(NiUpdateData&) is inlined in every SDK build: it runs
		// UpdateDownwardPass(data, 0) and, when the object has a parent and the data does not carry
		// flag 0x200, parent->UpdateUpwardPass(data). Both are virtual, at slots 0x30 and 0x42, so they
		// can be called through the object's own vtable on any runtime - no engine id is needed for this.
		constexpr std::size_t kUpdateDownwardSlot = 0x30;
		constexpr std::size_t kUpdateUpwardSlot = 0x42;
		constexpr std::uint32_t kUpdateNoUpward = 0x200;  // NiAVObject::Update skips the upward pass
	}

	void UpdateStatic(RE::NiAVObject* a_object)
	{
		RE::NiUpdateData data{};
		if (g.update) {
			// OG: the free function the ids know. Kept so 1.10.163 behaves exactly as before.
			using func_t = void (*)(RE::NiAVObject*, RE::NiUpdateData&);
			reinterpret_cast<func_t>(g.update)(a_object, data);
		} else {
			// Any runtime: the two virtual passes the same function performs. The parent is read at
			// NiAVObject::parent (+0x28: NiObjectNET is 0x28 bytes). Reading it as a pointer rather than
			// through the SDK field keeps this translation unit from needing the full NiAVObject type.
			using pass_t = void (*)(RE::NiAVObject*, RE::NiUpdateData&, std::uint32_t);
			using upward_t = void (*)(void*, RE::NiUpdateData&);
			constexpr std::size_t kObjectParent = 0x28;
			// The object's own vtable, read once: slots 0x30 (downward) and 0x42 (upward).
			const auto slots = *reinterpret_cast<void* const* const*>(a_object);
			reinterpret_cast<pass_t>(slots[kUpdateDownwardSlot])(a_object, data, 0);
			const auto parent = *Field<void* const>(a_object, kObjectParent);
			if (parent && (data.flags & kUpdateNoUpward) == 0) {
				const auto parentSlots = *reinterpret_cast<void* const* const*>(parent);
				reinterpret_cast<upward_t>(parentSlots[kUpdateUpwardSlot])(parent, data);
			}
		}
		SettlePreviousWorld(a_object);
	}

	RE::NiNode* NewNode(std::uint16_t a_children)
	{
		if (!g.nodeCtor) {
			// Any runtime: the SDK's NiNode constructor is inline (it sizes the children array and sets
			// the child vtable through stl::emplace_vtable), so no engine id is needed.
			return new RE::NiNode(a_children);
		}
		auto memory = RE::aligned_alloc(alignof(RE::NiNode), sizeof(RE::NiNode));
		if (!memory) {
			return nullptr;
		}
		std::memset(memory, 0, sizeof(RE::NiNode));
		using func_t = RE::NiNode* (*)(void*, std::uint32_t);
		return reinterpret_cast<func_t>(g.nodeCtor)(memory, a_children);
	}

	FadeRange ReadFadeRange(const RE::NiAVObject* a_root) noexcept
	{
		FadeRange range;
		if (VtableOfObject(a_root) == g.fadeNodeVtable) {
			const auto node = static_cast<const RE::BSFadeNode*>(a_root);
			range.nearDistance = *Field<const float>(node, Offset::kFadeNear);
			range.farDistance = *Field<const float>(node, Offset::kFadeFar);
			range.type = *Field<const std::uint8_t>(node, Offset::kFadeType);
			if (!std::isfinite(range.nearDistance) || !std::isfinite(range.farDistance)) {
				range.farDistance = 0.0F;  // Init's FLT_MAX is finite; anything else is no range
			}
		}
		return range;
	}

	float FadeStartDistance(const FadeRange& a_range) noexcept
	{
		constexpr float kNever = std::numeric_limits<float>::infinity();
		// Type 6 only fades in; the table has entries up to type 10 (TES::InitFadeOutMultipliers, 7.9).
		if (!a_range.Valid() || a_range.type == 6 || a_range.type > 10 || *reinterpret_cast<const std::uint8_t*>(g.fadingOn) == 0) {
			return kNever;
		}
		const float mult = reinterpret_cast<const float*>(g.fadeMults)[a_range.type];
		if (!(mult > 0.0F)) {
			return kNever;  // ComputeFadeAmount snaps such nodes to 1
		}
		const auto camera = RE::Main::WorldRootCamera();
		float      lodAdjust = camera ? *Field<const float>(camera, 0x194) : 1.0F;
		lodAdjust = lodAdjust > 0.0F ? lodAdjust : 1.0F;
		float distanceMult = *reinterpret_cast<const float*>(g.fadeDistMult);
		distanceMult = distanceMult > 0.0F ? distanceMult : 1.0F;
		// ComputeFadeAmount: s = lodAdjust / mult x distance; fading starts once s passes distanceMult x near.
		return distanceMult * a_range.nearDistance * mult / lodAdjust;
	}

	float LoadedReach() noexcept
	{
		const auto uGrids = std::clamp(*reinterpret_cast<const std::int32_t*>(g.uGrids), 1, 63);
		return static_cast<float>(uGrids + 1) * 2048.0F * 1.4142F;
	}

	RE::BSFadeNode* NewChunkFadeNode(RE::NiAVObject* a_shape, float a_fade, const FadeRange& a_range)
	{
		static_assert(sizeof(RE::BSFadeNode) == 0x1C0);
		const auto uGrids = *reinterpret_cast<const std::int32_t*>(g.uGrids);
		if (!a_range.Valid() && (uGrids < 1 || uGrids > 63)) {
			static bool logged = false;
			if (!std::exchange(logged, true)) {
				logger::error("uGridsToLoad reads {}: chunks are left as they are", uGrids);
			}
			return nullptr;
		}
		auto memory = RE::aligned_alloc(0x10, sizeof(RE::BSFadeNode));  // as ProcessTriShape: 0x1C0 bytes, 16-aligned
		if (!memory) {
			return nullptr;
		}
		std::memset(memory, 0, sizeof(RE::BSFadeNode));
		using ctor_t = RE::BSFadeNode* (*)(void*);
		const auto node = reinterpret_cast<ctor_t>(g.fadeNodeCtor)(memory);
		node->local.MakeIdentity();

		if (a_range.Valid()) {
			// The members' own range and type: the chunk fades out where they would (7.9).
			*Field<std::uint8_t>(node, Offset::kFadeType) = a_range.type;
			using range_t = void (*)(RE::BSFadeNode*, float, float);
			reinterpret_cast<range_t>(g.setRange)(node, a_range.nearDistance, a_range.farDistance);
		} else {
			// The range first, while the node's world bound is still 0: near / far then come out as a precombined
			// chunk's, whatever its radius.
			using range_t = void (*)(RE::BSFadeNode*, float, bool);
			reinterpret_cast<range_t>(g.fadeRange)(node, static_cast<float>(uGrids), false);
		}

		node->flags.flags |= ObjectFlag::kPickChildren | (a_shape->flags.flags & ObjectFlag::kMeshLOD);
		if (a_shape->flags.flags & ObjectFlag::kMeshLOD) {
			// A BSMeshLODTriShape draws the LOD prefix of this node's level, nothing at level 0: start at full detail,
			// settled, as DetermineMeshLODLevel leaves a node with mesh LOD off (FO4-ENGINE-NOTES 7.11).
			node->meshLODFadingLevel = 2;
			node->currentMeshLODLevel = 3;
			node->previousMeshLODLevel = 3;
		}
		node->previousMaxA = 1.0F;
		if (a_fade >= 1.0F) {
			// Faded in, as TESObjectCELL::UpdateFadeNodes leaves a cell's fade nodes after a load.
			node->currentFade = 1.0F;
			node->currentDecalFade = 1.0F;
			node->flags.flags |= ObjectFlag::kFadeComplete;
		} else {
			// Mid fade-in, as AttachCombinedObjectArt starts a chunk (at 0) and then OnVisibleFrameOnly.
			node->currentFade = std::max(a_fade, 0.0F);
			node->currentDecalFade = node->currentFade;
			node->flags.flags &= ~ObjectFlag::kFadeComplete;
			node->frameCounter = *reinterpret_cast<const std::int32_t*>(g.fadeFrame);
		}
		node->AttachChild(a_shape, true);
		return node;
	}

	RE::BSFadeNode* NewProbe()
	{
		auto memory = RE::aligned_alloc(0x10, sizeof(RE::BSFadeNode));
		if (!memory) {
			return nullptr;
		}
		std::memset(memory, 0, sizeof(RE::BSFadeNode));
		using ctor_t = RE::BSFadeNode* (*)(void*);
		const auto node = reinterpret_cast<ctor_t>(g.fadeNodeCtor)(memory);
		node->local.MakeIdentity();
		node->name = RE::BSFixedString("RuntimeCombiner probe");
		// Init leaves near and far at FLT_MAX, so ComputeFadeAmount returns 1 at any distance (7.9).
		node->currentFade = 1.0F;
		node->currentDecalFade = 1.0F;
		node->previousMaxA = 1.0F;
		node->flags.flags |= ObjectFlag::kAlwaysDraw | ObjectFlag::kFadeComplete;
		return node;
	}

	bool TakeProbeSeen(RE::NiAVObject* a_probe) noexcept
	{
		auto&      flags = a_probe->flags.flags;
		const bool seen = (flags & ObjectFlag::kAccumulated) != 0;
		flags &= ~ObjectFlag::kAccumulated;
		return seen;
	}

	const char* ClassName(const RE::NiAVObject* a_object)
	{
		const auto rtti = a_object ? a_object->GetRTTI() : nullptr;
		return rtti && rtti->GetName() ? rtti->GetName() : "?";
	}

	float FadeIn(const RE::NiAVObject* a_root) noexcept
	{
		if (VtableOfObject(a_root) != g.fadeNodeVtable || (a_root->flags.flags & ObjectFlag::kFadeComplete)) {
			return 1.0F;
		}
		return std::clamp(static_cast<const RE::BSFadeNode*>(a_root)->currentFade, 0.0F, 1.0F);
	}

	std::vector<RE::TESObjectCELL*> LoadedCells()
	{
		std::vector<RE::TESObjectCELL*> cells;
		const auto                      tes = *reinterpret_cast<TES**>(g.tes);
		if (!tes) {
			return cells;
		}
		const auto attached = [](RE::TESObjectCELL* a_cell) {
			return a_cell && a_cell->cellState.get() == RE::TESObjectCELL::CELL_STATE::kAttached;
		};
		if (tes->interiorCell) {
			if (attached(tes->interiorCell)) {
				cells.push_back(tes->interiorCell);
			}
			return cells;
		}
		const auto grid = tes->gridCells;
		if (!grid) {
			return cells;
		}
		using func_t = RE::TESObjectCELL** (*)(const GridCellArray*, std::uint32_t, std::uint32_t);  // GridCell { cell }
		for (std::uint32_t x = 0; x < grid->dimension; ++x) {
			for (std::uint32_t y = 0; y < grid->dimension; ++y) {
				const auto gridCell = reinterpret_cast<func_t>(g.gridGet)(grid, x, y);
				if (gridCell && attached(*gridCell)) {
					cells.push_back(*gridCell);
				}
			}
		}
		return cells;
	}

	bool InInterior() noexcept
	{
		const auto tes = *reinterpret_cast<TES**>(g.tes);
		return tes && tes->interiorCell;
	}

	namespace
	{
		// TESObjectCELL +0xD0 loadedData; its +0xC0 / +0xD0 array of attached precombined chunks (7.12).
		bool ReadPrecombinedList(const RE::TESObjectCELL* a_cell, RE::NiAVObject* const*& a_list, std::uint32_t& a_count) noexcept
		{
			__try {
				const auto loaded = *reinterpret_cast<const std::byte* const*>(reinterpret_cast<const std::byte*>(a_cell) + 0xD0);
				a_list = loaded ? *reinterpret_cast<RE::NiAVObject* const* const*>(loaded + 0xC0) : nullptr;
				a_count = loaded && a_list ? *reinterpret_cast<const std::uint32_t*>(loaded + 0xD0) : 0;
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		bool ReadRadius(RE::NiAVObject* const* a_list, std::uint32_t a_index, float& a_radius) noexcept
		{
			__try {
				const auto object = a_list[a_index];
				a_radius = object ? object->worldBound.fRadius : 0.0F;
				return object != nullptr;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}
	}

	std::uint32_t PrecombinedChunks(const RE::TESObjectCELL* a_cell, std::vector<float>* a_radii)
	{
		RE::NiAVObject* const* list = nullptr;
		std::uint32_t          count = 0;
		if (!ReadPrecombinedList(a_cell, list, count)) {
			return 0;
		}
		for (std::uint32_t i = 0; a_radii && i < count; ++i) {
			if (float radius = 0.0F; ReadRadius(list, i, radius)) {
				a_radii->push_back(radius);
			}
		}
		return count;
	}

	namespace
	{
		std::uint32_t CountMeshes(RE::NiAVObject* a_object)
		{
			if (!a_object || a_object->GetAppCulled()) {
				return 0;
			}
			if (a_object->IsGeometry()) {
				return 1;
			}
			const auto    node = a_object->IsNode();
			std::uint32_t count = 0;
			if (node) {
				for (auto& child : node->children) {
					count += CountMeshes(child.get());
				}
			}
			return count;
		}
	}

	void WalkEntries(const RE::TESObjectCELL* a_cell, std::vector<WalkEntry>& a_out, std::uint32_t& a_hidden)
	{
		const auto loaded = a_cell ? a_cell->loadedData : nullptr;
		const auto root = loaded ? loaded->cell3D.get() : nullptr;
		if (!root || root->children.capacity() <= 9) {
			return;
		}
		std::unordered_set<const RE::NiAVObject*> precombined;
		RE::NiAVObject* const*                    list = nullptr;
		std::uint32_t                             count = 0;
		if (ReadPrecombinedList(a_cell, list, count)) {
			precombined.insert(list, list + count);
		}
		for (const std::uint16_t index : { std::uint16_t{ 3 }, std::uint16_t{ 9 } }) {
			const auto cellNode = root->children[index].get();
			const auto node = cellNode ? cellNode->IsNode() : nullptr;
			const auto add = [&](RE::NiAVObject* a_object) {
				if (!a_object) {
					return;
				}
				if (a_object->GetAppCulled()) {
					++a_hidden;
					return;
				}
				a_out.push_back({ a_object, CountMeshes(a_object), precombined.contains(a_object), index == 9 });
			};
			if (!node) {
				continue;
			}
			for (auto& pointer : node->children) {
				const auto child = pointer.get();
				if (child && IsPlainNode(child) && !child->GetAppCulled()) {
					for (auto& member : static_cast<RE::NiNode*>(child)->children) {
						add(member.get());
					}
				} else {
					add(child);
				}
			}
		}
	}

	const void* CombinedRefs(const RE::TESObjectCELL* a_cell) noexcept
	{
		const auto extras = a_cell ? a_cell->extraList.get() : nullptr;
		return extras ? extras->GetByType(static_cast<RE::EXTRA_DATA_TYPE>(kCombinedRefsExtra)) : nullptr;
	}

	bool InCombinedRefs(const void* a_combinedRefs, std::uint32_t a_formID) noexcept
	{
		if (!a_combinedRefs) {
			return false;
		}
		__try {
			// ExtraCombinedRefs +0x18: BSTSet<u32 formID>, CommonLibF4's BSTScatterTable layout (capacity +0x24,
			// sentinel +0x30, entries +0x40: what TESObjectCELL::AddReference reads, FO4-ENGINE-NOTES 7.12).
			return reinterpret_cast<const RE::BSTSet<std::uint32_t>*>(static_cast<const std::byte*>(a_combinedRefs) + 0x18)->contains(a_formID);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	namespace
	{
		// The precombine switch as a game setting. Fallout 4 keeps bUseCombinedObjects in the
		// GameSettingCollection, whose singleton the Runtime Database carries for OG, NG and AE alike
		// (RE::GameSettingCollection::GetSingleton() in RE/Bethesda/Settings.h). Reading it through the
		// collection avoids needing the address of the engine's own global, which only OG has an id for.
		constexpr std::string_view kUseCombinedSetting = "bUseCombinedObjects"sv;
		constexpr std::size_t      kSettingValue = 0x08;  // SETTING_VALUE Setting::_value (private; see Settings.h)

		// The Setting for a_name, or null. Main thread.
		[[nodiscard]] RE::Setting* FindSetting(std::string_view a_name)
		{
			const auto collection = RE::GameSettingCollection::GetSingleton();
			if (!collection) {
				return nullptr;
			}
			const auto it = collection->settings.find(a_name);
			return it != collection->settings.end() ? it->second : nullptr;
		}

		// Whether the switch is on, or nullopt when the setting is not there (an older or trimmed table).
		[[nodiscard]] std::optional<bool> ReadSettingBool(std::string_view a_name)
		{
			const auto setting = FindSetting(a_name);
			if (!setting) {
				return std::nullopt;
			}
			return *Field<const std::uint8_t>(setting, kSettingValue) != 0;
		}

		// Sets the switch. False when the setting is not there. The value lives behind a private member
		// (SETTING_VALUE Setting::_value at +0x08) and the SDK exposes no SetBinary, so it is written at
		// that offset; the collection is the engine's own, so the change sticks for the cell loads that follow.
		[[nodiscard]] bool WriteSettingBool(std::string_view a_name, bool a_value)
		{
			const auto setting = FindSetting(a_name);
			if (!setting) {
				return false;
			}
			*Field<std::uint8_t>(setting, kSettingValue) = a_value ? 1 : 0;
			return true;
		}
	}

	bool PrecombinesEnabled() noexcept
	{
		if (g.useCombined) {
			return *reinterpret_cast<const volatile std::uint8_t*>(g.useCombined) != 0;
		}
		try {
			return ReadSettingBool(kUseCombinedSetting).value_or(true);
		} catch (...) {
			return true;  // precombines on: the safe reading, the engine's own default
		}
	}

	void SetPrecombinesEnabled(bool a_enabled) noexcept
	{
		if (g.useCombined) {
			*reinterpret_cast<volatile std::uint8_t*>(g.useCombined) = a_enabled ? 1 : 0;
			return;
		}
		try {
			if (!WriteSettingBool(kUseCombinedSetting, a_enabled)) {
				logger::error("bUseCombinedObjects is not in the GameSettingCollection: precombines are left as they are");
			}
		} catch (...) {
			logger::error("bUseCombinedObjects could not be written: precombines are left as they are");
		}
	}

	bool ReadChunkView(const RE::NiAVObject* a_shape, const RE::BSFadeNode* a_fadeNode, ChunkView& a_out) noexcept
	{
		__try {
			const auto property = *Field<const std::byte* const>(a_shape, Offset::kShaderProperty);
			if (!property) {
				return false;
			}
			a_out.passes = *Field<const void* const>(property, Offset::kRenderPasses) != nullptr;
			a_out.alpha = *Field<const float>(property, Offset::kShaderAlpha);
			a_out.material = reinterpret_cast<const RE::NiObjectNET*>(property)->name.c_str();
			a_out.shaderFlags = *Field<const std::uint64_t>(property, Offset::kShaderFlags);
			const auto& now = a_shape->world;
			const auto& before = a_shape->previousWorld;
			a_out.moving = now.translate.x != before.translate.x || now.translate.y != before.translate.y ||
			               now.translate.z != before.translate.z || now.scale != before.scale;
			for (int i = 0; i < 3 && !a_out.moving; ++i) {
				for (int j = 0; j < 3 && !a_out.moving; ++j) {
					a_out.moving = now.rotate.entry[i].pt[j] != before.rotate.entry[i].pt[j];
				}
			}
			if (a_fadeNode) {
				// BSFadeNode::OnVisible stamps the fade frame number when a view files the node (7.8).
				const auto frame = *reinterpret_cast<const volatile std::int32_t*>(g.fadeFrame);
				a_out.inView = frame - a_fadeNode->frameCounter <= 3;
				a_out.fade = a_fadeNode->currentFade;
				a_out.owner = *Field<const void* const>(property, Offset::kShaderFadeNode) == a_fadeNode;
			}
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	bool PrevisEnabled()
	{
		using func_t = bool (*)();
		return reinterpret_cast<func_t>(g.previsEnabled)();
	}

	bool PrevisActive()
	{
		using func_t = bool (*)();
		return reinterpret_cast<func_t>(g.previsActive)();
	}

	bool SeenByMainView(const RE::NiAVObject* a_object) noexcept
	{
		return a_object && (a_object->flags.flags & ObjectFlag::kAccumulated) != 0;
	}

	namespace
	{
		using DynamicFunc = void (*)(void*, RE::NiAVObject*);

		// The engine's MultiCellVisibilityData: what TESObjectREFR::RegisterWithPrecomputedVisibility passes (5.5d).
		[[nodiscard]] void* Visibility() noexcept
		{
			return g.previsFeed ? *reinterpret_cast<void* const*>(g.visibility) : nullptr;
		}
	}

	bool RegisterPrevisObject(RE::NiAVObject* a_object) noexcept
	{
		const auto visibility = Visibility();
		if (!visibility || !a_object) {
			return false;
		}
		reinterpret_cast<DynamicFunc>(g.registerDynamic)(visibility, a_object);
		return true;
	}

	void UnregisterPrevisObject(RE::NiAVObject* a_object) noexcept
	{
		if (const auto visibility = Visibility(); visibility && a_object) {
			reinterpret_cast<DynamicFunc>(g.unregisterDynamic)(visibility, a_object);
		}
	}

	namespace
	{
		// MultiCellVisibilityData's dynamic-object set: a BSTSet<NiAVObject*> whose table starts at +0x88 (capacity
		// +0x94, free +0x98, entries +0xB0; FO4-ENGINE-NOTES 5.5d), the layout of CommonLibF4's BSTScatterTable.
		constexpr std::size_t kDynamicSet = 0x88;
		using DynamicSet = RE::BSTSet<RE::NiAVObject*>;

		[[nodiscard]] bool InDynamicSet(const void* a_visibility, RE::NiAVObject* a_object) noexcept
		{
			__try {
				return reinterpret_cast<const DynamicSet*>(static_cast<const std::byte*>(a_visibility) + kDynamicSet)->contains(a_object);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}
	}

	bool UnregisterIfDynamic(RE::NiAVObject* a_object) noexcept
	{
		const auto visibility = Visibility();
		if (!visibility || !a_object || !InDynamicSet(visibility, a_object)) {
			return false;
		}
		reinterpret_cast<DynamicFunc>(g.unregisterDynamic)(visibility, a_object);
		return true;
	}

	std::uint32_t PrevisDynamicObjects() noexcept
	{
		const auto visibility = static_cast<const std::byte*>(Visibility());
		if (!visibility) {
			return 0;
		}
		__try {
			const auto capacity = *reinterpret_cast<const std::uint32_t*>(visibility + kDynamicSet + 0x0C);
			const auto free = *reinterpret_cast<const std::uint32_t*>(visibility + kDynamicSet + 0x10);
			return capacity >= free ? capacity - free : 0;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return 0;
		}
	}

	namespace
	{
		using QueryFunc = void (*)();
		QueryFunc g_queryOriginal{ nullptr };
		void (*g_queryAfter)() { nullptr };

		std::chrono::steady_clock::duration g_queryTime{};  // main thread only
		std::uint32_t                       g_queryCalls{ 0 };

		void PrevisQueryThunk()
		{
			const auto start = std::chrono::steady_clock::now();
			g_queryOriginal();
			g_queryTime += std::chrono::steady_clock::now() - start;
			++g_queryCalls;
			g_queryAfter();
		}

		using PurgeFunc = void (*)(void*);
		PurgeFunc g_purgeOriginal{ nullptr };
		void (*g_resetAfter)() { nullptr };

		void GameResetPurgeThunk(void* a_tes)
		{
			g_purgeOriginal(a_tes);
			g_resetAfter();
		}

		// One trampoline for every call-site hook (allocating again would replace it).
		F4SE::Trampoline& Trampoline()
		{
			static const bool allocated = (F4SE::AllocTrampoline(128), true);
			(void)allocated;
			return F4SE::GetTrampoline();
		}

		// The engine's `call a_target` at a_site (E8 rel32), or 0 when another plugin took it.
		[[nodiscard]] std::uintptr_t CallTarget(std::uintptr_t a_site) noexcept
		{
			const auto site = reinterpret_cast<const std::uint8_t*>(a_site);
			return site[0] == 0xE8 ? a_site + 5 + *reinterpret_cast<const std::int32_t*>(site + 1) : 0;
		}
	}

	bool InstallPrevisQueryHook(void (*a_after)())
	{
		if (!g.previsFeed || !a_after) {
			return false;
		}
		// The site must still be the engine's own `call 1264353` (another plugin may have taken it).
		const auto target = CallTarget(g.previsQuerySite);
		if (target != g.previsQuery) {
			logger::error("previs: Render_PreUI+0x{:X} is not the engine's call to the previs query (byte {:02X}, target {:X}); "
						  "the original meshes are shown while previs is active",
				kPrevisQuerySite, *reinterpret_cast<const std::uint8_t*>(g.previsQuerySite), target);
			return false;
		}
		g_queryAfter = a_after;
		g_queryOriginal = reinterpret_cast<QueryFunc>(Trampoline().write_call<5>(g.previsQuerySite, &PrevisQueryThunk));
		return true;
	}

	double TakePrevisQueryTime(std::uint32_t& a_calls) noexcept
	{
		a_calls = std::exchange(g_queryCalls, 0);
		return std::chrono::duration<double, std::milli>(std::exchange(g_queryTime, {})).count();
	}

	bool InstallGameResetHook(void (*a_after)())
	{
		const auto reset = Ask(kGameResetID, "Main::PerformGameReset");
		const auto purge = Ask(kPurgeCellsID, "TES::PurgeBufferedCells");
		if (!reset || !purge || !a_after) {
			g_caps.resetHook = false;
			return false;
		}
		const auto site = reset + kGameResetPurgeSite;
		if (CallTarget(site) != purge) {
			logger::error("Main::PerformGameReset+0x{:X} is not the engine's call to TES::PurgeBufferedCells (byte {:02X})",
				kGameResetPurgeSite, *reinterpret_cast<const std::uint8_t*>(site));
			return false;
		}
		g_resetAfter = a_after;
		g_purgeOriginal = reinterpret_cast<PurgeFunc>(Trampoline().write_call<5>(site, &GameResetPurgeThunk));
		return true;
	}
}
