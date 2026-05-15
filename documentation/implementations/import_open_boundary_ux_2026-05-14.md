# Open boundary — import / UI solution (design only, 2026-05-14)

This document records the **agreed product direction** for `AppInvalidTag::OpenBoundary` without implementing the expanded import UI yet. Implementation can follow in a dedicated UI pass.

## What `OpenBoundary` means here

An edge used by the solid’s face loops appears in **only one** half-edge incidence (see `GeometryValidity::EvaluateAppInvalidTagsForSolid`). That usually means an **open sheet or shell** — not necessarily broken data, often **by design** for viewing or single-sided parts.

## Principles

1. **Do not treat “open” as always wrong** — Default stance: **inform**, don’t auto-close on import unless the user opts into a **named** repair (hole fill / cap / “make solid” experiment) with clear semantics.

2. **Validity is contextual** — Same scene: **warn** for display; **block or gate** only for tools whose contract requires a **closed** volume (e.g. certain CGAL carve paths, future booleans). Copy should say **invalid for this step** where appropriate.

3. **One diagnostics surface, many entry points** — File bar / dialog import should **not** host a large expandable panel (space + consistency). Prefer a **single** collapsible region elsewhere (e.g. Files tab body, import result card, or small “Scene health” drawer) that any import path can **focus or deep-link** after load.

## UI phases (recommended order)

### Phase A — Visibility (first ship)

- After import completes (and solid cache is fresh), if `OpenBoundary` is set on the active solid, show a **compact line** in the import / files context: e.g. “Open boundary — not a closed volume.”
- Optional: link or button **“Details”** that expands **inline** (same tab), not a second modal unless needed.

### Phase B — Prerequisite / tool gating

- When a **tool** lists a prerequisite (e.g. Structure), if the solid fails that prerequisite **partly because of** `OpenBoundary`, expand the prerequisite row (or associated card) to show the **same** short message + `DescribeAppInvalidTagsForLog` subset for debugging.
- **Block** starting the operation when the tool contract requires closed volume; **do not** auto-patch here by default.

### Phase C — “Patch” toggle (only when the patch exists)

- A toggle **“Close / repair open boundary”** appears only when a **concrete** repair pipeline is implemented and named (CGAL soup repair, cap holes, etc.). Until then, omit the toggle or label it **“Coming soon”** — avoid a control that promises an undefined fix.

## Import paths

| Path | Behaviour |
|------|-----------|
| **File bar / quick open** | Keep **minimal** feedback (toast, one line, or badge). Full text lives in the **shared** diagnostics area (Phase A). |
| **Files panel / import flow** | Natural home for **expandable** diagnostics and future patch toggles. |
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
