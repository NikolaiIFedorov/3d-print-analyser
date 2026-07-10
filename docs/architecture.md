# Architecture

This document defines the domain model, the reasoning behind each decision, and the algorithms that follow from it. Coding conventions and practices (not domain algorithms) live in `practices/project_practices.md`. What Temper is, who it's for, and how its tools fit into an FDM workflow — the product-level context this doc assumes — lives in [product.md](product.md).

## Glossary

- **FDM** — Fused Deposition Modeling, the 3D printing process this app targets: melted filament extruded layer by layer.
- **CAD** — Computer-Aided Design; the 3D model a user starts from.
- **OCCT** — Open CASCADE Technology, the geometry kernel this app builds on. Everything under `TopoDS_*` (`TopoDS_Shape`, `TopoDS_Face`, etc.) is an OCCT type.
- **BRep** — Boundary Representation, the way OCCT stores a solid: as its bounding faces/edges/vertices, not a mesh.
- **NURBS** — Non-Uniform Rational B-Splines, the math behind curved CAD surfaces (as opposed to flat, triangulated ones).
- **STEP / STL / OBJ / 3MF** — the file formats Import reads and Export writes. STEP preserves curved BRep surfaces; STL/OBJ/3MF are triangle meshes with no curve information (see [Import](#import)).
- **VBO** — Vertex Buffer Object, an OpenGL buffer holding geometry data on the GPU.
- **TBB** — Threading Building Blocks, the threading library used for intra-tool parallelism (see [Concurrency](#concurrency)).
- **HiDPI** — high pixel-density displays (e.g. Retina), where physical pixels and logical UI units diverge.

## Contents
- [Glossary](#glossary)
- [Architecture at a glance](#architecture-at-a-glance)
- [Data](#data)
  - [Part](#part)
  - [Issue](#issue)
  - [Operation](#operation)
  - [Settings](#settings)
  - [Invariants](#invariants)
- [Event Flow](#event-flow)
  - [Resizing](#resizing)
  - [Tool panel shapes](#tool-panel-shapes)
  - [Live preview](#live-preview)
  - [Worker-completion handling](#worker-completion-handling)
- [Architecture Layers](#architecture-layers)
  - [Scene](#scene)
  - [Logic](#logic)
  - [UI](#ui)
  - [Rendering](#rendering)
  - [Concurrency](#concurrency)
- [Tools](#tools)
  - [Import](#import)
  - [Analysis](#analysis)
  - [Structure](#structure)
  - [Calibrate](#calibrate)
- [Healing](#healing)
- [Background](#background)
  - [Rejected approaches](#rejected-approaches)
  - [Future & deferred](#future--deferred)
  - [Diagram maintenance](#diagram-maintenance)

---

## Architecture at a glance

Six pieces, wired by ownership:

```mermaid
flowchart LR
    subgraph Main["Main thread"]
    UI["UI"]
    Scene["Scene<br/>(owns the model)"]
    Rendering["Rendering"]
    end
    subgraph Worker["Worker thread pool"]
    Concurrency["Concurrency<br/>(worker queues)"]
    Logic["Logic<br/>(Tools)"]
    Healing["Healing"]
    end

    UI -- "submits user work" --> Scene
    Scene -- "submits tool job" --> Concurrency
    Concurrency -- "runs tool" --> Logic
    Logic -- "sends tool result" --> Healing
    Healing -- "returns healed result" --> Concurrency
    Concurrency -- "delivers healed result" --> Scene
    Concurrency -- "reports tool progress" --> UI
    Concurrency -- "shares in-progress preview" --> Rendering
    Scene -- "shares current model" --> Rendering
    Scene -- "shares current model" --> UI
    UI -- "triggers render" --> Rendering
```

You need something to actually act on the model — read a file in, run a diagnostic, carve out material. That computation is **[Logic](#logic)**, split into isolated **[Tools](#tools)**: Import, Analysis, Structure, Calibrate, and more once built.

A tool's result shouldn't just overwrite the model outright — something has to decide when it's actually safe to keep. That's **[Scene](#scene)**: the only layer that writes the model's canonical state — formally, a Part (see [Data](#data)) — and the one that hands off to Logic in the first place, for whichever tool the user picked via UI. Scene never chooses the tool itself; it only decides when that tool's result is safe to commit.

Not every result a tool computes is trustworthy, either — an OCCT (Open CASCADE Technology, the geometry kernel this app builds on) boolean can produce a shape that's subtly broken. **[Healing](#healing)** checks every result once, right before Scene commits it, so nothing invalid ever becomes the model's live state. Only tools that modify the model go through this — a read-only tool like Analysis never touches `current`, so there's nothing for Healing to check.

You also need to see the model and act on it — pick a face, read a flagged problem, tune a setting. **[UI](#ui)** is the only layer that listens for input, and the only one that decides whether a given input actually matters.

And you need to actually see the geometry. **[Rendering](#rendering)** draws whatever Scene and UI have already settled — it makes no decisions of its own.

None of this can block the main thread while it runs, so each tool's computation happens on its own worker queue, isolated from the others — that's **[Concurrency](#concurrency)**.

Finally, all of the above happens in response to something, not on a fixed tick: the main thread sleeps until a user action or a finished job wakes it, handles that one thing, and goes back to sleep. That's **[Event Flow](#event-flow)**.

**[Data](#data)** comes first below, defining the nouns (Part, Operation, Issue) that Event Flow's mechanism runs on; Event Flow follows right after.

---

## Data

[Event Flow](#event-flow) and everything after uses these names — Part, Operation, Issue, `current` — freely. Each item below is defined by what it owns and nothing more; cross-cutting rules spanning more than one of them are gathered in Invariants at the end instead of being repeated per-item.

- **Session** — the workspace needs to hold more than one model open at once, each independent of the others.
  - Owns one or more Parts, processed independently — no shared state between them.
- <a id="part"></a>**Part** — the thing the user wants to print. Owns only what defines it, so there's exactly one place that answers "who owns this":
  - `TopoDS_Shape current` — the live geometry
  - `vector<TopoDS_Shape> history` — undo stack, per-Part (cheap: OCCT shapes share underlying data)
  - `TopoDS_Shape`'s own hierarchy (Solid → Shell → Face → Wire → Edge → Vertex — a solid decomposed into its constituent faces, edges, and vertices) *is* the dependency graph for what's inside `current` — there's no separate reference system to maintain. Sub-shape relationships are traversed via `TopExp_Explorer` on demand, and sub-shapes are identified by `TopoDS_Face` value comparison, never raw pointers — pointers go stale the moment a shape is rebuilt.
  - **Why:** keeping Part this narrow is what makes a single, settled set of ownership rules possible — if Part also held Issues, the picking index, or any other derived state, "who owns this" would need re-deriving per consumer instead of being settled once (see Invariants).
  - ```mermaid
    flowchart LR
        Part --> current["current<br/>(TopoDS_Shape)"]
        Part --> history["history<br/>(undo stack)"]
        current --> Hierarchy["Solid → Shell → Face →<br/>Wire → Edge → Vertex"]
    ```
- <a id="issue"></a>**Issue** — a Part on its own can't say whether it's printable, so something needs to record where its specific problems are: Analysis's own record of a Part's problems, tied to real geometry (`TopoDS_Face` values) so it survives a rebuild.
  - Located on a Part by `TopoDS_Face` value, never a raw pointer: a single face for most detectors, a chain of faces for Layer Difference's vertical ribbon, or no face at all for Build Volume, which is a whole-Part problem rather than a local one.
  - Can carry a link to another tool — Build Volume links to Split — and clicking it opens that tool's panel directly, instead of (or alongside) highlighting geometry in the viewport.
  - Owned by Analysis, a Logic tool, not by Part (see Invariants for why); kept as a per-Part cache: a list of Issues, plus a pending flag and a stale flag.
    - On commit, Scene discards the cache's entries for that Part and sets **stale** — never keeps entries computed against a shape that no longer exists, since some would already reference `TopoDS_Face` values that no longer resolve to anything.
    - **Pending** drives the UI progress bar while an Analysis job runs.
    - **Stale** is cleared only once a completed Analysis run repopulates the cache against the new `current`. While stale is true, an empty cache means *not yet analyzed*, not *no problems found* — the UI must show these as distinct states, not collapse them (see Rendering).
  - ```mermaid
    stateDiagram-v2
        Stale --> Pending: Analysis run submitted
        Pending --> Fresh: run completes, cache repopulated
        Fresh --> Stale: Scene commits (cache discarded)
    ```
- <a id="operation"></a>**Operation** — a modification applied to an *existing* Part (e.g. Structure). Never mutates in place: pushes `current` onto `history` and replaces it with a new `TopoDS_Shape`, so undo is a pop.
  - On failure, reports one standard shape, not invented per-tool: a short, copyable **error code**; a short actionable **message** — what the user can do about it; an optional longer **why**, shown if there's room.
  - Import is not an Operation — it has no prior `current`/`history` to act on. It's a sibling concept that constructs a brand-new Part directly (`current` = the imported/healed shape, `history` empty, unless the source file itself carries a persisted original — see [Import](#import)).
  - **Why:** "always produces a new shape" is what makes undo just a pop off `history` instead of needing a separate undo-log or a deep-copy-before-mutate step — and it's what lets Healing check a result once, at commit, instead of every tool re-verifying state it might have silently corrupted in place.
  - ```mermaid
    flowchart LR
        subgraph Before
        C1["current: A"]
        H1["history: [...]"]
        end
        subgraph After
        C2["current: B"]
        H2["history: [..., A]"]
        end
        Before -- "Operation produces B" --> After
    ```
- <a id="settings"></a>**Settings** — persisted constants the user can tune (the tolerance constant, default tool parameters, etc.) — the only thing in the app that persists across restarts through a dedicated store.
  - Sessions/Parts still have no general project-file format, since there's no need for one yet at this app's current scope.
  - Surfaced in two places: a general settings surface for global values, each tool's own panel for tool-specific ones (`insetMm`, `buildVolumeXY`, etc.) instead, for better discoverability.
  - The one deliberate exception: Export's embedded-original mechanism (see Background → Future & deferred) persists a single `history` entry inside the exported file itself, not through Settings' store. Everything else about a Part — Issues, in-progress tool state, the rest of `history` — still doesn't survive a restart.
  - ```mermaid
    flowchart LR
        Global["General settings<br/>surface"] --> Store["Settings store<br/>(persisted)"]
        ToolPanel["Tool's own panel"] --> Store
        Export -. "embeds one history<br/>entry (bypasses store)" .-> File["Exported file"]
    ```
- <a id="invariants"></a>**Invariants** — cross-cutting rules spanning more than one item above, gathered here instead of being repeated per-item.
  - Derived state is owned by whoever computes it — not by Part, and not by Scene just because Scene triggered the work. Part owns only what defines it (`current`, `history`). This covers Issues (Analysis), the picking index (Rendering), and the live-preview slot (Concurrency). A lookup keyed by Part identity costs a negligible hash-map access regardless of which side of this rule it lives on, so this is purely an ownership rule, not a performance one.
  - A Part's Issues are always defined in Analysis's cache — possibly empty, never unknown, and never a mix of previous-shape and current-shape entries: Scene discards the cache and sets stale on commit, so an empty cache with stale true means *not yet analyzed*, never *previous shape's results*. The stale flag is written only by Scene (on commit) and only cleared by Analysis (on a completed run); Rendering/UI only read it.
  - An Operation always produces a new shape; mutation in place is forbidden.
  - The live-preview slot is mutex-protected: writer (worker, at phase boundaries) and reader (render thread, once per frame) each hold the mutex only for the duration of a handle copy.
  - Sub-shapes are referenced by value, never by raw pointer — a raw pointer would dangle the moment a shape is rebuilt (see Part).
  - Scene is the only layer that writes a Part's canonical state (`current`, `history`) and Settings — not the same claim as "Scene owns all state," per the rule above. UI is the only consumer that's purely read-only everywhere.
  - A Part's `current` is always valid — validated once at commit (see Healing), never re-checked downstream. Anything reading `current` can trust it unconditionally.

Wired by ownership:

```mermaid
flowchart LR
    Session["Session"] -- "owns" --> Part["Part<br/>(current, history)"]
    Operation["Operation"] -- "replaces current on" --> Part
    Part -. "located by<br/>TopoDS_Face" .-> Issue["Issue<br/>(owned by Analysis)"]
```

---

## Event Flow

Most interactive apps run a continuous loop — read input, update, render, repeat, dozens of times a second, whether or not anything actually changed. This app instead sleeps the main thread until something worth reacting to happens, handles it, and goes back to sleep. Two distinct things can wake it:

1. **User input** — the user did something (moved the camera, clicked a button, entered a value). UI is the only layer that listens for these events, and it's also the one that decides whether a given event is actually relevant to anything — one that wouldn't change any state does nothing further.
2. **A worker job finishing** — a background tool computation (an Import, an Analysis run, a Structure operation) completes. Scene — the layer that owns a Part's canonical state, previewed in [Architecture at a glance](#architecture-at-a-glance) and covered fully in [Architecture Layers → Scene](#scene) — handles this wake directly, with no relevance check first, because Scene submitted that job itself and already knows the result matters.

Both paths converge on the same question before anything is drawn: has anything actually changed since the last frame? If not, the thread sleeps again without rendering. If so, it renders once — synced to the display's refresh via **vsync**, so it never draws more than once per refresh, avoiding tearing and wasted draws — then sleeps again. At a glance:

```mermaid
flowchart LR
    Sleep["Sleep until vsync<br/>or a wake-up"]
    Wake{"What woke it?"}
    Input["Handle the user-input event<br/>(UI-local, settings, or a tool flow)"]
    Worker["Handle the completed worker job<br/>(Operation, Analysis, or preview)"]
    Converge{"State changed?"}
    Render["Render"]
    Sleep --> Wake
    Wake -- "user input" --> Input
    Wake -- "worker completion" --> Worker
    Input --> Converge
    Worker --> Converge
    Converge -- "no" --> Sleep
    Converge -- "yes" --> Render
    Render --> Sleep
```

Two wake sources, each handled, then one shared convergence check before render. Everything below is what "handle the user-input event" and "handle the completed worker job" actually dispatch to — the same loop, every branch:

*This diagram (and five others in this doc) also renders as a static image, [architecture-event-flow.png](architecture-event-flow.png), for viewers without live Mermaid — see [Diagram maintenance](#diagram-maintenance) in Background for what that image is and how to regenerate it after an edit.*

```mermaid
flowchart TD
    Sleep(["Sleep until vsync or a wake-up"])
    Wake{"What woke it?"}
    Sleep --> Wake

    %% ---------- user input event ----------
    Wake -- "user input event" --> ResizeQ{"Resize event?"}

    ResizeQ -- "yes (exception path)" --> ResizeSync["Sync watcher updates viewport,<br/>camera aspect-ratio, ortho clip bounds"]
    ResizeSync --> ResizeAnchor["Re-resolve anchors: priority-ranked<br/>collapse, panel layout reflow"]
    ResizeAnchor --> ResizeRender["Push camera matrices to renderer(s);<br/>render now"]
    ResizeRender -. "loop straight back — skips<br/>relevance check & convergence" .-> Sleep

    ResizeQ -- "no" --> CheckRelevance["UI checks relevance"]
    CheckRelevance --> RelevantQ{"Relevant?"}
    RelevantQ -. "no" .-> Sleep
    RelevantQ -- "yes" --> LocalOrDomain{"UI-local, settings,<br/>or domain (tool flow)?"}

    LocalOrDomain -- "UI-local" --> UILocal["UI updates itself:<br/>camera, panel layout,<br/>in-progress selection,<br/>hover-to-render request"]
    UILocal --> Converge

    LocalOrDomain -- "settings (direct — nothing<br/>to select, no preview)" --> SettingsCommit["Scene updates + persists<br/>the setting"]
    SettingsCommit --> Converge

    LocalOrDomain -- "domain (tool flow)" --> PrereqCheck["Check Prerequisites + Selections"]
    PrereqCheck --> SatisfiedQ{"Satisfied?"}
    SatisfiedQ -- "no" --> Missing["Tool panel shows what's<br/>missing (required vs optional)"]
    Missing --> Converge
    SatisfiedQ -- "yes" --> ToolShape{"Tool shape?"}

    ToolShape -- "modifying (Import,<br/>Structure, Cut)" --> SubmitPreview["Submit/update preview job to<br/>Concurrency, debounced; shows last<br/>cached preview meanwhile"]
    SubmitPreview --> AcceptCancelQ{"Accept or Cancel?"}
    AcceptCancelQ -- "cancel" --> Discard["Discard preview"]
    Discard --> Converge
    AcceptCancelQ -- "accept" --> SubmitOp["Scene submits Operation job to<br/>Concurrency, runs async; shows progress<br/>bar via checkpoint/phase callbacks"]
    SubmitOp --> Converge

    ToolShape -- "calculating (Calibrate)" --> ComputeSync["Compute synchronously —<br/>cheap geometry math,<br/>no worker thread needed"]
    ComputeSync --> ShowResults["Show copyable results"]
    ShowResults --> Converge

    ToolShape -- "diagnostic (Analysis)" --> SubmitAnalysisJob["Submit Analysis job to Concurrency;<br/>shows progress bar while running"]
    SubmitAnalysisJob --> Converge

    ToolShape -- "hybrid (Orient)" --> SubmitSweep["Submit orientation-sweep job;<br/>reuses Analysis's detectors across<br/>candidate bed orientations"]
    SubmitSweep --> ShowRecommendation["Show recommendation preview"]
    ShowRecommendation --> AcceptOrientQ{"Accept?"}
    AcceptOrientQ -- "no" --> Converge
    AcceptOrientQ -- "yes" --> UpdateBuildDir["Update UI-local build-direction<br/>state — no Operation"]
    UpdateBuildDir --> Converge

    %% ---------- worker-completion wake ----------
    Wake -- "worker-completion wake" --> SceneDirect["Scene handles directly —<br/>no UI relevance check"]
    SceneDirect --> WhichJob{"Which job completed?"}

    WhichJob -- "Operation / Import" --> SucceededQ{"Succeeded &<br/>valid?"}
    SucceededQ -- "no" --> ShowError["Progress bar shows error:<br/>copyable code + actionable<br/>message + optional why"]
    ShowError --> Converge
    SucceededQ -- "yes" --> CommitShape["Scene commits new current,<br/>pushes old shape to history"]
    CommitShape --> ImportQ{"Was this Import?"}
    ImportQ -- "no" --> Converge
    ImportQ -- "yes" --> OpenAnalysis["Open Analysis panel,<br/>submit Analysis run for the new Part"]
    OpenAnalysis --> Converge

    WhichJob -- "Analysis" --> AnalysisCommit["Analysis commits its Issues<br/>cache for that Part, clears<br/>pending flag"]
    AnalysisCommit --> Converge

    WhichJob -- "preview-compute" --> UpdatePreviewSlot["Update the live-preview /<br/>latest-intermediate-result slot"]
    UpdatePreviewSlot --> Converge

    %% ---------- convergence ----------
    Converge{"UI or Scene state<br/>changed since last frame?"}
    Converge -. "no" .-> Sleep
    Converge -- "yes" --> Render["Render: read Scene + UI camera +<br/>live-preview slot; triangulate-if-<br/>missing, z-fighting tie-break, draw;<br/>bounded time budget per frame"]
    Render --> Sleep
```

This lets UI handle its own presentation (camera, panel layout, hover) without routing every nudge through Scene, while still funneling every domain mutation through Scene as the one writer — and keeps the main thread idle whenever nothing has actually changed.

### Resizing
A window resize is a deliberate exception to the wake-and-check path above: instead of going through UI's relevance check, an OS-level event watcher intercepts `SDL_EVENT_WINDOW_RESIZED` synchronously and updates state — including rendering — immediately, rather than waiting for the next reactive wake.

This is justified, not a shortcut — deferring to the next polled frame would show a stretched or stale frame for one visible frame during an interactive resize drag, a real artifact, not just a layering nicety being skipped for convenience.

- The viewport update uses physical pixels for HiDPI (high pixel-density displays, e.g. Retina) correctness, while logical size still drives camera/UI math.
- The viewport itself stays a fixed size regardless of panel layout — panels overlay it rather than resizing it, since visual stability of the 3D view matters more than reclaiming screen space when a panel collapses.

### Tool panel shapes
Every tool panel needs its required inputs selected before it can do anything — the diagram above calls this the Prerequisites + Selections gate: if something required is still missing, the panel just shows what's needed (required vs. optional) and waits. Once satisfied, what happens next depends on the tool's *shape* — four patterns, each suited to a different kind of tool:

- **Write** — modifies a Part's `current` (its live geometry, see [Data → Part](#part)):
  - **Modifying** (Structure, future Cut, and Import — which creates a Part instead of modifying one, but follows the same shape) — preview the result, then Cancel or Accept; only Accept actually commits.
    - Accept is disabled while another Operation is already in-flight for that Part. The preview was computed against `current` as it stood at preview time; if a different Operation commits first, that preview is now stale. Accepting it anyway would run the submitted job against the wrong shape and silently discard the in-flight result.
- **Read-only** — `current` stays untouched:
  - **Diagnostic** (Analysis) — submits a job to its worker queue, shows a progress bar, and commits its own Issues cache once done (see [Data → Issue](#issue)).
  - **Calculating** (Calibrate) — ends in copyable results instead of an accept step; there's nothing to commit.
  - **Hybrid** (Orient) — calculates a recommendation, but its Accept step only changes UI-local build-direction state, not an Operation — nothing is committed to the Part.

A settings change isn't part of this classification at all — it skips the gate entirely. It does mutate domain state (so it isn't purely UI-local), but there's nothing to select and no preview to show, so the Prerequisites + Selections gate doesn't apply either. It's its own direct path: Scene updates and persists the value immediately.

### Live preview
While a long-running Operation (see [Data → Operation](#operation)) is still computing, what should the viewport show in the meantime? Two different answers were considered.

**Phase-level snapshots** — showing the shape as of the last completed checkpoint while the next phase computes — work well: a checkpointed operation (one broken into stages with a real, valid shape at each stage boundary — Structure's carve is built this way) produces a genuine intermediate `TopoDS_Shape` at each phase boundary. OCCT shapes are cheap to copy (a handle increment, not a deep copy — see [Concurrency → Intra-tool parallelism](#concurrency)), so handing one back to the main thread mid-operation costs almost nothing.

- This needs one small addition to Concurrency: a future (the usual way a worker hands back a result) only resolves once, so delivering several intermediate shapes over one operation's lifetime needs a separate "latest intermediate result" slot the worker updates between phases instead. Concurrency owns that slot (see Invariants); Rendering only reads it.
- The slot is protected by a mutex — the worker holds it briefly to write a new shape handle at each phase boundary; the render thread holds it briefly to copy the handle once per frame. Contention is negligible since writes happen only at phase boundaries.

**Continuous frame-by-frame animation** of a single OCCT call in progress isn't achievable on any graphics API: an OCCT boolean or fillet call is an atomic black box with no mid-call hook — there's no valid shape to expose until the call returns, because the shape doesn't exist in any partial form before then. This isn't a rendering limitation waiting to be solved; the data to render simply isn't there yet.

- This isn't a reason to consider Vulkan either: the actual bottleneck here was never render-thread parallelism (this app draws a handful of Parts with cached triangulation — nowhere near the draw-call volume where Vulkan's multithreaded command recording pays off), and switching graphics APIs wouldn't expose OCCT's internal call state regardless.

### Worker-completion handling
When an Operation or Import job finishes, Scene checks **success and validity** before committing anything. Failure — whether the algorithm errored outright, or it ran clean but produced an invalid shape that Healing couldn't fix (see [Healing](#healing)) — shows the standardized error (code, message, optional why) in the same progress bar, rather than silently discarding the result or leaving stale state behind.

On success, Scene commits the new shape as the Part's `current`.

- If this was an Import, Scene also opens the Analysis panel and submits an Analysis run for the new Part immediately (see [Import](#import)) — the one case where Analysis runs without the user separately triggering it.
- Otherwise, Analysis exposes its own Issues cache directly rather than handing anything back to Scene (see [Architecture Layers → Logic](#logic) for why Analysis's result path differs from an Operation's), and runs only when the user triggers it.
- A live-preview-slot update from an in-progress preview job follows this same no-relevance-check path, since it's also Scene-initiated work completing, not user input.

---

## Architecture Layers

Event Flow described *when* things happen; this section describes *who* does each part of the work. The app splits into five layers, each with one clear job and one clear boundary on what it's allowed to touch — Scene orchestrates, Logic computes, UI listens and displays, Rendering draws, and Concurrency runs the actual worker threads underneath all of them.

### Scene
Owns Session and the operation queue. The only layer that writes to a Part's canonical state (`current`/`history`) — Part itself holds that data (see [Data → Part](#part)), Scene is the sole gatekeeper for changing it.

**Why:** orchestrates, doesn't compute — submits Logic tools' work to Concurrency's worker thread and commits what comes back onto a Part, without taking ownership of what a tool produces along the way (see Logic). Keeps "deciding what happens to a Part" separate from "computing it."

Also owns the **operation queue** — pending/in-progress Logic work on the worker thread. UI submits to it; rendering just reads a busy flag.

Uses the **tolerance constant** (owned/persisted by Settings, default grounded in real manufacturing/measurement resolution — ~0.001mm, finer than a printer can reproduce or a caliper can read) for two roles, one number feeding both, not picked independently per call site:
- Our own geometric comparisons — degenerate-loop checks, eligibility thresholds.
- The tolerance argument passed into OCCT APIs that accept one — sewing, face-fixing.

### Logic
Owns each tool's own domain computation and any tool-specific derived cache — Analysis's Issues, Structure's carve algorithm, Calibrate's correction formulas, Import's facet/sew/unify pipeline. This is where `src/logic/` (Analysis, Structure, Calibrate, Import, GeometryOps) actually runs.

**Why a separate layer:** distinct from Scene, which orchestrates but doesn't compute. Scene triggers Logic running and invokes it via Concurrency's worker thread, but that doesn't make them the same layer — what happens to a tool's result still depends on the tool:
- **Operations / Import** — hand a new shape back for Scene to commit.
- **Analysis** — exposes its Issues cache directly; nothing handed back to Scene.
- **Calibrate** — results are just read by UI; nothing committed at all.

A tool's result isn't "successful" until it's valid — see [Healing](#healing), the shared mechanism every tool's commit path goes through.

### UI
The only layer that listens for input events, and the only one that decides whether an event is relevant (see Event Flow). Drives imports, submits operations to Scene's queue, displays Issues and Part state. Never touches geometry directly. Owns the **camera** — it's what the user directly manipulates, so it's UI-local state, not a rendering decision, even though it feeds the render.

Resizing is a deliberate exception to the normal event flow — see Event Flow → Resizing.

**Repositioning.** Panels and HUD elements don't use fixed pixel coordinates; each resolves its position from anchors (screen edges/corners) plus a content-box model (margin + padding + content, computed bottom-up). Resizing the window re-resolves every panel's box top-down, so panels reflow instead of needing per-resolution layout logic.

- Each parent element defines a local coordinate system; children position relative to their parent's, recursively — a standard scene-graph layout, not per-element absolute coordinates.
- Margin and padding are reserved space no child content ever occupies — enforced by how each child's position is computed from its parent's content-box, not by clipping content that overflows into that space afterward.
- The **viewport itself stays a fixed size** regardless of this — panels overlay it rather than resizing it, since visual stability of the 3D view matters more than reclaiming screen space when a panel collapses.
- Only an actual window resize retriggers the camera aspect-ratio/projection update, never a panel layout change alone.

**Sizing.** Under space pressure, elements collapse in priority order rather than uniformly shrinking or clipping:
- Each element has a priority rank; lower-priority elements collapse first.
- Each collapse state has its own minimum size; the overall minimum is whatever the deepest non-collapsed element needs.
- If available space drops below the current minimum, collapse the lowest-priority element next, and recompute.
- What "collapse" means is element-specific (hide entirely, icon-only, re-wrap) — not one uniform behavior.

Units are computed from resolution and display scale (`SDL_GetWindowDisplayScale`), not raw pixels, so an element's *visual* size stays consistent across screen densities.

Tool panels split into modifying/calculating/hybrid shapes, with settings changes taking their own direct path — see Event Flow → Tool panel shapes. Live preview during a running operation is phase-level only, not continuous — see Event Flow → Live preview.

**Hover-to-render.** Anywhere the UI displays a reference to a sub-shape (an Issue's location, a selected face), hovering it highlights that feature in the viewport. This is the same picking index (`TopTools_IndexedMapOfShape`) Rendering owns for click-to-pick, just driven in reverse — no new infrastructure needed.

**Multiple open panels.** A tool can open another tool directly — Import opens Analysis on completion, a Build Volume Issue opens Split (see Tools → Analysis) — so more than one tool panel can be open at once. Rather than a second, parallel layout system for stacking panels in a column, this reuses the collapse mechanism Sizing already has: opening a tool this way focuses it (full space) and collapses whichever panel had focus before, to that panel's own defined collapse state — a breadcrumb trail back through the chain (Import → Analysis → Split), not a fixed two-panel limit.

Losing focus this way is not the same as being dismissed: a panel collapsed only because a chain-link opened another tool is still open, just visually shrunk, so any in-flight work it's tracking keeps running undisturbed (see Concurrency's cancellation trigger).

### Rendering
Purely reactive: no logic, no decisions, just reflects state Scene/UI already settled. Two separate gates, both required:

- Vsync gates *when* a draw can happen — never more than once per refresh, in sync with the display, avoiding tearing and wasted draws between refreshes.
- The state-changed check (Event Flow → convergence) gates *whether* it actually does — no redraw if nothing changed since the last frame, no per-frame recompute.

Owns triangulation — reads cached triangulation from OCCT's faces, running `BRepMesh_IncrementalMesh` lazily on first draw if missing. Owns the **picking index** (`TopTools_IndexedMapOfShape` per Part) for mapping viewport clicks back to sub-shapes, rebuilt against `current` after every commit.

Between a commit and the user re-running Analysis, the Issues cache is empty and marked stale (see [Data → Issue](#issue)) rather than holding entries computed against the previous shape — those entries could reference face values the new shape no longer has, and there's no reliable way to tell which surviving ones are still meaningful. An empty stale cache and a genuinely clean result both show zero Issues, so the UI must surface the distinction (*not yet analyzed* vs. *verified clean*) rather than let one be mistaken for the other.

**Buffer updates.** GPU geometry lives in two global VBOs (Vertex Buffer Objects — GPU-resident geometry buffers; one for triangle meshes, one for wireframe lines), each mapped once at startup with `GL_MAP_PERSISTENT_BIT | GL_MAP_WRITE_BIT | GL_MAP_COHERENT_BIT` and double-buffered (two VBOs, or one split in half). Each Part occupies a tracked chunk (vertex and index offsets) within them.

Packing splits into two phases, the same isolation principle Intra-tool parallelism uses for read-only OCCT work: do the slow part on a private copy nobody else can see, and only touch shared state for the fast, bounded part.

- **Pack** — the background thread builds the new chunk's vertex/index data into its own private staging buffer, plain CPU memory the GPU and render thread never touch. No synchronization needed here at all, for the same reason a read-only OCCT job is safe on a copied handle: nothing else can observe a private copy mid-write.
- **Publish** — once packing is complete, the background thread copies the staged data into the actual persistent-mapped VBO chunk in one bounded bulk copy. This is the only step that needs the fence: `glFenceSync` after draw calls marks when the GPU started reading a half; the main thread checks the fence and signals the background thread (via a condition variable) when that half is safe to overwrite. Because publish is a straight memory copy — no packing logic, no variable-cost work — its duration scales with data size alone, so it isn't the part of the pipeline that risks running long.
- Dirty-tracking: a per-Part dirty set (`geometryDirtySolids`) drives targeted partial updates (only the affected chunk is rewritten); a `geometryDirtyAll` flag triggers a full repack and is set only when the chunk layout changes (a Part added, removed, or its mesh size crosses a boundary).

**Z-fighting.** The actual symptom is chaotic flicker: when two faces are this close in depth, which one wins the depth test can resolve inconsistently per-pixel or per-frame as the camera moves, reading as visual noise rather than a clean edge — not a clean, consistent "one face disappears." This app hits the precondition often: small faces nested inside or coplanar with a larger host face (a classified sub-face, a Structure preview overlay).

The fix: when two faces are within the tolerance constant's depth range of each other (a genuine tie, not just nearby in depth), bias whichever has the smaller projected screen area toward the camera — deterministic, so the flicker has a fixed winner instead of an unstable one. That also keeps the smaller face reliably visible and pickable as a side effect, rather than it winning or losing at random. This only resolves real ties — it doesn't reorder normal, legitimately depth-separated geometry, since there's no tie to break there.

**Fog for depth cueing** — a separate concern from z-fighting: a perceptual aid helping the user visually judge which face is in front, not a fix for a rendering artifact.

**Reference grids.** The viewport draws a ground-plane grid at `buildVolumeXY` spacing (see [Settings](#settings)), giving a visual sense of scale against the printer's bed. A tolerance grid reuses the same primitive at the tolerance constant's spacing instead, in a distinct color — making that otherwise abstract number visually graspable next to real geometry.

The tolerance grid is culled unless it's actually resolvable: below roughly one line per two screen pixels (the Nyquist limit for a line pattern), it reads as aliasing noise, not a scale reference. It only draws once the camera is zoomed in close enough to clear that threshold — in practice, far past the zoom level where the bed grid itself is still legible — so it needs no separate logic to track what the user's looking at; the visibility check alone puts it wherever they've zoomed in.

### Concurrency
Concurrency is what actually runs Logic's computations — every "submit a job" mentioned above ends up here. Logic work runs on worker threads, isolated **per tool** — not one shared pool. The main thread owns UI and rendering and never blocks.

Each tool owns its own queue and worker(s), not only because OCCT can hang (rare — effectively a bug on OCCT's own side when it happens), but because a tool's own algorithm can too — a bug or pathological case in one tool's code must not be able to starve another tool's queue. Structure's carve runs on its own isolated runner precisely for this reason: a stuck case there would otherwise starve Import/Analysis's shared queue, a real precedent, not a hypothetical one. This generalizes that pattern to every tool instead of leaving it a one-off special case.

**Algorithm, per tool's queue:**

1. Scene submits a unit of work to that tool's task queue (`fn` receives a cancellation token it can check cooperatively during long calls). Submission is non-blocking — it enqueues the job and immediately returns a handle wrapping a future; the caller does not wait. Queue behaviour differs by job type:
   - **Operation jobs queue behind** any in-flight job for that Part — safe, since Operations are discrete and ordered.
   - **Preview jobs cancel-supersede** — a new preview submission flips the in-flight preview's cancellation token and replaces it, because the new input always makes the old preview result irrelevant; queuing stale previews would leave the user waiting through results they'll never use.
2. That tool's worker(s) pull queued jobs FIFO and run them off the main thread.
3. On completion, the job wakes the main thread's blocked event wait — so the main thread discovers the result without polling every frame.
4. The main thread polls the handle (non-blocking by default) to check if the result is ready, and if so commits it — e.g. Scene swaps in the new shape (once validated, see [Healing](#healing)).
5. Cancellation is cooperative only: requesting cancellation flips a flag; the running task must check it itself to actually stop early — there's no preemption.
   - On the UI side, the user explicitly collapsing or dismissing a tool panel is the natural cancel trigger. A panel that collapses only because focus moved to a newly-opened tool (see UI → Multiple open panels) is not dismissed — it's still open, just visually shrunk — so it doesn't cancel anything.
   - Cancellation isn't guaranteed instant either way, so the UI must reflect that honestly: show the cancellation as pending until the worker actually stops, rather than assuming immediate success.
6. Teardown never blocks the main thread. Destroying a handle whose result isn't ready yet gets a bounded grace wait (about one frame); any wait beyond that is deferred to a detached thread instead. This matters because some calls can hang indefinitely, and blocking on cleanup would freeze the UI.
   - True OCCT hangs are rare — a normal slow case exits at its next cooperative cancellation check — and per-tool isolation already limits the blast radius to one queue. Not worth process isolation or `pthread_cancel` unless this becomes a real user-reported issue.

**Intra-tool parallelism:**

Not all OCCT operations have the same thread-safety constraints. The distinction that determines whether an operation can be parallelized:

- **Modifying** (boolean cuts, fillets, sewing, anything producing a new shape) — serialized within the tool's worker. These write new `TopoDS_Shape` objects and must not run concurrently.
- **Read-only** (slicing, shape traversal, cross-section computation) — safe to parallelize. They only read the input shape; `current` is never touched.

The safety mechanism for read-only parallelism: copy the `TopoDS_Shape` handle once per worker thread before dispatching, on a single thread. `TopoDS_Shape` copies are O(1) — a handle increment, no geometry duplication. After dispatch, each thread holds its own handle with no concurrent reference-count manipulation.

**TBB** (Threading Building Blocks, `tbb::task_group`/`tbb::parallel_for`) is the threading primitive for intra-tool parallelism — not `std::async`. Analysis can be triggered repeatedly across a session, so raw thread creation per run compounds; TBB's thread pool reuses threads across runs. Use `tbb::task_group` for fan-outs (Analysis's detectors), `tbb::parallel_for` for range parallelism (slice layer ranges). Not `tbb::flow::graph` — the DAGs involved are at most two levels deep and don't warrant it.

Analysis is the primary beneficiary: the slice step runs sequentially on the OCCT thread (or in parallel across layer ranges with pre-copied handles), then hands off raw geometry (plain 2D point arrays — no OCCT) to four detector threads via `tbb::task_group`. Structure's pipeline is mostly modifying OCCT operations and gains little from this model currently.

---

## Tools

Collectively, these are the **Logic** layer (see Architecture Layers → Logic) — Scene orchestrates and commits, Logic computes. Open items and future tools are in [Background](#background).

### Import
Not an Operation — it has no prior Part to act on. Constructs a brand-new Part directly. Supports STL, OBJ, STEP, and 3MF.

**Algorithm:**

1. **STEP** imports as native BRep (Boundary Representation — a solid stored as its bounding faces/edges, not a mesh) via `STEPControl_Reader` — true curved surfaces, no conversion needed.
2. **STL/OBJ/3MF** go through facet → sew → unify:
   - One planar face per triangle (`BRepBuilderAPI_MakeFace`).
   - Sewn into a shell (`BRepBuilderAPI_Sewing`).
   - `ShapeUpgrade_UnifySameDomain` merges adjacent *coplanar* triangles into larger flat faces.
3. That only recovers flat faces — a tessellated cylinder stays a faceted prism. Reconstructing curved faces (fitting NURBS — Non-Uniform Rational B-Splines, the math behind smooth curved surfaces — curves/surfaces to the tessellation) would recover that precision, but no surface-fitting step exists yet — an unimplemented future enhancement, not a current capability (see Background → Future & deferred for why it's still open).

**Hand-off to Analysis.** A freshly imported Part has no prior Issues cache, so on a successful commit Scene opens the Analysis panel and immediately submits a run for the new Part — a deliberate exception to Analysis's user-triggered rule (see [Analysis](#analysis)). This is safe specifically because Analysis is read-only and diagnostic: there's no Accept gate to skip, unlike a Modifying tool. It doesn't generalize to Structure or Split, which still require an explicit Accept no matter how their panel was opened.

**Reading back an exported original.** A `tempered.stp` file (written by Export, see Background → Future & deferred) has an embedded pre-Operation original that seeds the new Part's `history` with that one entry instead of starting empty — Undo reaches back to the true original even after a save → close → reopen round-trip. Everything else about Import proceeds as normal; the embedded data is just extra STEP-comment content any other STEP reader already ignores.

### Analysis
Diagnostic. Runs when the user triggers it — except immediately after Import, which triggers a run automatically as part of handing the new Part off for review (see [Import](#import)) — computing its per-Part Issues cache (see [Data → Issue](#issue)). Full derivation and uncalibrated constants: [analysis-redesign.md](todo/analysis-redesign.md).

**Why:** each detector ties to a specific physical cause in the FDM (Fused Deposition Modeling) process — the layer-by-layer melted-filament printing this app targets. New detectors should be justified the same way, not bolted on ad hoc:

- **Overhang** (extrusion) — nozzle pressure/momentum pushes unsupported material sideways. Not gravity-driven — happens upside-down too.
- **Not Enough Space** (reheat + cornering motion lag) — two independent, compounding mechanisms:
  - *Reheat* (loop-level): a small loop has a short perimeter, so the nozzle revisits it before it cools. Severity tracks loop size/shape, not shrinkage — no global factor fixes it, so it must be flagged geometrically.
  - *Cornering motion lag* (corner-level): the nozzle decelerates into a sharp corner; extrusion lags the velocity change (what pressure/linear advance imperfectly compensates), over-extruding locally, independent of loop size.
- **Instability** (motion) — a tall thin section topples under disturbance, like Euler buckling: a column yields along its weakest axis, not its average width.
- **Layer Difference** (shrinkage/warping) — differential cooling shrinks the part. Uniform across the whole part (unlike reheat), so Calibrate's single scale factor corrects it — Analysis just flags where the area-delta is large enough to signal risk.
- **Build Volume** (motion system range) — the print head's motion system has a fixed travel range set by the bed's physical X/Y extent, so geometry wider than that footprint is mechanically unreachable — a hard travel limit, not a print-quality risk like the other four.

**Algorithm, per Part:** one shared slicing step feeds four *independent* detectors — not a sequence; 2-5 below don't depend on each other, only on 1. A fifth, Build Volume, skips slicing entirely and runs directly off the Part's shape:

```mermaid
flowchart TD
    Slice["Slice into per-layer<br/>cross-sections (once)"]

    subgraph OH["Overhang (extrusion)"]
    direction TB
    OHDiff["Diff: currentLayer −<br/>previousLayer"]
    OHScore["Score by reach<br/>distance, not area"]
    OHDiff --> OHScore
    end

    subgraph LD["Layer Difference (shrinkage/warping)"]
    direction TB
    LDDiff["Diff: currentLayer −<br/>previousLayer (same primitive)"]
    LDScore["Score by raw<br/>area-delta magnitude"]
    LDMap["Chain across layers →<br/>vertical ribbon (BridgeSurface)"]
    LDDiff --> LDScore --> LDMap
    end

    subgraph NES["Not Enough Space (reheat + cornering lag)"]
    direction TB
    NESBase["Loop-level baseline:<br/>loopPerimeter / nominalPrintSpeed<br/>vs cooling threshold"]
    NESMult["× corner-level multiplier:<br/>turning angle, avgEdgeLength/edgeLength"]
    NESMap["Tag flagged edge to<br/>originating TopoDS_Face"]
    NESBase --> NESMult --> NESMap
    end

    subgraph INS["Instability (motion)"]
    direction TB
    INSCompute["minWidth via convex hull<br/>+ rotating calipers"]
    INSScore["Feed into run-detection +<br/>height/width-ratio logic"]
    INSCompute --> INSScore
    end

    subgraph BV["Build Volume (motion range)"]
    direction TB
    BVCompute["Bounding box of<br/>current (no slicing)"]
    BVScore["Compare X/Y extent to<br/>buildVolumeXY setting"]
    BVCompute --> BVScore
    end

    Issues[("Issues cache")]

    Slice --> OHDiff
    Slice --> LDDiff
    Slice --> NESBase
    Slice --> INSCompute
    OHScore --> Issues
    LDMap --> Issues
    NESMap --> Issues
    INSScore --> Issues
    BVScore --> Issues
```

*Each detector's internal compute → score (→ map) steps are right there in the box — nothing is split into a second diagram.*

1. Slice into per-layer cross-sections once, shared by all four detectors below.
2. **Overhang**:
   - **Diff** — boolean diff `currentLayer − previousLayer` gives the unsupported region directly.
   - **Scoring** — threshold on **reach distance** (how far the diff region extends past the supported edge), not area: a long thin sliver that cantilevers far is the failure case, not a short wide strip with more area but little reach. Area stays as a secondary signal.
3. **Layer Difference**:
   - **Diff** — the same diff primitive as Overhang (`currentLayer − previousLayer`).
   - **Scoring** — thresholded on raw area-delta magnitude instead of reach: warping is about how much cross-sectional area shifted, not which direction it's unsupported in.
   - **Mapping** — not a single-face defect: flagged regions chain across consecutive layers into a vertical ribbon (`BridgeSurface`), since warping runs through the part's height, not one face.
4. **Not Enough Space** — per loop: `risk = loopReheatRisk(loopPerimeter, nominalPrintSpeed) * (1 + cornerDwellBonus)`.
   - **Loop-level baseline** (`loopReheatRisk`) — estimated revisit time (`loopPerimeter / nominalPrintSpeed`) compared against a cooling threshold.
   - **Corner-level multiplier** (`cornerDwellBonus`) — scores each corner's turning angle, and each edge's length *relative to the loop's own average* (`avgEdgeLength / edgeLength` — the short edge must be the denominator, or the ratio backwards-favors long edges).
   - **Face mapping** — each corner is tagged with its originating `TopoDS_Face` at slice time, so the flag becomes a clip region on that face, not the whole loop.
5. **Instability**, per loop:
   - **Computation** — `minWidth` via convex hull + rotating calipers (a convex polygon's minimum width is always perpendicular to one of its hull edges, so only as many directions as hull edges need checking).
   - **Scoring** — feed `minWidth` into the run-detection + height/width-ratio logic, flagging a run of layers whose minimum width is small relative to how tall the run is.
6. **Build Volume** — compute the Part's axis-aligned bounding box directly from `current` (no slicing needed) and compare its X/Y extent against `buildVolumeXY` (Analysis's own tool-specific setting; see [Settings](#settings)). Z isn't checked — gantry travel comfortably exceeds any practical part height. An oversized Part gets a single, Part-wide Issue (no `TopoDS_Face` — the whole geometry doesn't fit, not one region of it) that links directly to the **Split** tool instead of highlighting a location in the viewport.

### Structure
Optimization, the opposite direction from Analysis: removes material/weight/print-time from an over-built region without weakening it. An **Operation** — produces a new shape via OCCT carve/fillet/offset/boolean primitives (`GeometryOps`).

**Mechanism:** the tool auto-selects every eligible face (near-horizontal, large enough span) — the user excludes faces, rather than picking them in. Per remaining face: inset the boundary by `insetMm` (holes get an outset, untouched), carve away the interior (vertical prism subtraction), then brace the void with diagonal struts anchored to the remaining wall band.

The carve is always **vertical** — never angled — so it never introduces an overhang. A carve that left an unsupported angled wall would just trade one defect for another, so this constraint is load-bearing, not a shortcut.

**Why:**

- **Bending → axial conversion.** A solid panel resists load mainly via bending stiffness (scales with thickness³) — material in the middle of a thick slab does little work relative to material at the boundary. Diagonal struts redirect that load into axial tension/compression instead, which carries load per unit mass far more efficiently — the same principle behind trusses and I-beams.
- **Corners outlast walls.** Not because the filament there is stronger — it's the same material everywhere. A flat wall span is like a simply-supported plate: unsupported mid-span, so it deflects most there. A corner is braced on two sides, like an L-bracket vs. a flat sheet — stiffer by geometry, not material. FDM's flexible filament and weak inter-layer bonding make this more *visible*, but the cause is leverage, not "weak walls." This is also why inset distance, not material choice, is the real safety lever.
- **Fillets at strut/wall junctions cut stress concentration.** A sharp internal corner is a textbook stress riser — a real, quantifiable factor (2-3x vs. a filleted one). Real mechanical work, not cosmetic.

**Algorithm, per eligible face:**

1. Project the face onto the horizontal world-XY cutting plane (slanted faces project tilted, via an affine shear, so a circular hole still carves a cylinder wall instead of a polygon prism).
2. Offset by `insetMm`: outer boundary inward, hole loops outward (untouched).
3. Place an anchor at each inset-edge midpoint, carrying that edge's inward normal.
4. Generate strut candidates between anchor pairs, dropping any that cross a ring boundary (outer or hole) or whose midpoint falls outside the outer ring or inside a hole's void.
5. Clip each surviving candidate against its own walls' real geometry (an OCCT boolean, not a 2D approximation). An end that doesn't land squarely on the wall it's connecting to **tapers** narrower than `insetMm/2` instead of being rejected outright — a tapered strut is still buildable, and may be the only option at that anchor. The taper starts as late as possible: find where the strut's centerline crosses the wall's real boundary (the point a zero-width strut would just reach), and only begin narrowing from there. That way the strut holds full width for as much of its length as the wall geometry allows, rather than tapering gradually over its whole span.
6. Rank valid pairs on four terms:
   - Combined length.
   - Bisector alignment with the edge's inward normal.
   - **Closeness to 45° from the walls each strut touches** — the mechanically optimal angle for the bending→axial conversion above, so it needs its own term, not just length/alignment.
   - **Taper penalty** — a tapered tip carries less material than its length alone suggests, so it loses to a full-width alternative when one exists, but still wins over no strut at all.
7. Land surviving strut quads on the real wall geometry, fuse overlapping ones, then **notch them out of** the offset footprint — the void is footprint *minus* struts.
8. Fillet every corner by `insetMm` (radius = inset, 1:1).
9. Weight-aware filter: per disjoint void pocket, compare wall-skin cost (`perimeter * wallThicknessMm`) to the pocket's own area. Fill back to solid if skinning costs more than the pocket saves.
10. Cut the solid by the remaining voids: full Z slab for near-horizontal faces, `min(face Z)` to solid top for slanted ones.

### Calibrate
Diagnostic-adjacent. Read-only — never mutates `current`, not an Operation, no history entry.

**Why:** a tool to automate tuning printer accuracy, distilled to the two corrections that account for nearly all of it in practice: shrinkage (Contour) and an unexplained hole-specific offset (Hole).

**Algorithm:**

1. User picks two faces: both perpendicular to the build axis (keeps the measurement in-plane, independent of Z calibration) and parallel to each other (a caliper reading only means something between parallel planes).
2. Classify the pick as **Contour** or **Hole** by topology (does either face touch a hole-inner-edge). Can't mix Contour with Hole — that conflates two distinct, independently-tuned error sources.
3. Compute the nominal CAD (Computer-Aided Design) span: perpendicular distance between the two picked planes, as originally modeled.
4. User measures the print with calipers, enters the value.
5. Apply the matching correction:
   - **Contour**: `contourScale = nominal / measured` — a ratio, since shrinkage scales proportionally with the part.
   - **Hole**: `holeRadiusOffsetMm = 0.5 * (nominal - measured)` — an absolute offset, since hole error doesn't scale with diameter the way Contour error scales with span.

---

## Healing

A tool's result isn't "successful" until it's valid. Healing happens once, at the [commit](#operation) step — the moment a tool's finished result is about to replace a Part's `current`, not at any intermediate step inside a tool's own algorithm. It's never re-checked afterward, so anything reading a Part's `current` (Analysis, Rendering, future tools) can trust it unconditionally. Lives in `GeometryOps` as a shared utility every tool's commit path calls, not reinvented per-tool — the same pattern `GeometryOps` already follows for other generic OCCT primitives (offset, fillet, boolean, ring math).

**Why:** OCCT operations (booleans, fillets, mesh-to-BRep conversion) can produce shapes that are topologically well-formed by OCCT's own rules but geometrically broken in ways that corrupt slicing or printing if left uncaught. Healing in one shared place, rather than each tool inventing its own ad hoc check, is what makes the "trust `current` unconditionally" guarantee actually hold.

OCCT's `ShapeFix` package supplies the actual repair *primitives* — closing a gap, fixing a curve-on-surface mismatch, correcting orientation. We don't reimplement geometric healing; that would be reinventing a CAD kernel. What's ours is the **dispatch**: deciding which specific fixer applies to which detected problem, what tolerance bounds it, and what happens if it still fails afterward. There's no one generic "fix it" call — each type below routes to a different tool, on purpose.

**Types & fixes:**

- **Not watertight** — a free edge (used by only one face) means the shell doesn't enclose a defined volume; Analysis can't slice an open shell into meaningful cross-sections.
  - Detection: build an edge→face adjacency map (`TopExp::MapShapesAndAncestors`) and flag any edge whose face count is 1 instead of 2.
  - Fix: re-run a sewing pass (`BRepBuilderAPI_Sewing`, the same tool Import already uses) at the tolerance constant's value — the same mechanism, just applied again post-operation, snapping free-edge endpoints that are only slightly apart.
  - Unrecoverable if the gap is wider than tolerance — that's a real hole in the source, not a numerical artifact, and gets reported rather than forced shut.
- **Inconsistent orientation / self-intersection** — face normals disagreeing about which side is "outside," or a trimmed surface intersecting itself.
  - Detection: `BRepCheck_Analyzer`, walking the shape and reporting per-sub-shape status.
  - Fix: `ShapeFix_Face` (one bad face) or `ShapeFix_Shape` (shape-wide) re-derives consistent orientation and, where possible, re-trims a self-intersecting surface along the intersection curve — the exact mechanism already proven for Structure's fillet path (`ShapeFix_Face` inside `FilletFaceWithChFi2d`), generalized here instead of staying that one tool's special case.
  - Unrecoverable if the self-intersection is severe enough that re-trimming can't produce two well-bounded faces — rare, but possible after an extreme boolean.
- **Tolerance violation** — two points meant to be coincident (a sewing seam, a boolean cut boundary) sit farther apart than the shape's tolerance.
  - Detection: `BRepCheck_Analyzer`'s tolerance-related statuses, or comparing a known-coincident pair directly against the tolerance constant.
  - Fix: `ShapeFix_Shape`'s gap-closing, explicitly bounded to the tolerance constant's value — healing can never widen the *effective* tolerance beyond what we've already decided is acceptable elsewhere, so it can't quietly make the model less precise than the rest of the app assumes.
- **Degenerate sub-shapes** — zero-area faces, zero-length edges.
  - Detection: area/length checks per face/edge against the tolerance constant.
  - Fix: removed rather than repaired — there's no meaningful fix for a sub-shape with no real extent — followed by `ShapeUpgrade_UnifySameDomain` to re-merge whatever coplanar faces the deletion exposed.

**Not a type:** a sub-shape shared by 3+ faces (e.g. two faces resolved out of a self-intersection by splitting along the intersection curve, which legitimately produces this).

- `BRepCheck_Analyzer` doesn't flag it — it's checking BRep consistency, not manifold-ness for printability, and there'd be nothing for `ShapeFix` to fix since the data structure is correct.
- Rejecting it would also contradict the self-intersection fix above: that fix's correct output *is* this shape.
- If a near-zero-width pinch ever matters physically, that's an Analysis concern (Instability's `minWidth` already watches for vanishing cross-sections), not a Healing one.

**Algorithm:** check → if clean, done → if not, dispatch to the matching fixer → re-check → still broken is the "no" branch of `Succeeded & valid?`, healed is "yes".

1. Run `BRepCheck_Analyzer`, plus our own free-edge check (manifold/printability concerns it doesn't cover).
2. Clean → done, report success.
3. Not clean → dispatch to the matching fixer from Types & fixes above — which problem determines which `ShapeFix` tool, not one generic call — bounded by the tolerance constant.
4. Re-run the same checks on the healed result.
5. Still invalid → report the standardized error, don't commit (Event Flow: `Succeeded & valid?` = no). Otherwise → report success, commit.

```mermaid
flowchart TD
    Start(["Tool produces a candidate shape"])
    Start --> Check["Run BRepCheck_Analyzer +<br/>our own free-edge check"]
    Check --> Clean{"Clean?"}
    Clean -- "yes" --> Success(["Valid — report success"])
    Clean -- "no" --> Heal["Attempt heal: ShapeFix_Shape<br/>family, bounded by the<br/>tolerance constant"]
    Heal --> Recheck["Re-run the same checks"]
    Recheck --> StillBad{"Still invalid?"}
    StillBad -- "yes" --> Fail(["Invalid — report error<br/>(Event Flow: Succeeded & valid? = no)"])
    StillBad -- "no" --> Success
```

---

## Background

Design decisions that didn't make it in, open work, and explicit non-goals. Not needed to understand the current architecture — context for why it's shaped the way it is.

### Rejected approaches

**Shader-based tessellation (Rendering):** considered as a replacement for `BRepMesh_IncrementalMesh` — faster, but it means re-implementing OCCT's surface evaluation (planes, cylinders, NURBS, trimmed surfaces) in GLSL, for limited hardware support — and no profiling shows CPU-side triangulation is actually a bottleneck at this app's scale (a handful of Parts, cached results). Revisit only if that changes.

**Elephant's foot correction (Calibrate):** a third correction, alongside Contour and Hole, was considered and dropped. It needs a genuinely different pick interaction (two *edges* on one cap, vs. two *faces* for Contour/Hole — a structurally separate interaction model, not a UI variation), and unlike Contour/Hole error it's fixable after the fact via post-processing, so it wasn't worth the added interaction complexity.

**Concurrent per-tool runs on one Part (Concurrency):** considered running two tools concurrently on one Part's disjoint faces, if each can prove it never touches the other's faces. Doesn't hold up for two reasons:

- OCCT's coplanar-merge/topology-rebuild steps can touch neighboring faces even when a carve's intent is local, so disjointness is hard to prove in the first place.
- Even if proven, two independently-computed results still need merging back into one `current` afterward, which OCCT has no general primitive for.

Most realistic same-Part tool pairs aren't parallelism candidates anyway: Analysis needs a stable `current` to produce meaningful results so it can't run concurrently with an in-flight Operation, and Calibrate is synchronous/read-only already.

**Explicit dependency/reference graph (Data):** considered maintaining a separate structure tracking relationships between sub-shapes, but `TopoDS_Shape`'s own hierarchy (Solid → Shell → Face → Wire → Edge → Vertex, traversed via `TopExp_Explorer`) already *is* that graph — a parallel system would just be another thing to keep in sync for no added information (see [Data → Part](#part)).

### Future & deferred

**Future tools** — not designed yet; it'll be a while before either is picked up:
- **Tolerance** — clearance/fit adjustment between mating parts. Likely scoped to face-tagging (user marks press-fit/smooth-motion-fit faces) rather than full assembly modeling, lighter-weight than tracking relative Part transforms. 3MF's multi-object support could be a future source of relative positioning if that ever matters.
- **Cut** — divides an oversized part into printable pieces. The one tool that would be allowed to suggest an explicit fix rather than just flag a problem.
- **Export** — writes the final geometry out, mirroring Import's formats, and triggers an Analysis run first (see Tools → Import → Hand-off to Analysis for the same read-only-triggers-are-safe reasoning). For STEP output, also embeds the Part's pre-Operation original shape (`history`'s first entry, see [Data → Part](#part)) inside a standard STEP comment block — a fully spec-conformant `.step` file any STEP reader opens normally, written with a `tempered.stp` extension so Temper itself recognizes it and can round-trip Undo history through it. STL output (`.stl`, no `tempered.stl` variant) carries no such payload: STL has no comment mechanism to embed it in, and isn't precise enough to make a lossless original worth preserving through it anyway. Open question: whether an unresolved Issue blocks the write or is only an advisory warning.

**Deferred:**
- **Orient** — not yet implemented. Re-runs Analysis's detectors across candidate bed orientations, recommends/previews whichever minimizes overhang/instability/layer-difference signals; changes build direction, not the shape. The orientation-search itself (how candidates are generated/scored beyond reusing Analysis's detectors) isn't designed. Open question whether accepting a recommendation needs an Operation/history entry, or is purely UI-local state like Calibrate's build-direction vector.

**Per-tool open items:**
- **Analysis** — Not Enough Space's cooling threshold and corner-multiplier weight are uncalibrated, pending print experiments. OCCT boolean cost on continuously-changing cross-sections is an open perf risk — profile before picking a fix. See [analysis-redesign.md](todo/analysis-redesign.md).
- **Import** — a scan-mesh format (PLY) is a candidate addition, not yet confirmed; drop it if it turns out complex, since it's a nice-to-have, not core. Separately, an optional "attempt curve reconstruction" toggle (off by default, recovering only flat faces; on, attempts to fit arcs/curved surfaces from the tessellation) depends on a mesh-to-BRep surface-fitting capability that doesn't exist yet — no confirmed turnkey OCCT solution for it.

### Diagram maintenance

Each Mermaid block in this doc (Event Flow's overview, Event Flow's detailed loop, Architecture at a glance's overview, Data's overview, Analysis's algorithm, and Healing's) is its own source of truth, kept separately editable. [architecture-event-flow.png](architecture-event-flow.png) is one combined generated snapshot of all six, for viewers that don't render Mermaid live — not six separate files, so there's one image to find.

- The other five diagrams render off to the side of the main loop, each in its own labeled box — none graph-connected to each other.
- Analysis's diagram includes its detectors' internal steps directly inside each detector's box — deliberately not split into a second diagram, since that split read as "missing" rather than "more detail available" to a reader without the surrounding context.
- Data's four per-item diagrams (Part, Issue, Operation, Settings) are a deliberate exception, not part of this six: each is small, embedded inside its own list item rather than standalone, and stays Mermaid-only — folding four more boxes into the combined layout wouldn't scale the "beside the main loop" approach cleanly, and a reader without live Mermaid still gets the full fact in the prose right above each one.

**Regenerate after editing any of the six:**

1. Extract each block, wrap the other five in labeled subgraphs nested inside one outer `Side` subgraph, link `Main ~~~ Side` (an invisible Mermaid edge — layout hint only, draws nothing) so the renderer places them beside the main loop instead of stacking everything vertically.
2. Render the combined source once: `npx -y @mermaid-js/mermaid-cli -i <combined .mmd> -o architecture-event-flow.png -b white -w 7000 -s 2`.
3. High resolution so it stays readable when zoomed; if your viewer always shrinks images to a fixed thumbnail, open the file directly rather than relying on an inline preview.

Node/subgraph IDs must be unique across the *entire* combined source, not just within one diagram's original block — Mermaid silently merges same-named nodes/subgraphs from different diagrams into one, which is what caused the Not Enough Space detector to render outside its own group the first time (its subgraph ID collided with the fan-out diagram's `NES` node).
