# Calibrate second pick — parallel face constraint (2026-05-05)

## Idea

Gate Calibrate tool second measurement pick so it only accepts geometry whose **resolved face** is compatible with the first pick, using the same parallelism rule as nominal span (`CalibNominal::SpanBetweenFaces`): `|dot(na, nb)| >= 0.75`, distinct faces. This filters hover and click commit.

## Plan

- Centralize threshold as `CalibrateNominal::kFaceNormalParallelAlignThreshold` and add `NormalsAlignedForCalibPick`.
- Helper `CalibSecondPickAcceptsHit` in `display.cpp` (anonymous namespace) using existing `ResolveCalibFaceForWorkflow`.
- `UpdatePickHover` / `TryCommitCalibrateFacePick`: when point 2 row is active and point 1 has a pick, reject non-matching hits (parallel + contour/hole workflow).

## Note on “same normal”

Product language “same normal” often means **coplanar parallel faces** (including **opposite** outward normals on opposite sides of a slab). This implementation allows **parallel or opposite** so box thickness still works; it does **not** require `dot > 0` only.

## Outcome

- Clean build of `CAD_OpenGL`.
- Second prerequisite subtitle updated to “parallel to first pick”.

---

## Update — face-only picks + reject hover (same day)

**Idea:** Calipers need a face to seat against; edge-primary picks removed for Calibrate. Invalid second-pick targets should read as blocked (dim + translucent), not invisible.

**Implementation:**

- `PickCalibrateAtPixel`: face raycast only; commits set `calibEdgePoint*` to null.
- `hoverCalibPickRejected` + `SetHoverCalibPick(..., rejected)`; second-pick constraint fail → hover face kept, `rejected=true`.
- Separate mesh `UploadPickHighlightRejectMesh` / `DrawPickHighlightReject` (blend, `kPickHighlightRejectHoverAlpha` in `RenderingExperiments`) after main pick fill; muted RGB from theme base.
- Stencil: reject pass runs with patches + main pick highlight (same `GL_REPLACE` block).

**Outcome:** Build clean; edges no longer selectable for new calibrate picks (legacy committed edge refs still clear via existing rebuild hygiene).

**Retro:** Split VAO for reject avoids per-vertex alpha in shader; tune alpha and muted mix in one place.

---

## Update — translucent veil on all non-selectable faces (second pick)

While waiting for the second Calibrate point, every pick-triangle face that fails `NormalsAlignedForCalibPick` relative to the first resolved face (excluding already committed calib faces) is collected and drawn in a single mesh with `kPickHighlightCalibInvalidPoolAlpha` (0.13) and a light neutral tint. Draw order: main pick highlight → invalid pool → reject-hover (darker) so the cursor target still reads strongest.

Constants: `RenderingExperiments::kPickHighlightCalibInvalidPoolAlpha`; new GL path `UploadPickHighlightCalibInvalidMesh` / `DrawPickHighlightCalibInvalid`.

**Tweak (readability):** `kPickHighlightCalibInvalidPoolAlpha` had been turned up to 1.0 during experiments, which removed translucency and made dark `poolTint` read as opaque crushing. Restored modest alpha (~0.22) and shifted pool vertices toward a **light haze** (mix toward ~0.91 RGB) so standard blend reads as frosted/see-through rather than only darker. Slightly lowered brighten on that draw pass to avoid blowing highlights on the light overlay.

---

## Update — invalid veil read opaque (diffuse + bright tint) (2026-05-05)

**Problem:** Blend and `uAlpha` were active, but `basic.frag` still applied directional **lighting** on the invalid-pool and reject-hover draws. That pushed fragment RGB toward white (`min(fragColor * lighting, 1.0)`), so the overlay looked like a **solid light gray sheet** instead of a see-through tint over the patch. (Grid or scenery *through closed solids* is still impossible without true transparent materials — depth/occlusion unchanged.)

**Approach:** `DrawPickHighlightCalibInvalid` and `DrawPickHighlightReject` set `uLightingEnabled = 0` so output is `vec4(fragColor, uAlpha)`. Nudged `poolTint` closer to theme base, lowered `kPickHighlightCalibInvalidPoolAlpha` to 0.16 and reject alpha to 0.32.

**Files:** `OpenGLRenderer.cpp`, `RenderingExperiments.hpp`, `display.cpp`.

**Outcome:** Clean build; invalid layers should read as translucent over the shaded mesh.

