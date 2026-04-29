# Architecture: UI Panel Direct Access Refactor

## Executive Summary

Replaces the string-based `SetSectionVisible` / `SetSectionValue` lookup API with direct `Panel*` member access in `Display`. The change eliminates an entire class of silent lookup-mismatch bugs (one of which was the proximate cause of the "Import file button stays visible" regression), reduces per-update overhead, and aligns with the project's DRY and consistency guidelines.

**Verdict: Approve**

**Top strengths:**
1. Removes the ID mismatch footgun — wrong string silently does nothing.
2. Zero-overhead updates: direct struct field writes instead of two-level tree search.
3. Follows the same pattern already used for `std::deque<Panel> panels` in `UIRenderer`.

---

## 1. Requirements & Motivation

### Functional Requirements
- After importing a file, the "Import file" button must be hidden and the analysis result/verdict/config sections must be shown.
- The UI must reflect the current scene state every time `Frame()` processes a dirty scene.

### Non-Functional Requirements
- Changes must not affect rendering performance — `Frame()` is the hot path.
- No ABI or API breakage to existing callers of `UIRenderer`.

### Constraints
- C++23, CMake, macOS/SDL3/OpenGL stack.
- `Panel` is a recursive self-referential struct; container choice for `sections` is constrained.

### Root Cause of the Bug
`SetSectionVisible("Analysis", "Import file", ...)` was called with the display label `"Import file"` but the section was registered with id `"ImportAction"`. The lookup silently returned without matching, leaving `visible` unchanged. String IDs are an indirect, unverified reference; the compiler cannot catch the mismatch.

---

## 2. Solution Description

### Components

| Component | Change |
|-----------|--------|
| `UIRenderer.hpp` | Added `MarkDirty()` to expose the internal `dirty` flag for direct callers |
| `Display` (display.hpp) | Added `Panel* uiResult`, `uiImportPara`, `uiVerdict`, `uiConfig` members |
| `Display::InitUI()` | Assigns the four pointers when sections are created; `reserve(4)` prevents reallocation |
| `Display::Frame()` | All `SetSectionVisible` / `SetSectionValue` calls replaced with direct field writes |

### Data Flow

**Before:**
```
Frame() → SetSectionVisible("Analysis", "Result", ...) 
        → GetPanel("Analysis")        // O(n) linear scan
        → recursive id search         // O(n) tree walk
        → panel.visible = value       // write
```

**After:**
```
Frame() → uiResult->visible = value   // O(1) direct write
```

### Key Abstractions
- `Panel*` raw pointers stored as `Display` members. The pointees are owned by `UIRenderer::panels` (`std::deque`), which never invalidates pointers. The `sections` sub-vector inside each `Panel` is stabilized by `reserve(4)`.

---

## 3. Design Principles

- **DRY**: Eliminated 7 duplicated string literals that each referenced the same 4 panels. Each panel is now named once (at creation) and referenced directly thereafter.
- **Fail-loud over fail-silent**: String ID mismatches silently no-op; pointer access either works correctly or crashes loudly on a null/dangling pointer — both are preferable for debugging.
- **Performance first** (project guideline): Removed two O(n) tree walks per dirty-scene frame (one for each `SetSection*` call). Replaced with direct field writes.
- **Consistency**: `UIRenderer` itself already uses `std::deque<Panel>` for stable pointer semantics at the top level. This change applies the same rationale one level down.

---

## 4. Alternatives Considered

| Option | Pros | Cons | Verdict |
|--------|------|------|---------|
| Keep string IDs (status quo) | No structural change | Silent failures, O(n) search per call, mismatch-prone | Rejected |
| `Panel*` members + `reserve` | O(1), compile-time safety, minimal change | `reserve` count must be kept in sync manually | **Chosen** |
| `std::deque<Panel> sections` | Pointer-stable without `reserve` | C++ only allows `std::vector` for incomplete (self-referential) types; `deque` fails to compile | Rejected (language constraint) |
| `std::vector<std::unique_ptr<Panel>> sections` | Pointer-stable, no manual count | 18 call sites in `UIRenderer.cpp` require `->` dereference through `unique_ptr`; significant churn in hot rendering path | Deferred (viable but high-impact) |
| `std::unordered_map` keyed by ID | O(1) lookup by name | Unordered maps don't preserve insertion order — sections would render in hash order, breaking layout | Rejected |

---

## 5. Technology & Dependencies

### New Dependencies
None.

### Integration Impact
- `MarkDirty()` added to `UIRenderer` public API — non-breaking addition.
- `SetSectionVisible` / `SetSectionValue` remain in `UIRenderer` — not removed (still usable for future callers).
- `display.hpp` includes `Panel.hpp` transitively via `UIRenderer.hpp` — no new include needed.

---

## 6. Tradeoffs

| Decision | Alternative | Chosen Approach | Rationale |
|----------|-------------|-----------------|-----------|
| `reserve(4)` magic number | No reserve (unsafe) | Explicit reserve with comment | `vector` requires stable capacity; count is trivially audited against the 4 `AddParagraph`/`AddSection` calls directly below it |
| Keep `SetSectionVisible` / `SetSectionValue` | Remove them | Keep as dead code | They may be useful for future dynamic panels not constructed in `InitUI`; removal is premature |
| `Panel*` raw pointer | `Panel&` reference member | Raw pointer | Reference members prevent default move/copy; raw pointers with null-init are idiomatic for optional lazy-initialized handles |

---

## 7. Best Practices Compliance

### Conforming
- **Reserve container capacity** — `best_practices.md` explicitly recommends `std::vector::reserve` when size is known. Applied here.
- **Prefer contiguous containers** — `std::vector<Panel>` retained for cache friendliness.
- **Fail-loud** — Direct pointer access surfaces bugs immediately rather than silently no-opping.
- **DRY** — 7 string literals collapsed to 0; each panel referenced once through a typed pointer.
- **Performance** — O(n) searches eliminated from the per-frame hot path.

### Non-Conforming

| Issue | Severity | Notes |
|-------|----------|-------|
| `reserve(4)` is a magic number that must be kept in sync manually | Minor | The count is adjacent to the `AddParagraph`/`AddSection` calls and easy to audit, but a comment-only guard. A future addition without bumping the reserve would silently re-introduce dangling pointers. Consider a static assert or a `Panel::AddParagraphStable` helper that enforces pre-reservation. |
| `uiResult->values = lines` copies a `std::vector<SectionLine>` | Minor | Each `SectionLine` contains `std::function` members; the copy is non-trivial. `std::move(lines)` would be correct here since `lines` is a local that is not used after the assignment. |
