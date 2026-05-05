# Calibrate second pick — parallel face constraint (2026-05-05)

## Idea

Gate Calibrate tool second measurement pick so it only accepts geometry whose **resolved face** is compatible with the first pick, using the same parallelism rule as nominal span (`CalibNominal::SpanBetweenFaces`): `|dot(na, nb)| >= 0.75`, distinct faces. This filters hover and click commit.

## Plan

- Centralize threshold as `CalibrateNominal::kFaceNormalParallelAlignThreshold` and add `NormalsAlignedForCalibPick`.
- Helper `CalibSecondPickAcceptsHit` in `display.cpp` (anonymous namespace) using existing `ResolveCalibFaceForWorkflow`.
- `UpdatePickHover` / `TryCommitCalibrateFacePick`: when point 2 row is active and point 1 has a pick, reject non-matching hits.

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