**Retro:** Any future “glass” overlay pass that reuses `basic` should treat **lighting + alpha** interaction as a footgun; flat unlit tints are safer for SRC_ALPHA blending.

---

## Update — alpha vs brightness (blend math) + albedo-matched pool tint (2026-05-05)

**Observation:** With a **light** `poolTint` vs **darker** lit patch, standard blend `α·src + (1-α)·dst` makes **small α darken** and **large α lighten** — so `kPickHighlightCalibInvalidPoolAlpha` is **not** a transparency dial, it’s the mix ratio.

**Change:** Build `poolTint` from `Color::GetFace()` times approximate lit response (`SceneMeshBrightenAmount`, typical diffuse), then a modest cool shift. Defaults: `kPickHighlightCalibInvalidPoolAlpha = 0.28f` (user’s experimental `0.9f` reset). Document in `RenderingExperiments.hpp` that α is veil strength once tint matches dst.

---

## Update — hover-only blocked styling (2026-05-05)

**Idea:** Full-model invalid-face veil was subtle/noisy; product-wise, a **distinct hover** on blocked faces may suffice.

**Implementation:** `kCalibrateSecondPickDrawInvalidFacePool` default **false** — skip `RenderPickHighlightCalibInvalid` and the invalid-pool mesh build unless toggled. Reject hover uses **warning amber** fill + **face boundary** pick-segment outline (same line pass as other pick lines). Slightly higher reject alpha (`kPickHighlightRejectHoverAlpha` 0.42).

**Files:** `RenderingExperiments.hpp`, `display.cpp` / `display.hpp`, (log).

**Outcome:** Build clean.

---

## Update — blocked hover = same accent path, darker (2026-05-05)

**Idea:** Avoid separate warning style; match valid-face pick highlight (`GetAccentSteps` + lit `basic` shader) with a **lower luminance** accent step (`kCalibrateRejectHoverAccentDepthStepsDark` / `Light`). No reject mesh or orange outline; `DrawPickHighlightReject` stays unused when mesh empty.

**Files:** `RenderingExperiments.hpp`, `display.cpp`, `display.hpp`.

---

## Update — blocked hover: darker **neutral** gray (2026-05-05)

**Correction:** Blocked second-pick hover should read as **grayscale**, not darker accent (low-L accent was reading mud/gray).

**Change:** `Color::GetUI(depth)` vertex color + same lit pick mesh as valid hover. Tune via `kCalibrateRejectHoverGrayUiDepthDark` / `Light`.

---

## Update — contour vs hole second pick (2026-05-05)

**Idea:** Contour (outer) and hole (opening) calibrations use different compensation parameters; do not allow pairing a contour-classified face with a hole-classified face on the two measurement picks.

**Implementation:** `CalibSecondPickWorkflowsCompatible` in `CalibDistanceType.hpp` (`ClassifyFace` on both picks). `CalibSecondPickAcceptsHit` takes scene, layer height, `holeEdges` (same inputs as `ClassifyFace`). `CombinePickedFaces` returns `CalibWorkflow::None` for mixed contour+hole as a safety net. Invalid-face pool (when enabled) also treats workflow mismatch like non-parallel.

**Files:** `CalibDistanceType.hpp` / `.cpp`, `display.cpp`.

---

## Update — hole detection + second-pick gate (2026-05-05)

**Problem:** Contour/hole block often did not apply: `CalibSecondPickAcceptsHit` skipped workflow when `layerHeightMm <= 0.0`; vertical hole walls often only shared **vertices** with inner loops, not the same `Edge*`. Annular faces use **two** loops (`>= 2`), not `>= 3`.

**Changes:** Workflow check runs whenever `scene != nullptr`. `RebuildHoleCalibTopology` adds `holeRingPoints` from inner-loop edge endpoints. `FaceQualifiesAsHole` uses `loops.size() >= 2`, inner-loop edges, or any boundary vertex in `holeRingPoints`.

**Files:** `CalibDistanceType.hpp` / `.cpp`, `display.cpp` (`Geometry/Point.hpp`).

---

## Update — layer hole semantics + display wiring (2026-05-05)

**Idea:** Calibrate “hole” workflow means **layer hole** (opening in a slice ⟂ build direction), not “any face with an inner loop”. Topology rebuild restricts inner-loop edges to **cap** faces whose normals align with the build axis; first-layer slab extent uses projection along that axis.

