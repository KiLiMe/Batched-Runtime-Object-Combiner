#include "Combine/Manager.h"

#include "Combine/Gather.h"
#include "Engine/Engine.h"
#include "Settings.h"

namespace RC::Combine::Manager
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		constexpr std::uint32_t kFormDeletedOrDisabled = (1u << 5) | (1u << 11);
		constexpr std::size_t   kUpdateSlot = 0xCF;               // Actor::Update(float); PlayerCharacter's runs once per game frame
		constexpr std::uint32_t kNoChildCount = 0xFFFFFFFF;
		constexpr std::size_t   kCreateBytesPerFrame = 16u << 20;  // vertex + index data handed to the renderer per frame

		[[nodiscard]] double Ms(Clock::time_point a_since) noexcept
		{
			return std::chrono::duration<double, std::milli>(Clock::now() - a_since).count();
		}

		// ---- transforms ---------------------------------------------------------------------------------------

		struct World
		{
			float values[13]{};  // rotation 3x3, translate, scale

			World() = default;
			explicit World(const RE::NiTransform& a_t) noexcept
			{
				for (int i = 0; i < 3; ++i) {
					for (int j = 0; j < 3; ++j) {
						values[i * 3 + j] = a_t.rotate.entry[i].pt[j];
					}
				}
				values[9] = a_t.translate.x;
				values[10] = a_t.translate.y;
				values[11] = a_t.translate.z;
				values[12] = a_t.scale;
			}
			[[nodiscard]] bool operator==(const World& a_other) const noexcept
			{
				return std::equal(std::begin(values), std::end(values), std::begin(a_other.values));
			}
		};

		// ---- state --------------------------------------------------------------------------------------------

		// An object hidden because a chunk draws it now: a reference's 3D root, or one of its meshes.
		struct Hidden
		{
			RE::NiPointer<RE::NiAVObject>    object;
			RE::NiPointer<RE::TESObjectREFR> ref;
			RE::NiPointer<RE::NiAVObject>    root;  // the reference's 3D at apply
			std::uint32_t                    formID{ 0 };
			World                            world;
			std::uint32_t                    children{ kNoChildCount };  // a hidden node root's child count
		};

		// What the watchdog found.
		enum class Check
		{
			kOk,
			kStale,     // moved, unloaded, disabled, or shown again by the game
			kAttached,  // something was attached under a hidden root (a decal, an effect)
		};

		struct Container
		{
			RE::NiPointer<RE::NiNode> node;
			RE::NiNode*               parent{ nullptr };
		};

		// One applied chunk (the container holds it): for previs and the summary's audit.
		struct ChunkRecord
		{
			RE::NiPointer<RE::NiAVObject> shape;
			RE::BSFadeNode*               fadeNode{ nullptr };  // its own fade node (bChunkFadeNodes), held by the container
			std::vector<std::uint32_t>    refs;                 // form IDs of the references it draws meshes of
			std::uint32_t                 members{ 0 };
			std::uint32_t                 triangles{ 0 };
			std::uint64_t                 previsVisible{ 0 };   // the tick before the last query that found it visible
			bool                          registered{ false };  // a previs dynamic object (Engine::RegisterPrevisObject)

			// What a view's culling group gets, and what previs tests: the fade node, or the mesh itself.
			[[nodiscard]] RE::NiAVObject* Fed() const noexcept { return fadeNode ? static_cast<RE::NiAVObject*>(fadeNode) : shape.get(); }
		};

		struct Applied
		{
			std::vector<Container>   containers;
			std::vector<ChunkRecord> chunks;
			std::vector<Hidden>      hidden;
			std::uint64_t            frame{ 0 };         // applied at this tick
			bool                     fallback{ false };  // originals shown again and chunks hidden: revert pending
		};

		// Hangs beside a parent's containers and tells whether the main view draws what hangs there (Engine::NewProbe).
		// It stays while the cell is attached, through reverts, so it also tells when drawing resumes.
		struct Probe
		{
			RE::NiPointer<RE::BSFadeNode> node;
			RE::NiNode*                   parent{ nullptr };
			std::uint32_t                 seen{ 0 };    // consecutive ticks after a frame that drew it
			std::uint32_t                 unseen{ 0 };  // consecutive ticks after a frame that didn't
		};

		struct CellState
		{
			std::uint32_t            formID{ 0 };
			std::uint64_t            generation{ 0 };
			std::uint64_t            lastChange{ 0 };
			bool                     dirty{ true };
			bool                     building{ false };
			bool                     blind{ false };  // the main view doesn't draw the probes' groups: originals only
			std::unique_ptr<Applied> applied;
			std::vector<Probe>       probes;
			std::vector<std::pair<const char*, std::uint32_t>> blocked;  // the last gather's blocked parts by class
		};

		struct Totals
		{
			std::uint64_t applies{ 0 };
			std::uint64_t reverts{ 0 };
			std::uint64_t discarded{ 0 };
			std::uint64_t watchdog{ 0 };
			std::uint64_t attached{ 0 };
			std::uint64_t blind{ 0 };  // reverts because the main view stopped drawing a cell's chunks
		};

		std::unordered_map<RE::TESObjectCELL*, CellState>     g_cells;
		std::unordered_map<std::uint32_t, RE::TESObjectCELL*> g_hiddenRefs;  // formID -> cell
		Gather::HiddenSet                                     g_ours;
		Gather::FormSet                                       g_meshByMesh;  // references whose root stays visible
		std::vector<std::unique_ptr<Job>>                     g_jobs;  // submitted: baking, or waiting for GPU buffers
		Totals                                                g_totals;

		std::uint64_t g_frame{ 0 };
		bool          g_active{ true };     // the on/off switch
		bool          g_workshop{ false };  // workshop mode shows the originals (highlights, scrapping)
		bool          g_halted{ false };    // a self-check failed: nothing more is combined
		bool          g_previsPaused{ false };  // previs active and no query hook: originals shown
		bool          g_cbroLoaded{ false };    // CBRO is loaded (read at the first tick)
		bool          g_previsHook{ false };    // the previs query hook is in (Engine::InstallPrevisQueryHook)
		bool          g_keyDown{ false };
		std::size_t   g_watchCursor{ 0 };
		std::atomic<DWORD> g_mainThread{ 0 };  // the thread running PlayerCharacter::Update (set by the first tick)
		std::atomic<bool>  g_resetPending{ false };
		std::uintptr_t g_originalUpdate{ 0 };
		Clock::time_point g_lastSummary{};
		std::uint64_t g_blockedPrinted{ 0 };  // hash of the blocked-class totals last logged

		// Probe hysteresis (ticks): a cell's chunks go after this many frames in a row without a drawn probe, and
		// come back after this many frames in a row with every probe drawn.
		constexpr std::uint32_t kProbeLostTicks = 5;
		constexpr std::uint32_t kProbeBackTicks = 30;

		// ---- previs state (main thread: the tick and Render_PreUI) ----------------------------------------------

		std::uint64_t g_previsTick{ 0 };       // the tick of the last query that ran with previs active
		std::uint64_t g_previsFrames{ 0 };     // queries that ran with previs active, since the last summary
		std::uint32_t g_previsVisible{ 0 };    // chunks the last query's main-view pass found visible
		bool          g_group0Drawn{ false };  // the last tick saw a probe the main view drew

		// ---- bake thread ---------------------------------------------------------------------------------------

		std::mutex              g_queueMutex;
		std::condition_variable g_queueCv;
		std::deque<Job*>        g_queue;

		void WorkerMain()
		{
			for (;;) {
				Job* job = nullptr;
				{
					std::unique_lock lock{ g_queueMutex };
					g_queueCv.wait(lock, [] { return !g_queue.empty(); });
					job = g_queue.front();
					g_queue.pop_front();
				}
				const auto  start = Clock::now();
				const auto& settings = Settings::Get();
				for (auto& bucket : job->buckets) {
					bucket.chunks = Bake::Cluster(bucket.members, settings.chunkSize, settings.minShapesPerChunk);
					for (auto& chunk : bucket.chunks) {
						if (!Bake::Build(bucket.layout, bucket.chunkLayout, bucket.members, chunk, settings.minShapesPerChunk)) {
							chunk.vertexCount = 0;
						}
					}
				}
				job->stats.bakeMs = Ms(start);
				job->baked.store(true, std::memory_order_release);  // the job is the main thread's again
			}
		}

		void Submit(Job* a_job)
		{
			{
				std::scoped_lock lock{ g_queueMutex };
				g_queue.push_back(a_job);
			}
			g_queueCv.notify_one();
		}

		// ---- helpers -------------------------------------------------------------------------------------------

		// Nothing is applied: switched off, workshop mode, halted, or previs drawing the main view.
		[[nodiscard]] bool Paused() noexcept
		{
			return !g_active || g_workshop || g_halted || g_previsPaused;
		}

		[[nodiscard]] bool CulledByGame(const RE::NiAVObject* a_object)
		{
			return a_object->GetAppCulled() && !g_ours.contains(a_object);
		}

		[[nodiscard]] bool IsCombinableBase(const RE::TESObjectREFR* a_ref)
		{
			const auto base = a_ref->GetObjectReference();
			return base && (base->Is(RE::ENUM_FORM_ID::kSTAT) || base->Is(RE::ENUM_FORM_ID::kSCOL));
		}

		void Notify(const std::string& a_text)
		{
			logger::info("{}", a_text);
			if (Settings::Get().notify) {
				RE::SendHUDMessage::ShowHUDMessage(a_text.c_str(), nullptr, false, false);  // nullptr: no sound (FO4-ENGINE-NOTES 2.2)
			}
		}

		void MarkChanged(CellState& a_state)
		{
			a_state.dirty = true;
			a_state.lastChange = g_frame;
		}

		// Shows the originals again and hides the chunks: flag writes only, safe inside engine callbacks. Each chunk is
		// hidden itself too: previs tests its dynamic objects by their own flag, not their parents' (5.5d).
		void Fallback(CellState& a_state)
		{
			const auto applied = a_state.applied.get();
			if (!applied || applied->fallback) {
				return;
			}
			for (auto& container : applied->containers) {
				if (container.node && !container.node->GetAppCulled()) {
					container.node->SetAppCulled(true);
				}
			}
			for (auto& record : applied->chunks) {
				if (const auto fed = record.Fed(); fed && !fed->GetAppCulled()) {
					fed->SetAppCulled(true);
				}
			}
			for (auto& hidden : applied->hidden) {
				if (hidden.object && hidden.object->GetAppCulled()) {
					hidden.object->SetAppCulled(false);
				}
			}
			applied->fallback = true;
		}

		// Fallback, then takes the chunks out of previs and the scene and drops every reference held (main thread,
		// outside engine callbacks that walk the parent's children). The engine's queued removal keeps each chunk
		// alive until previs lets go of it (FO4-ENGINE-NOTES 5.5d).
		void Revert(CellState& a_state)
		{
			if (!a_state.applied) {
				return;
			}
			Fallback(a_state);
			auto applied = std::move(a_state.applied);
			for (auto& record : applied->chunks) {
				if (std::exchange(record.registered, false)) {
					Engine::UnregisterPrevisObject(record.Fed());
				}
			}
			for (const auto& hidden : applied->hidden) {
				g_ours.erase(hidden.object.get());
				g_hiddenRefs.erase(hidden.formID);
			}
			for (auto& container : applied->containers) {
				if (const auto node = container.node.get(); node && node->parent) {
					node->parent->DetachChild(node);
				}
			}
			++g_totals.reverts;
		}

		void RevertAll()
		{
			for (auto& [cell, state] : g_cells) {
				Revert(state);
				MarkChanged(state);
			}
		}

		// ---- probes --------------------------------------------------------------------------------------------

		void DetachProbes(CellState& a_state)
		{
			for (auto& probe : a_state.probes) {
				if (const auto node = probe.node.get(); node && node->parent) {
					node->parent->DetachChild(node);
				}
			}
			a_state.probes.clear();
			a_state.blind = false;
		}

		// Previs fed the last frame's views (the query ran with previs active and the query hook is in): the chunks,
		// registered as its dynamic objects, reach the main view through it while group 0 goes undrawn
		// (FO4-ENGINE-NOTES 5.3, 5.5d).
		[[nodiscard]] bool PrevisFeeds() noexcept
		{
			return g_previsHook && g_frame - g_previsTick <= 2;
		}

		// One probe per parent the job's buckets hang under (where its containers will go). False while any of them
		// is new, or neither drawn in the last frame nor covered by the previs feed: the cell waits.
		[[nodiscard]] bool EnsureProbes(CellState& a_state, const Job& a_job)
		{
			bool ready = !a_state.blind;
			for (const auto& bucket : a_job.buckets) {
				const auto parent = bucket.parent;
				const auto found = std::ranges::find(a_state.probes, parent, &Probe::parent);
				if (found != a_state.probes.end()) {
					ready = ready && (found->seen > 0 || PrevisFeeds());
					continue;
				}
				ready = false;
				RE::NiPointer<RE::BSFadeNode> node{ Engine::NewProbe() };
				if (!node) {
					continue;
				}
				parent->AttachChild(node.get(), false);  // appended, as the containers are
				Engine::UpdateStatic(node.get());
				a_state.probes.push_back({ std::move(node), parent });
			}
			return ready;
		}

		// The job's every parent has a probe the last frame drew, or the previs feed covers the main view.
		[[nodiscard]] bool ProbesDrawn(const CellState& a_state, const Job& a_job)
		{
			if (a_state.blind) {
				return false;
			}
			const bool feeds = PrevisFeeds();
			return std::ranges::all_of(a_job.buckets, [&](const Bucket& a_bucket) {
				const auto found = std::ranges::find(a_state.probes, a_bucket.parent, &Probe::parent);
				return found != a_state.probes.end() && (found->seen > 0 || feeds);
			});
		}

		// Each tick: did the last frame's main view draw each probe? Group 0 goes undrawn while previs feeds the main
		// view (FO4-ENGINE-NOTES 5.3); previs then draws the chunks as its dynamic objects. A cell whose probe went unseen for
		// kProbeLostTicks frames with no previs feed either shows its originals; it is rebuilt once every probe is
		// back for kProbeBackTicks frames, or the feed is. A probe the engine detached (its parent rebuilt) is
		// dropped and made again by the next gather.
		void WatchProbes()
		{
			std::size_t lost = 0;
			std::size_t back = 0;
			const bool  feeds = PrevisFeeds();
			g_group0Drawn = false;
			for (auto& [cell, state] : g_cells) {
				if (state.probes.empty()) {
					continue;
				}
				bool anyLost = false;
				bool allBack = true;
				for (auto it = state.probes.begin(); it != state.probes.end();) {
					auto& probe = *it;
					if (!probe.node || probe.node->parent != probe.parent) {
						if (probe.node && probe.node->parent) {
							probe.node->parent->DetachChild(probe.node.get());
						}
						it = state.probes.erase(it);
						MarkChanged(state);
						allBack = false;
						continue;
					}
					if (Engine::TakeProbeSeen(probe.node.get())) {
						++probe.seen;
						probe.unseen = 0;
						g_group0Drawn = true;
					} else {
						++probe.unseen;
						probe.seen = 0;
					}
					anyLost = anyLost || probe.unseen >= kProbeLostTicks;
					allBack = allBack && probe.seen >= kProbeBackTicks;
					++it;
				}
				if (state.probes.empty()) {
					state.blind = false;  // nothing left to tell: the next gather makes new probes and waits for them
					continue;
				}
				if (!state.blind && anyLost && !feeds) {
					state.blind = true;
					if (state.applied) {
						Revert(state);
						++g_totals.blind;
					}
					MarkChanged(state);
					++lost;
				} else if (state.blind && (allBack || feeds)) {
					state.blind = false;
					MarkChanged(state);
					++back;
				}
			}
			if (lost) {
				logger::info("the main view stopped drawing the combined meshes' group in {} cells and previs doesn't feed it: their original meshes are shown", lost);
			}
			if (back) {
				logger::info("the combined meshes are drawn again in {} cells (group 0 or the previs feed): combining them again", back);
			}
		}

		void ReleaseTriShapes(Job& a_job)
		{
			for (auto& triShape : a_job.triShapes) {
				Engine::ReleaseTriShape(triShape);
				triShape = nullptr;
			}
		}

		std::string CellName(RE::TESObjectCELL* a_cell, std::uint32_t a_formID)
		{
			const char* editorID = a_cell ? a_cell->GetFormEditorID() : nullptr;
			return editorID && *editorID ? std::format("{:08X} {}", a_formID, editorID) : std::format("{:08X}", a_formID);
		}

		std::string SkipSummary(const Stats& a_stats)
		{
			std::string text;
			for (std::size_t i = 0; i < a_stats.skips.size(); ++i) {
				if (a_stats.skips[i]) {
					text += std::format("{}{} {}", text.empty() ? "" : ", ", SkipName(static_cast<Skip>(i)), a_stats.skips[i]);
				}
			}
			return text.empty() ? "none"s : text;
		}

		void LogCell(const Job& a_job, std::string_view a_what)
		{
			if (!Settings::Get().logCells) {
				return;
			}
			const auto& s = a_job.stats;
			logger::info(
				"cell {} {}: {} references, {} with meshes captured ({} meshes); {} chunks replace {} meshes ({} tris, {} verts, {:.1f} MB); "
				"{} references hidden whole, {} partly; gather {:.2f} ms, bake {:.2f} ms (worker), apply {:.2f} ms; left out: {}",
				CellName(a_job.cell, a_job.cellFormID), a_what, s.references, s.candidates, s.capturedShapes, s.chunks, s.bakedShapes,
				s.triangles, s.vertices, static_cast<double>(s.vertexBytes) / (1 << 20), s.wholeRefs, s.partialRefs, s.gatherMs, s.bakeMs, s.applyMs, SkipSummary(s));
		}

		// ---- previs --------------------------------------------------------------------------------------------

		// Render_PreUI, right after the previs query (main thread, before the cull): notes that previs fed this
		// frame's views. The query draws the chunks itself: each is one of its dynamic objects (Apply), tested and
		// added to the main view, the sun's cascades and the precipitation map as their originals were
		// (FO4-ENGINE-NOTES 5.5d). With CBRO in classic mode the main and sun records go unused (CBRO suspends previs
		// over the cull and the cascades) and group 0 draws the chunks instead.
		// Also reads, at the one moment it means that, which chunks the query's main-view pass found visible
		// (Engine::SeenByMainView): for the audit.
		void OnPrevisQuery() noexcept
		{
			if (!Engine::PrevisActive()) {
				return;
			}
			g_previsTick = g_frame;
			++g_previsFrames;
			std::uint32_t visible = 0;
			for (auto& [cell, state] : g_cells) {
				if (!state.applied || state.applied->fallback) {
					continue;
				}
				for (auto& record : state.applied->chunks) {
					if (record.registered && Engine::SeenByMainView(record.Fed())) {
						record.previsVisible = g_frame;
						++visible;
					}
				}
			}
			g_previsVisible = visible;
		}

		// ---- apply ---------------------------------------------------------------------------------------------

		// The scene must still be what was captured: same 3D, same transforms, same meshes.
		[[nodiscard]] bool StillValid(const Job& a_job)
		{
			for (const auto& source : a_job.refs) {
				const auto ref = source.ref.get();
				const auto root = source.root.get();
				if (!ref || (ref->GetFormFlags() & kFormDeletedOrDisabled)) {
					return false;
				}
				const auto loaded = ref->loadedData;
				if (!loaded || loaded->data3D.get() != root || root->parent != source.parent || CulledByGame(root) ||
					!(World(root->world) == World(source.rootWorld))) {
					return false;
				}
			}
			for (const auto& source : a_job.shapes) {
				const auto shape = source.shape.get();
				if (!shape->parent || *Engine::Field<const void*>(shape, Engine::Offset::kRendererData) != source.rendererData ||
					!(World(shape->world) == World(source.world))) {
					return false;
				}
			}
			return true;
		}

		// Hands the chunks' data to the renderer, continuing where the last frame stopped, until a_budget bytes are
		// spent (the copy is the main-thread cost). Sets created once every chunk has its TriShape (or none).
		void CreateTriShapes(Job& a_job, std::size_t& a_budget)
		{
			std::size_t index = 0;
			for (auto& bucket : a_job.buckets) {
				for (auto& chunk : bucket.chunks) {
					if (index++ < a_job.triShapes.size()) {
						continue;
					}
					const auto bytes = chunk.vertices.size() + chunk.indices.size() * sizeof(std::uint16_t);
					if (a_budget == 0) {
						return;
					}
					a_budget = bytes >= a_budget ? 0 : a_budget - bytes;
					void* triShape = nullptr;
					if (chunk.vertexCount) {
						triShape = Engine::CreateTriShape(chunk.vertices.data(), static_cast<std::uint32_t>(chunk.vertices.size()), bucket.chunkDesc,
							chunk.indices.data(), static_cast<std::uint32_t>(chunk.indices.size()));
					}
					a_job.triShapes.push_back(triShape);
					// The engine copied the data (its buffers keep a CPU copy).
					std::vector<std::byte>().swap(chunk.vertices);
					std::vector<std::uint16_t>().swap(chunk.indices);
				}
			}
			a_job.created = true;
		}

		[[nodiscard]] bool TriShapesReady(const Job& a_job)
		{
			return std::ranges::all_of(a_job.triShapes, [](const void* a_triShape) { return !a_triShape || Engine::TriShapeReady(a_triShape); });
		}

		[[nodiscard]] std::size_t ChunkCount(const Job& a_job) noexcept
		{
			std::size_t count = 0;
			for (const auto& bucket : a_job.buckets) {
				count += bucket.chunks.size();
			}
			return count;
		}

		void Apply(Job& a_job, CellState& a_state)
		{
			const auto start = Clock::now();
			// One TriShape (or none) per chunk, in bucket-major order: anything else is a bookkeeping error, and
			// pairing a chunk with another chunk's data would draw the wrong mesh.
			if (a_job.triShapes.size() != ChunkCount(a_job)) {
				logger::error("cell {:08X}: {} TriShapes for {} chunks; not applied", a_job.cellFormID, a_job.triShapes.size(), ChunkCount(a_job));
				return;
			}
			// With bChunkFadeNodes, a cell gathered while its references are still fading in after attach: each chunk
			// carries on from the furthest fade among its originals, read before they are hidden. An original this
			// plugin hid is drawn by the old chunk, faded in (FO4-ENGINE-NOTES 7.8). Without, chunks are drawn at once.
			const bool         fadeNodes = Settings::Get().chunkFadeNodes;
			std::vector<float> fades;
			for (const auto& bucket : a_job.buckets) {
				for (const auto& chunk : bucket.chunks) {
					if (!fadeNodes) {
						fades.push_back(1.0F);
						continue;
					}
					float fade = 0.0F;
					for (const auto member : chunk.baked) {
						const auto& shape = a_job.shapes[bucket.shapes[member]];
						const auto  root = a_job.refs[shape.ref].root.get();
						const bool  ours = g_ours.contains(shape.shape.get()) || g_ours.contains(root);
						fade = std::max(fade, ours ? 1.0F : Engine::FadeIn(root));
					}
					fades.push_back(fade);
				}
			}

			Revert(a_state);  // the previous version, if any, in the same frame: no gap

			auto applied = std::make_unique<Applied>();
			applied->frame = g_frame;
			std::unordered_map<RE::NiNode*, RE::NiNode*> containers;
			auto& stats = a_job.stats;

			std::size_t chunkIndex = 0;
			for (auto& bucket : a_job.buckets) {
				for (auto& chunk : bucket.chunks) {
					auto& triShape = a_job.triShapes[chunkIndex++];
					if (!triShape) {
						continue;
					}
					// The chunk's mesh is a clone of one member (its shader and alpha properties, flags and name)
					// with the combined data in place of the member's.
					const auto representative = a_job.shapes[bucket.shapes[chunk.baked.front()]].shape.get();
					RE::NiPointer<RE::NiAVObject> clone{ Engine::Clone(representative) };
					if (!clone || !Engine::IsExactTriShape(clone.get())) {
						Engine::ReleaseTriShape(triShape);
						triShape = nullptr;
						continue;
					}
					Engine::SetGeometry(clone.get(), triShape, chunk.vertexCount, chunk.triangleCount);
					triShape = nullptr;  // owned by the clone now

					auto& bound = *Engine::Field<RE::NiBound>(clone.get(), Engine::Offset::kModelBound);
					bound.center = RE::NiPoint3{ chunk.boundCenter[0], chunk.boundCenter[1], chunk.boundCenter[2] };
					bound.fRadius = chunk.boundRadius;
					// Parent world rotation is identity with unit scale (Gather checks), so local = world - parent.
					const auto& parentWorld = bucket.parent->world.translate;
					clone->local.MakeIdentity();
					clone->local.translate = RE::NiPoint3{ chunk.origin[0] - parentWorld.x, chunk.origin[1] - parentWorld.y, chunk.origin[2] - parentWorld.z };
					clone->flags.flags &= ~(Engine::ObjectFlag::kAppCulled | Engine::ObjectFlag::kNotVisible | Engine::ObjectFlag::kAccumulated);
					clone->fadeAmount = 1.0F;
					clone->meshLODFadingLevel = 0;
					clone->currentMeshLODLevel = 0;
					clone->previousMeshLODLevel = 0;

					// With bChunkFadeNodes, under its own fade node with its members' fade range (a bucket holds one fade
					// class, Gather): the engine fades it out where they would fade (FO4-ENGINE-NOTES 7.9). Otherwise
					// straight in the container.
					RE::NiPointer<RE::BSFadeNode> fadeNode;
					if (fadeNodes) {
						Engine::FadeRange range;
						if (bucket.fadeKey) {
							range.nearDistance = bucket.fadeNear;
							range.farDistance = bucket.fadeFar;
							range.type = bucket.fadeType;
						}
						fadeNode.reset(Engine::NewChunkFadeNode(clone.get(), fades[chunkIndex - 1], range));
						if (!fadeNode) {
							continue;
						}
					}

					auto& container = containers[bucket.parent];
					if (!container) {
						const auto node = Engine::NewNode(8);
						if (!node) {
							continue;
						}
						auto& entry = applied->containers.emplace_back();
						entry.node.reset(node);
						entry.parent = bucket.parent;
						node->name = RE::BSFixedString("RuntimeCombiner");
						node->local.MakeIdentity();
						bucket.parent->AttachChild(node, false);  // appended: no empty slot of the parent is reused
						container = node;
					}
					container->AttachChild(fadeNode ? static_cast<RE::NiAVObject*>(fadeNode.get()) : clone.get(), true);
					auto& record = applied->chunks.emplace_back();
					record.shape = clone;
					record.fadeNode = fadeNode.get();
					for (const auto member : chunk.baked) {
						const auto formID = a_job.refs[a_job.shapes[bucket.shapes[member]].ref].formID;
						if (std::ranges::find(record.refs, formID) == record.refs.end()) {
							record.refs.push_back(formID);
						}
					}
					record.members = static_cast<std::uint32_t>(chunk.baked.size());
					record.triangles = chunk.triangleCount;

					for (const auto member : chunk.baked) {
						auto& source = a_job.shapes[bucket.shapes[member]];
						source.baked = true;
						++a_job.refs[source.ref].baked;
					}
					++stats.chunks;
					stats.bakedShapes += static_cast<std::uint32_t>(chunk.baked.size());
					stats.triangles += chunk.triangleCount;
					stats.vertices += chunk.vertexCount;
					stats.vertexBytes += static_cast<std::uint64_t>(chunk.vertexCount) * bucket.chunkLayout.stride;
				}
			}
			// Placed for good: their previousWorld is their world, or TAA would see each chunk move in from wherever its
			// clone was built (FO4-ENGINE-NOTES 7.10).
			for (auto& container : applied->containers) {
				Engine::UpdateStatic(container.node.get());
			}

			// Hide what the chunks now draw: a reference whose every part was combined is hidden at its root (the
			// scene walk skips it), others mesh by mesh.
			const auto hide = [&](RE::NiAVObject* a_object, const SourceRef& a_ref, bool a_root) {
				a_object->SetAppCulled(true);
				auto& hidden = applied->hidden.emplace_back();
				hidden.object.reset(a_object);
				hidden.ref = a_ref.ref;
				hidden.root = a_ref.root;
				hidden.formID = a_ref.formID;
				hidden.world = World(a_object->world);
				if (const auto node = a_root ? a_object->IsNode() : nullptr) {
					hidden.children = node->children.size();
				}
				g_ours.insert(a_object);
				g_hiddenRefs[a_ref.formID] = a_job.cell;
			};
			std::vector<bool> wholeHidden(a_job.refs.size(), false);
			for (std::size_t i = 0; i < a_job.refs.size(); ++i) {
				const auto& ref = a_job.refs[i];
				if (ref.baked && ref.whole && ref.baked == ref.captured) {
					hide(ref.root.get(), ref, true);
					wholeHidden[i] = true;
					++stats.wholeRefs;
				} else if (ref.baked) {
					++stats.partialRefs;
				}
			}
			for (const auto& shape : a_job.shapes) {
				if (shape.baked && !wholeHidden[shape.ref]) {
					hide(shape.shape.get(), a_job.refs[shape.ref], false);
				}
			}

			// While previs is active it draws the main view, the sun's cascades and the precipitation map from its
			// per-frame query, not group 0. A static reference outside its cell's previs list (all of them with
			// precombines off) is one of the query's dynamic objects, tested and drawn each frame: so is each chunk,
			// in its originals' place (FO4-ENGINE-NOTES 5.5d).
			for (auto& record : applied->chunks) {
				record.registered = Engine::RegisterPrevisObject(record.Fed());
			}
			stats.applyMs = Ms(start);
			if (!applied->containers.empty()) {
				a_state.applied = std::move(applied);
				++g_totals.applies;
			}
			LogCell(a_job, stats.chunks ? "combined"sv : "nothing combined (no chunk of enough meshes)"sv);
		}

		// ---- jobs ----------------------------------------------------------------------------------------------

		void ProcessJobs()
		{
			const auto& settings = Settings::Get();
			std::size_t createBudget = kCreateBytesPerFrame;
			for (auto it = g_jobs.begin(); it != g_jobs.end();) {
				auto& job = **it;
				if (!job.baked.load(std::memory_order_acquire)) {
					++it;
					continue;
				}
				const auto found = g_cells.find(job.cell);
				const bool current = found != g_cells.end() && found->second.generation == job.generation;
				if (!current || Paused()) {
					if (current) {
						found->second.building = false;
						MarkChanged(found->second);
					}
					ReleaseTriShapes(job);
					++g_totals.discarded;
					it = g_jobs.erase(it);
					continue;
				}
				auto& state = found->second;
				if (!settings.apply) {
					for (const auto& bucket : job.buckets) {
						for (const auto& chunk : bucket.chunks) {
							if (chunk.vertexCount) {
								++job.stats.chunks;
								job.stats.bakedShapes += static_cast<std::uint32_t>(chunk.baked.size());
								job.stats.triangles += chunk.triangleCount;
								job.stats.vertices += chunk.vertexCount;
								job.stats.vertexBytes += static_cast<std::uint64_t>(chunk.vertexCount) * bucket.chunkLayout.stride;
							}
						}
					}
					LogCell(job, "dry run (bApply=0)"sv);
					state.building = false;
					it = g_jobs.erase(it);
					continue;
				}
				if (!job.created) {
					CreateTriShapes(job, createBudget);
					if (!job.created) {
						++it;  // the frame's budget ran out: the rest next frame
						continue;
					}
				}
				if (!TriShapesReady(job)) {
					++it;
					continue;
				}
				state.building = false;
				if (!StillValid(job) || !ProbesDrawn(state, job)) {
					ReleaseTriShapes(job);
					MarkChanged(state);
					++g_totals.discarded;
					it = g_jobs.erase(it);
					continue;
				}
				Apply(job, state);
				ReleaseTriShapes(job);  // any left over (chunks that failed to apply)
				it = g_jobs.erase(it);
			}
		}

		void StartJobs()
		{
			const auto& settings = Settings::Get();
			const auto  start = Clock::now();
			for (auto& [cell, state] : g_cells) {
				if (!state.dirty || state.building || state.blind || g_frame - state.lastChange < settings.settleFrames) {
					continue;
				}
				if (Ms(start) > settings.gatherBudgetMs) {
					break;
				}
				state.dirty = false;
				auto job = std::make_unique<Job>();
				job->cell = cell;
				job->cellFormID = state.formID;
				job->generation = ++state.generation;
				job->started = Clock::now();
				Gather::Cell(cell, g_ours, g_meshByMesh, *job);
				state.blocked = job->stats.blockedClasses;
				if (job->buckets.empty()) {
					LogCell(*job, "nothing to combine"sv);
					Revert(state);  // nothing to combine any more
					continue;
				}
				if (!EnsureProbes(state, *job)) {
					MarkChanged(state);  // a new probe, or one the last frame didn't draw: gather again after a settle
					continue;
				}
				state.building = true;
				Submit(job.get());
				g_jobs.push_back(std::move(job));
			}

			const auto votes = Gather::Votes();
			if (!g_halted && votes.disagree > 16 && votes.disagree > votes.agree) {
				g_halted = true;
				logger::error(
					"rotation self-check failed ({} meshes match the other convention, {} this one): combining stopped, everything restored",
					votes.disagree, votes.agree);
				RevertAll();
			}
		}

		// ---- watchdog ------------------------------------------------------------------------------------------

		[[nodiscard]] Check CheckHidden(const Hidden& a_hidden)
		{
			const auto ref = a_hidden.ref.get();
			if (!ref || (ref->GetFormFlags() & kFormDeletedOrDisabled)) {
				return Check::kStale;
			}
			const auto loaded = ref->loadedData;
			if (!loaded || loaded->data3D.get() != a_hidden.root.get()) {
				return Check::kStale;
			}
			const auto object = a_hidden.object.get();
			if (!object->GetAppCulled() || !object->parent || !(World(object->world) == a_hidden.world)) {
				return Check::kStale;
			}
			if (a_hidden.children != kNoChildCount) {
				if (const auto node = object->IsNode(); node && node->children.size() > a_hidden.children) {
					return Check::kAttached;
				}
			}
			return Check::kOk;
		}

		// Re-checks a slice of the hidden objects each frame: a moved, unloaded, disabled or re-shown original
		// means the chunk is stale; the cell shows its originals and is rebuilt. Something attached under a
		// hidden root (a decal, an effect) would stay hidden with it: that reference keeps its root visible from
		// then on and has only its meshes hidden.
		void Watchdog()
		{
			auto budget = Settings::Get().watchdogRefsPerFrame;
			std::size_t total = 0;
			for (const auto& [cell, state] : g_cells) {
				total += state.applied ? state.applied->hidden.size() + state.applied->containers.size() : 0;
			}
			if (total == 0) {
				return;
			}
			if (g_watchCursor >= total) {
				g_watchCursor = 0;
			}
			std::size_t position = 0;
			for (auto& [cell, state] : g_cells) {
				if (!state.applied || budget == 0) {
					continue;
				}
				const auto& applied = *state.applied;
				const auto  count = applied.hidden.size() + applied.containers.size();
				if (position + count <= g_watchCursor) {
					position += count;
					continue;
				}
				bool stale = applied.fallback;
				for (auto i = g_watchCursor - position; i < count && budget > 0 && !stale; ++i, --budget, ++g_watchCursor) {
					if (i < applied.hidden.size()) {
						const auto& hidden = applied.hidden[i];
						const auto  result = CheckHidden(hidden);
						if (result == Check::kAttached) {
							g_meshByMesh.insert(hidden.formID);
							++g_totals.attached;
							logger::info("reference {:08X}: something was attached under its hidden root; from now on only its meshes are hidden", hidden.formID);
						}
						stale = result != Check::kOk;
					} else {
						const auto& container = applied.containers[i - applied.hidden.size()];
						stale = !container.node || container.node->parent != container.parent;
					}
				}
				position += count;
				if (stale) {
					++g_totals.watchdog;
					Revert(state);
					MarkChanged(state);
					state.lastChange = 0;  // rebuild at once: the originals are drawing one by one meanwhile
				}
			}
		}

		// ---- cells and events ----------------------------------------------------------------------------------

		// The attached cells are read from the engine every frame (TES's interior cell or exterior grid). Attach and
		// detach events arrive on the loading thread during loading screens; only their type values are logged.
		void PollCells()
		{
			const auto  loaded = Engine::LoadedCells();
			std::size_t added = 0;
			std::size_t removed = 0;
			for (const auto cell : loaded) {
				if (!g_cells.contains(cell)) {
					auto& state = g_cells[cell];
					state.formID = cell->GetFormID();
					MarkChanged(state);
					++added;
				}
			}
			for (auto it = g_cells.begin(); it != g_cells.end();) {
				if (std::ranges::find(loaded, it->first) == loaded.end()) {
					Revert(it->second);
					DetachProbes(it->second);
					it = g_cells.erase(it);
					++removed;
				} else {
					++it;
				}
			}
			if (added || removed) {
				logger::info("attached cells: {} (+{}, -{})", g_cells.size(), added, removed);
			}
		}

		void OnObject(std::uint32_t a_formID, bool a_loaded)
		{
			if (const auto hidden = g_hiddenRefs.find(a_formID); hidden != g_hiddenRefs.end()) {
				if (const auto found = g_cells.find(hidden->second); found != g_cells.end()) {
					if (!a_loaded) {
						Fallback(found->second);  // the full revert waits for the next frame
					}
					MarkChanged(found->second);
				}
				return;
			}
			const auto form = RE::TESForm::GetFormByID(a_formID);
			if (!form || form->GetFormType() != RE::ENUM_FORM_ID::kREFR) {
				return;
			}
			const auto ref = static_cast<RE::TESObjectREFR*>(form);
			if (!IsCombinableBase(ref)) {
				return;
			}
			if (const auto found = g_cells.find(ref->GetParentCell()); found != g_cells.end()) {
				MarkChanged(found->second);
			}
		}

		struct PendingObject
		{
			std::uint32_t formID{ 0 };
			bool          loaded{ false };
		};
		std::mutex                 g_eventMutex;
		std::vector<PendingObject> g_events;
		std::atomic<std::uint32_t> g_cellEventsLogged{ 0 };

		class CellSink final : public RE::BSTEventSink<RE::CellAttachDetachEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::CellAttachDetachEvent& a_event, RE::BSTEventSource<RE::CellAttachDetachEvent>*) override
			{
				if (a_event.cell && g_cellEventsLogged.fetch_add(1) < 8) {
					logger::info("cell event: type {} for {:08X} (thread {})", static_cast<int>(a_event.type.get()), a_event.cell->GetFormID(), GetCurrentThreadId());
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		// Object (un)load events: handled at once on the game thread, queued from any other.
		class ObjectSink final : public RE::BSTEventSink<RE::TESObjectLoadedEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESObjectLoadedEvent& a_event, RE::BSTEventSource<RE::TESObjectLoadedEvent>*) override
			{
				if (GetCurrentThreadId() == g_mainThread.load(std::memory_order_relaxed)) {
					OnObject(a_event.formID, a_event.loaded);
				} else {
					std::scoped_lock lock{ g_eventMutex };
					g_events.push_back({ a_event.formID, a_event.loaded });
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		CellSink   g_cellSink;
		ObjectSink g_objectSink;

		void DrainEvents()
		{
			std::vector<PendingObject> events;
			{
				std::scoped_lock lock{ g_eventMutex };
				events.swap(g_events);
			}
			for (const auto& event : events) {
				OnObject(event.formID, event.loaded);
			}
		}

		// ---- frame ---------------------------------------------------------------------------------------------

		void Toggle()
		{
			g_active = !g_active;
			if (g_active) {
				for (auto& [cell, state] : g_cells) {
					state.dirty = true;
					state.lastChange = 0;  // settled: rebuild now
				}
				Notify("Runtime Combiner: on"s);
			} else {
				RevertAll();
				Notify("Runtime Combiner: off (original meshes)"s);
			}
		}

		// Inside a cone of kAuditHalfCone around the camera's view direction (its world rotation's row 0,
		// FO4-ENGINE-NOTES 6.1), within kAuditRange units, with the camera outside the bound: on screen unless
		// something stands in front of it.
		constexpr float kAuditRange = 2000.0F;
		constexpr float kAuditHalfCone = 0.7F;  // radians, about 40 degrees

		[[nodiscard]] bool InView(const RE::NiCamera& a_camera, const RE::NiBound& a_bound)
		{
			const auto& eye = a_camera.world.translate;
			const auto& forward = a_camera.world.rotate.entry[0];
			const float dx = a_bound.center.x - eye.x;
			const float dy = a_bound.center.y - eye.y;
			const float dz = a_bound.center.z - eye.z;
			const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
			if (distance <= a_bound.fRadius || distance - a_bound.fRadius > kAuditRange) {
				return false;
			}
			const float cosine = (dx * forward.pt[0] + dy * forward.pt[1] + dz * forward.pt[2]) / distance;
			return cosine >= std::cos(std::min(kAuditHalfCone + std::asin(a_bound.fRadius / distance), 3.14159F));
		}

		// What the renderer made of the applied chunks: drawn at least once (render passes exist) and blended, and
		// the chunks on screen that were never drawn (in view, applied over 120 frames ago), which should be none.
		void AuditChunks()
		{
			struct Suspect
			{
				float              distance{ 0.0F };
				const ChunkRecord* record{ nullptr };
				std::uint32_t      cell{ 0 };
				const char*        material{ nullptr };
				std::uint64_t      shaderFlags{ 0 };
			};
			std::size_t          total = 0, drawn = 0, blended = 0, inView = 0, fading = 0, foreign = 0, filed = 0, moving = 0, unreadable = 0;
			std::vector<Suspect> suspects;
			const auto           camera = RE::Main::WorldRootCamera();
			// A chunk in view is expected drawn when nothing culls it but the frustum (group 0 drawn, no CBRO, an
			// exterior: interiors cull by portals), or when the main view draws previs's records and the last query
			// found it visible. CBRO's own culling of group 0 can't be read here: no expectation then.
			const bool frustumOnly = g_group0Drawn && !g_cbroLoaded && !Engine::InInterior();
			const bool previsDraws = PrevisFeeds() && !g_group0Drawn;
			for (const auto& [cell, state] : g_cells) {
				if (!state.applied) {
					continue;
				}
				const bool settled = g_frame - state.applied->frame > 120;
				for (const auto& record : state.applied->chunks) {
					++total;
					Engine::ChunkView view;
					if (!record.shape || !Engine::ReadChunkView(record.shape.get(), record.fadeNode, view)) {
						++unreadable;
						continue;
					}
					drawn += view.passes ? 1 : 0;
					blended += view.passes && view.alpha < 1.0F ? 1 : 0;
					fading += view.fade < 1.0F ? 1 : 0;
					foreign += view.owner ? 0 : 1;
					filed += view.inView ? 1 : 0;
					moving += view.moving ? 1 : 0;
					const bool expected = frustumOnly || (previsDraws && g_frame - record.previsVisible <= 1);
					if (camera && settled && expected && InView(*camera, record.shape->worldBound)) {
						++inView;
						if (!view.passes) {
							const auto& eye = camera->world.translate;
							suspects.push_back({ record.shape->worldBound.center.GetDistance(eye), &record, state.formID, view.material,
								view.shaderFlags });
						}
					}
				}
			}
			if (total == 0) {
				return;
			}
			std::string fadeNodes;
			if (Settings::Get().chunkFadeNodes) {
				fadeNodes = std::format("; fade nodes: filed by a view at the last cull {}, fading {}, fade alpha from another node {}", filed,
					fading, foreign);
			}
			logger::info("chunk audit: {} chunks, drawn at least once {} (blended {}), moving for TAA {} (should be 0); on screen within {:.0f} "
						 "units and expected drawn {}, never drawn {}{}; unreadable {}",
				total, drawn, blended, moving, kAuditRange, inView, suspects.size(), fadeNodes, unreadable);
			const auto count = std::min<std::size_t>(suspects.size(), 3);
			std::partial_sort(suspects.begin(), suspects.begin() + count, suspects.end(), [](const Suspect& a_l, const Suspect& a_r) { return a_l.distance < a_r.distance; });
			for (std::size_t i = 0; i < count; ++i) {
				const auto& s = suspects[i];
				logger::warn("  on screen but never drawn: cell {:08X}, {:.0f} units from the camera, {} meshes (first reference {:08X}), {} tris, "
							 "mesh '{}', material '{}', shader flags {:016X}",
					s.cell, s.distance, s.record->members, s.record->refs.empty() ? 0u : s.record->refs.front(), s.record->triangles,
					s.record->shape->name.c_str(), s.material ? s.material : "", s.shaderFlags);
			}
		}

		// Previs since the last summary: frames whose query ran with previs active, and the chunks it tests as its
		// dynamic objects.
		void LogPrevis()
		{
			if (!g_previsHook) {
				return;
			}
			std::size_t registered = 0;
			for (const auto& [cell, state] : g_cells) {
				if (state.applied) {
					registered += std::ranges::count_if(state.applied->chunks, &ChunkRecord::registered);
				}
			}
			logger::info("previs: active in {} frames, {} chunks registered as its dynamic objects, {} found visible by its last main-view "
						 "pass; group 0 drawn last frame: {} (yes: previs off, or suspended over the cull as CBRO classic does)",
				std::exchange(g_previsFrames, 0), registered, PrevisFeeds() ? g_previsVisible : 0u, g_group0Drawn ? "yes" : "no");
		}

		// The blocked parts of the attached cells by class: what keeps references from being hidden whole and
		// meshes from being combined. Logged when it changed.
		void LogBlocked()
		{
			std::vector<std::pair<std::string_view, std::uint64_t>> totals;
			for (const auto& [cell, state] : g_cells) {
				for (const auto& [name, count] : state.blocked) {
					const std::string_view key{ name };
					const auto             found = std::ranges::find(totals, key, &std::pair<std::string_view, std::uint64_t>::first);
					if (found != totals.end()) {
						found->second += count;
					} else {
						totals.emplace_back(key, count);
					}
				}
			}
			std::ranges::sort(totals, [](const auto& a_l, const auto& a_r) { return a_l.second > a_r.second; });
			std::uint64_t hash = 1469598103934665603ull;
			std::uint64_t all = 0;
			std::string   text;
			for (const auto& [name, count] : totals) {
				hash = (hash ^ std::hash<std::string_view>{}(name)) * 1099511628211ull;
				hash = (hash ^ count) * 1099511628211ull;
				all += count;
				if (text.size() < 400) {
					text += std::format("{}{} {}", text.empty() ? "" : ", ", name, count);
				}
			}
			if (hash == g_blockedPrinted) {
				return;
			}
			g_blockedPrinted = hash;
			logger::info("blocked parts in the attached cells: {} ({})", all, text.empty() ? "none"s : text);
		}

		void Summary()
		{
			const auto now = Clock::now();
			if (g_cells.empty() || now - g_lastSummary < std::chrono::seconds(10)) {
				return;
			}
			g_lastSummary = now;
			std::size_t cells = 0, chunks = 0, hidden = 0, building = 0, blind = 0;
			for (const auto& [cell, state] : g_cells) {
				building += state.building ? 1 : 0;
				blind += state.blind ? 1 : 0;
				if (state.applied) {
					++cells;
					hidden += state.applied->hidden.size();
					chunks += state.applied->chunks.size();
				}
			}
			const auto votes = Gather::Votes();
			logger::info(
				"summary: {} of {} attached cells combined ({} building, {} not drawn by the main view), {} chunks in the scene, {} originals "
				"hidden; {}{}{}precombines {}; applies {}, reverts {} (main view stopped drawing {}), discarded {}, watchdog {}, attached {}; "
				"rotation check {} agree / {} disagree",
				cells, g_cells.size(), building, blind, chunks, hidden, g_active ? "" : "switched off, ", g_workshop ? "workshop mode, " : "",
				g_previsPaused ? "previs active without the query hook (originals shown), " : "", Engine::PrecombinesEnabled() ? "on" : "off", g_totals.applies,
				g_totals.reverts, g_totals.blind, g_totals.discarded, g_totals.watchdog, g_totals.attached, votes.agree, votes.disagree);
			AuditChunks();
			LogPrevis();
			LogBlocked();
		}

		void Reset()
		{
			RevertAll();
			for (auto& [cell, state] : g_cells) {
				DetachProbes(state);
			}
			g_cells.clear();
			g_hiddenRefs.clear();
			g_ours.clear();
			g_meshByMesh.clear();
			logger::info("game load: everything restored and forgotten");
		}

		void Tick()
		{
			++g_frame;
			if (g_frame == 1) {
				g_mainThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
				g_cbroLoaded = GetModuleHandleW(L"CBRO.dll") != nullptr;
				logger::info("first frame tick (thread {}): precombines {}, previs {}, CBRO {}", GetCurrentThreadId(),
					Engine::PrecombinesEnabled() ? "on" : "off", Engine::PrevisEnabled() ? "on" : "off", g_cbroLoaded ? "loaded" : "not loaded");
			}
			const auto& settings = Settings::Get();
			if (g_resetPending.exchange(false)) {
				Reset();
			}
			PollCells();
			DrainEvents();

			if (settings.toggleHotkey) {
				// Only while the game window has focus (a key pressed in another window doesn't count).
				const auto main = RE::Main::GetSingleton();
				const bool focused = main && GetForegroundWindow() == reinterpret_cast<HWND>(main->hwnd);
				const bool down = focused && (GetAsyncKeyState(static_cast<int>(settings.toggleHotkey)) & 0x8000) != 0;
				if (down && !g_keyDown) {
					Toggle();
				}
				g_keyDown = down;
			}

			static const RE::BSFixedString workshopMenu{ "WorkshopMenu" };
			const auto ui = RE::UI::GetSingleton();
			const bool workshop = ui && ui->GetMenuOpen(workshopMenu);
			if (workshop != g_workshop) {
				g_workshop = workshop;
				if (workshop) {
					RevertAll();
					logger::info("workshop mode: original meshes shown");
				} else {
					for (auto& [cell, state] : g_cells) {
						MarkChanged(state);
					}
				}
			}

			// Without the previs query hook (its site taken, or a previs address missing) nothing tells whether previs
			// drew a frame, and a missing address leaves the chunks out of it. While previs is active the main view
			// draws previs's records instead of group 0 (FO4-ENGINE-NOTES 5.3): show the originals until it is off.
			// Without CBRO nothing suspends previs around the cull, so the tick sees what the cull will; with CBRO the
			// probes catch it.
			const bool previs = !g_previsHook && !g_cbroLoaded && Engine::PrevisActive();
			if (previs != g_previsPaused) {
				g_previsPaused = previs;
				if (previs) {
					RevertAll();
					logger::info("previs is active and the previs query hook is missing: original meshes shown while it is");
				} else {
					for (auto& [cell, state] : g_cells) {
						MarkChanged(state);
					}
					logger::info("previs is off: combining again");
				}
			}

			WatchProbes();

			// Fallbacks from engine callbacks become full reverts here.
			for (auto& [cell, state] : g_cells) {
				if (state.applied && state.applied->fallback) {
					Revert(state);
					MarkChanged(state);
				}
			}

			ProcessJobs();
			if (!Paused()) {
				Watchdog();
				StartJobs();
			}
			Summary();
		}

		void UpdateHook(RE::PlayerCharacter* a_this, float a_delta)
		{
			using func_t = void (*)(RE::PlayerCharacter*, float);
			reinterpret_cast<func_t>(g_originalUpdate)(a_this, a_delta);
			try {
				Tick();
			} catch (const std::exception& e) {
				if (!g_halted) {
					g_halted = true;
					logger::error("tick failed ({}): combining stopped", e.what());
				}
			}
		}
	}

	void Install()
	{
		std::thread(WorkerMain).detach();  // lives as long as the process; never joined at exit
		REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE::PlayerCharacter[0] };
		g_originalUpdate = vtable.write_vfunc(kUpdateSlot, &UpdateHook);
		logger::info("PlayerCharacter::Update (slot 0x{:X}) hooked; bake thread started", kUpdateSlot);
		g_previsHook = Engine::InstallPrevisQueryHook(&OnPrevisQuery);
		if (g_previsHook) {
			logger::info("previs query hooked (Render_PreUI+0x80); chunks are registered as previs dynamic objects, previs stays as it is");
		}
	}

	void OnGameDataReady()
	{
		const auto& settings = Settings::Get();
		if (settings.disablePrecombines) {
			if (Engine::PrecombinesEnabled()) {
				Engine::SetPrecombinesEnabled(false);
				logger::info("precombines switched off (bUseCombinedObjects = 0): every static loads its own 3D and is combined here");
			} else {
				logger::info("precombines already off");
			}
		} else {
			logger::info("precombines left as configured ({}): only references outside them are combined", Engine::PrecombinesEnabled() ? "on" : "off");
		}

		RE::CellAttachDetachEventSource::CellAttachDetachEventSourceSingleton::GetSingleton().source.RegisterSink(&g_cellSink);
		if (const auto source = RE::TESObjectLoadedEvent::GetEventSource()) {
			source->RegisterSink(&g_objectSink);
		}
		logger::info("cell attach/detach and object-loaded sinks registered");
	}

	// Messages arrive on the loading thread: the work waits for the next game frame.
	void OnPreLoadGame()
	{
		g_resetPending = true;
	}

}
