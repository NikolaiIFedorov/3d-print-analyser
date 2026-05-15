# Open boundary — import / UI solution (design only, 2026-05-14)

This document records the **agreed product direction** for `AppInvalidTag::OpenBoundary` without implementing the expanded import UI yet. Implementation can follow in a dedicated UI pass.

## What `OpenBoundary` means here

An edge used by the solid’s face loops appears in **only one** half-edge incidence (see `GeometryValidity::EvaluateAppInvalidTagsForSolid`). That usually means an **open sheet or shell** — not necessarily broken data, often **by design** for viewing or single-sided parts.

## Principles

1. **Do not treat “open” as always wrong** — Default stance: **inform**, don’t auto-close on import unless the user opts into a **named** repair (hole fill / cap / “make solid” experiment) with clear semantics.

2. **Validity is contextual** — Same scene: **warn** for display; **block or gate** only for tools whose contract requires a **closed** volume (e.g. certain CGAL carve paths, future booleans). Copy should say **invalid for this step** where appropriate.

3. **One diagnostics surface, many entry points** — File bar / dialog import should **not** host a large expandable panel (space + consistency). Prefer a **single** collapsible region elsewhere (e.g. Files tab body, import result card, or small “Scene health” drawer) that any import path can **focus or deep-link** after load.

## UI phases (recommended order)

### Phase A — Visibility + disposition (first ship)

- **When:** After import **attach** completes (solid exists in the scene, validity cache fresh). Only then is **3D blame** meaningful: highlights can reference **boundary edges** or solid-level tint (v1 may be coarse; v2 can walk open-boundary edge sets).
- **What:** If `OpenBoundary` is set on the affected solid(s), show a **standard coded warning** (stable **code** + short **description**), aligned with existing tool error payloads where practical. Optional **Details** expands inline with dev-oriented subset from `DescribeAppInvalidTagsForLog`.
- **Import prerequisite (clean-dependent tools):** For prerequisites such as **“Import a file”** that also imply **geometry clean enough for the tool contract**, the prerequisite is **not Done** until either:
  - **Fix** — user runs the **named** open-boundary repair and a **re-check** clears `OpenBoundary` (or the tool’s contract is satisfied); or
  - **Exit** — user leaves the import flow **without** satisfying clean contract: geometry **stays in the scene** for viewing, but **clean-dependent tools remain gated** until **Fix** is run later from a **secondary entry point** (shared diagnostics / Files / scene health — must be specified in implementation so users are not stranded).
- **Copy for Exit:** Make clear that Exit **does not** complete the clean prerequisite — e.g. “Exit keeps this file in the scene. Tools that need a closed volume stay unavailable until you repair.”

### Phase B — Prerequisite rows in other tools

- When a **tool** lists a prerequisite (e.g. Structure), if the solid fails **partly because of** `OpenBoundary`, expand the prerequisite row (or associated card) to show the **same** short message + log subset for debugging.
- **Block** operation start when the contract requires a closed volume; **do not** auto-patch by default.

### Phase C — Fix control (only when repair exists)

- Primary actions on the import / diagnostics surface: **Fix** (runs the **concrete, named** repair — CGAL soup repair, cap holes, etc.) and **Exit** (dismiss import UI, keep scene state as above). **No “Ignore”** for clean-contract completion: that would contradict “tools depend on clean file.”
- Until a repair exists: **Fix** is **disabled** with explicit copy (“Repair not available yet”) or omitted; **Exit** remains so users can leave the flow without pretending the model is closed.

## Import paths

| Path | Behaviour |
|------|-----------|
| **File bar / quick open** | Keep **minimal** feedback (toast, one line, or badge). Full text lives in the **shared** diagnostics area (Phase A). |
| **Files panel / import flow** | Natural home for **expandable** diagnostics, **Fix** / **Exit**, and post-Exit **secondary Fix** entry. |
| **Drag-and-drop** (if added) | Same as file bar → route to shared diagnostics after attach. |

## Tool-generated open boundaries

- **Default:** run the same **evaluation + cache refresh** after the tool commits; show **banner or tool panel** message if `OpenBoundary` appears and the tool was supposed to yield a **closed** solid.
- **Auto-patch:** only if the tool’s spec says “output must be closed” **and** a **safe, idempotent** repair exists (tests + logs). Otherwise **surface + optional user-triggered repair** — avoids hiding tool bugs.

## Code touchpoints (for implementers)

- Read tags: `Solid::cachedAppInvalidGeometryTags` + `cachedAppInvalidGeometryTagsFresh` (refresh after `CreateSolid` / `MergeCoplanarFaces`; lazy refresh if stale).
- Strings: `GeometryValidity::DescribeAppInvalidTagsForLog` for dev-style detail; product copy should be shorter and keyed by tag.
- Import pipeline: `Display::ProcessDeferredImportIfAny` / `MainThreadPipeline` finalize steps — good hook points once UI owns a “last import diagnostics” struct.

## Outcome

Design captured; **UI implementation deferred**. No automatic “close mesh” on import in this plan.

## Revision

- **2026-05-14 (initial)** — Phased visibility, gating, patch-only-when-real.
- **2026-05-14 (update)** — **Post-import** warning so **3D blame** can reference scene geometry; **import prerequisite** for clean-dependent tools stays incomplete until **Fix** succeeds; **Exit** leaves import UI and keeps the solid but **does not** satisfy clean contract; primary actions **Fix** + **Exit** (no **Ignore** for that contract); secondary **Fix** entry required after **Exit**.