**Display wiring:** After `CalibDistanceType` gained `buildDirWorld` on `RebuildHoleCalibTopology`, `ClassifyFace`, and `CombinePickedFaces`, `display.cpp` now includes `CalibDistanceType.hpp`, passes `DefaultCalibrateBuildDirection()` at all call sites, and threads the same vector through `CalibSecondPickAcceptsHit`.

**Outcome:** Clean build; hole classification and second-pick gating stay consistent with the layer-hole definition until build direction is user-configurable.

**Files:** `CalibDistanceType.hpp` / `.cpp`, `display.cpp`.

---

## Update — annular-only “hole” workflow (2026-05-05)

**Problem:** Classifying any single-loop face that **touches** inner-loop edges or ring vertices as `Hole` can pull in **contour** faces (shared boundary / mesh quirks). That defeats separating contour vs hole compensation paths.

**Approach:** `Hole` is only the **annular cap**: planar face with **≥2 loops** (outer + inner hole boundary) whose normal aligns with the build direction. Removed `RebuildHoleCalibTopology`, `holeEdges`, and `holeRingPoints` from the Calibrate path — classification is purely per-face. **Vertical hole walls** are now `Contour`; only the top/bottom annulus with two loops is `Hole`.

**Trade-off:** Picking two hole-wall faces stays contour+contour (allowed). Mixed annulus + wall becomes hole+contour and is **blocked** by the existing gate — users measure hole compensation on the **cap** annulus, not the wall.

**Files:** `CalibDistanceType.hpp` / `.cpp`, `display.cpp`.

---

## Update — hole sidewalls + slanted openings (2026-05-05)

**Problem:** Annular-only classification made **vertical hole walls** `Contour`, so **shrinkage** applied instead of **hole** compensation. Slanted window facets are not caps ∥ build, so inner-loop edges were never collected and sidewalls stayed `Contour`.

**Approach:** Rebuild inner-loop edges from any **planar** face with ≥2 loops (tilted cap allowed for edge discovery). `FaceQualifiesAsHole`: annulus = ≥2 loops + cap ∥ build; **sidewall** = one loop + incident inner-loop **edge** + not cap ∥ build (no vertex-only rule). `buildDirWorld` on rebuild reserved for future layer filtering.

**Files:** `CalibDistanceType.hpp` / `.cpp`, `display.cpp`.

---

## Update — layer-hole topology only from caps ∥ build (2026-05-05)

**Problem:** Collecting inner loops from **tilted** multi-loop faces made **oblique pocket walls** share `layerHoleInnerEdges` and get **`Hole` / hole radius offset**. For many slicers those openings are **not** per-layer enclosed voids (stair-step / no horizontal inner contour), so hole compensation is the wrong model.

**Approach:** `RebuildHoleCalibTopology` again requires the framing face **`FaceCapParallelBuildDir`** before adding inner loops. Slanted recesses → for meshes with no horizontal annulus those walls classify **`Contour`**. Through-holes that meet a **horizontal** annulus still contribute inner-loop edges; vertical sidewalls there remain **`Hole`**.

**Files:** `CalibDistanceType.hpp` / `.cpp`.

---

## Update — prerequisite copy, sharper subtitle text, stricter parallel gate (2026-05-08)

**Problem:** Calibrate prerequisite subtitles were long; second-line text looked soft at `fontScale` 0.85; users could commit second face picks that still looked visibly non-parallel.

**Approach:**

- UI copy in `display.cpp`: point 1 subtitle **"to measure against"**, point 2 **"parallel point to first selection"**.
- `kFaceNormalParallelAlignThreshold` raised from **0.75** to **0.985** (same order as edge chord `EdgesAreParallelForCalib`, ~10°) so `NormalsAlignedForCalibPick` matches product “parallel walls” expectation; `SpanPreviewBetweenFaces` / slab vs centroid heuristic uses the same constant.
- `UIRenderer::renderParagraph`: `renderSize = round(FontSize * fontScale)` and `tyDraw = round(ty)` for `AddText` so scaled lines align to the pixel grid (reduces mushy bitmap-font output).

**Files:** `display.cpp`, `CalibNominal.hpp`, `UIRenderer.cpp`, this log.

**Outcome:** Clean `CAD_OpenGL` build; second pick rejects normals with |dot| < 0.985 (~10° from parallel or opposite).

**Retro:** Loose 0.75 was historical “slab vs centroid” split in span preview; unifying with edge parallelism removes a confusing UX gap. ImGui `AddText` at fractional `FontSize * scale` is a common sharpness footgun — prefer rounding for small pixel fonts.