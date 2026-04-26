# Architecture: Camera — Orthographic Viewport Navigation

## Executive Summary

The `Camera` class implements a target-centric orthographic camera using a quaternion orientation and a scalar `orthoSize` for zoom. `Display` wraps it with five navigation methods (Orbit, Pan, Zoom, Roll, FrameScene) and a dirty-flag system that defers matrix uploads until the next `Frame()` tick. Input events from `Input` flow through `Display`'s public API into `Camera`'s math, keeping the camera model cleanly isolated from event handling.

**Verdict: Approve**

**Top 3 strengths:**
1. Quaternion-based orientation is singularity-free and produces smooth orbit in all directions, including at the poles — no gimbal lock.
2. Zoom is cursor-anchored: the world point under the cursor stays fixed as ortho size changes, matching the behaviour users expect from industry CAD tools.
3. `FrameBounds` computes tight framing from a world-space AABB in a single call; it is reusable from both automatic (post-import) and interactive (click-to-frame) code paths.

**Top 3 concerns:**
1. All `Camera` members are `public` — any code can mutate `target`, `orientation`, `orthoSize` etc. directly, bypassing `UpdateCamera()` and leaving the renderer with stale matrices.
2. `Display::GetCamera()` returns `Camera` by value — callers that only need to read camera state get a full copy; the only current caller uses the copy to set renderers anyway, so this is a minor inefficiency.
3. The `nearPlane = -100000.0f` / `farPlane = 100000.0f` default is correct for orthographic rendering (depth precision is linear) but the values are magic numbers embedded in the constructor without a named constant.

---

## 1. Requirements & Motivation

### Functional Requirements
- Orbit: rotate the view around a fixed world-space target point in response to mouse drag (e.g. middle button).
- Pan: translate the target and camera together in the camera's local XY plane.
- Zoom: scale the orthographic half-extents, anchoring on the cursor's world position.
- Roll: rotate the camera around its forward axis.
- Frame: fit the camera to a world-space AABB; used after import and click-to-frame on analysis flaws.
- Project / unproject: map screen pixels to world-space points on the target plane (`ScreenToWorld`).
- Resize: update aspect ratio on window resize without disrupting the current view orientation or target.

### Non-Functional Requirements
- Runs in the event/input loop; no per-frame allocations — all operations are pure math on value types (`glm::vec3`, `glm::quat`, `float`).
- Must not introduce visual artifacts (shimmering, pole-flip) during continuous orbit drag.

### Constraints
- Orthographic projection only — no perspective mode is implemented or planned.
- GLM 1.0.2 is the math library; quaternion operations via `glm::angleAxis`, `glm::normalize`, `glm::lookAt`.
- Camera state is owned by `Display` as a value member; no heap allocation.

---

## 2. Solution Description

### Components

| Component | Files | Responsibility |
|-----------|-------|----------------|
| `Camera` | `src/display/rendering/Camera/camera.hpp/.cpp` | Camera math: view/projection matrices, all navigation operations |
| `Display` (navigation methods) | `src/display/display.cpp/.hpp` | Public wrappers that call `Camera` methods then set `cameraDirty = true` |
| `Display::ScreenToWorld` | `src/display/display.cpp` | Unprojects a pixel to a world-space point on the camera's target plane |
| `Input` | `src/input/Input.cpp` | Translates SDL3 events into `Display::Orbit/Pan/Zoom/Roll` calls |

### Data Flow

```
SDL3 Event
    │
    ▼
Input::mouseGestures()
    │  applies sensitivity multiplier
    ▼
Display::Orbit / Pan / Zoom / Roll (float deltas)
    │  calls Camera method, sets cameraDirty = true, renderDirty = true
    ▼
Camera::Orbit / Pan / Zoom / Roll
    │  pure math — mutates orientation / target / orthoSize
    ▼
Display::Frame()  [next tick]
    │  if cameraDirty: renderer.SetCamera(camera); viewportRenderer.SetCamera(camera);
    ▼
ViewportRenderer / Renderer  — upload view + projection matrices to GPU uniform
```

### Key Abstractions

- **Quaternion orientation** (`glm::quat orientation`): represents the camera's rotation without accumulating Euler angle drift. World-space axes are recovered each frame via `orientation * vec3(1,0,0)` etc.
- **`orthoSize`**: half-height of the orthographic frustum in world units. Zoom scales this value; the projection matrix is rebuilt from it every frame.
- **`cameraDirty` flag** (`Display`): defers `SetCamera` calls to the start of `Frame()`, preventing redundant matrix uploads when multiple navigation events arrive in one frame.
- **`FrameBounds(min, max)`**: computes `target` as the AABB centroid; sets `orthoSize = maxDim * 0.6f` and `distance = halfDiag + 10.0f`. Distance has no effect on ortho zoom but positions the camera outside the bounding sphere for correct depth sorting.

### Navigation Math Details

#### Orbit
```
axis = normalize(right * deltaY + up * deltaX)   // perpendicular to drag in screen space
rotation = angleAxis(-magnitude, axis)
orientation = normalize(rotation * orientation)
```
The rotation is applied as a pre-multiplication so the axis is always expressed in camera space, producing intuitive "tumble" behaviour.

#### Pan
```
target -= right * (deltaX * scaleX)
target += up    * (deltaY * scaleY)
```
Scale factors equal `orthoSize * aspectRatio` (X) and `orthoSize` (Y) so pan speed is proportional to the visible world area — no pixel-size recalibration needed when zooming.

When `scroll = false` (right-click drag via `Display::Pan`), a `snapInput` pre-filter axis-aligns the delta when one axis is dominant (ratio ≥ 2:1), producing clean horizontal/vertical pan lanes.

#### Zoom
```
orthoSize *= (1 - delta)
delta_target = targetPoint - target
target += delta_target * (1 - actualZoomFactor)   // cursor-anchored
```
The cursor anchor is achieved by shifting `target` toward the cursor proportionally to the zoom change. `orthoSize` is clamped to [0.001, 10 000].

#### Roll
```
forward  = orientation * vec3(0, 0, -1)
rotation = angleAxis(delta, forward)
orientation = normalize(rotation * orientation)
```

#### FrameBounds
```
target    = (min + max) / 2
maxDim    = max(size.x, size.y, size.z)
orthoSize = maxDim * 0.6
distance  = length(size) / 2 + 10
```

---

## 3. Design Principles

- **SRP**: `Camera` contains only camera math; it has no knowledge of SDL, OpenGL, or the scene graph.
- **DIP** (partial): `Display` wraps `Camera` — renderers receive the camera via `SetCamera(camera)` rather than depending on `Display`.
- **Value semantics**: `Camera` is a plain value type (no virtual methods, no heap ownership). Copying is cheap enough for the occasional `GetCamera()` call and eliminates aliasing concerns.
- **Performance over purity**: All navigation methods are `O(1)` math with no allocations. The dirty-flag pattern avoids redundant `SetCamera` calls on frames with no input.

---

## 4. Alternatives Considered

| Option | Pros | Cons | Verdict |
|--------|------|------|---------|
| **Current: quaternion + orthoSize** | Singularity-free, simple zoom model | Public members allow external mutation | Chosen |
| **Euler angles (yaw/pitch/roll)** | Easy to understand, easy to clamp pitch | Gimbal lock at ±90° pitch, drift from floating-point accumulation | Rejected |
| **Perspective projection** | More natural for large scenes | Depth interpretation changes; ortho is standard for CAD measurement | Not applicable for this use case |
| **Camera-as-abstract-interface** | Enables perspective/ortho switching | Adds virtual dispatch; single camera mode in this app | Deferred |
| **Immediate matrix upload** | No dirty-flag complexity | Redundant uploads when multiple input events arrive in one frame | Rejected for the hot render path |

---

## 5. Technology & Dependencies

### Dependencies Used

| Library | Role |
|---------|------|
| GLM 1.0.2 | `glm::mat4`, `glm::quat`, `glm::lookAt`, `glm::ortho`, `glm::angleAxis`, `glm::normalize` |
| SDL3 (indirect) | `SDL_GetWindowSize` used in `Display::ScreenToWorld`; `SDL_GetMouseState` in `Input` |

No new dependencies are introduced. GLM is already a core project dependency.

### Integration Impact
- `Camera` is a value member of `Display` — no heap allocation, no ownership transfer.
- Renderers (`Renderer`, `ViewportRenderer`) receive the camera via `SetCamera(Camera)` at the start of each `Frame()` — they do not hold a pointer to `Camera`.
- `Input` calls only `Display` public methods; it has no direct access to `Camera`.

---

## 6. Tradeoffs

| Decision | Alternative Considered | Chosen Approach | Rationale |
|----------|----------------------|-----------------|-----------|
| All members `public` | Private members with accessors | Public fields | Convenience during development; renderers read several fields in `SetCamera`; reduces boilerplate |
| `cameraDirty` deferred upload | Immediate upload in each nav call | Deferred via dirty flag | Avoids redundant GPU uploads when multiple events arrive per frame |
| `snapInput` axis-alignment in `Display::Pan` | No snapping | Snap when one axis is dominant | Prevents diagonal drift during intentional horizontal/vertical pan |
| `nearPlane = -100000` / `farPlane = 100000` | Smaller symmetric range | ±100 000 | Ensures axes grid (±10 000 units) and arbitrarily large models are never near-clipped; linear ortho depth precision remains ample |
| `distance` retained in ortho | Remove `distance` (irrelevant for ortho) | Kept | `GetPosition()` is used by `GetViewMatrix`; retaining distance keeps the camera outside the model for correct depth sorting in the depth buffer |

---

## 7. Best Practices Compliance

### Conforming
- No heap allocation in any `Camera` method — all math operates on value-type members.
- `orientation` is re-normalized after every rotation to prevent quaternion drift accumulation.
- `orthoSize` is clamped on every zoom to prevent degenerate projections.
- `FrameBounds` padding (`+10` for distance, `*0.6` for orthoSize) ensures the model is never clipped immediately after framing.
- `SetAspectRatio` in `Display` pushes updated matrices directly to renderers before returning, ensuring the first resize-triggered `Render()` uses the correct projection.

### Non-Conforming

| Issue | Severity | Remediation |
|-------|----------|-------------|
| **All `Camera` members are `public`** | Minor | Make navigation-relevant fields private; expose read-only accessors (`GetOrthoSize()`, `GetOrientation()`); keep setters internal. Direct mutation currently bypasses `cameraDirty` — e.g., `camera.target = x` skips `UpdateCamera()`. |
| **Magic constants in constructor** (`nearPlane = -100000`, `farPlane = 100000`, `orthoSize = 2.5`, `distance = 5`) | Minor | Promote to `static constexpr` members or a `CameraDefaults` struct with inline documentation |
| **`GetCamera()` returns by value** | Minor | Return `const Camera&` for read-only access; the only real caller (`Display::SetAspectRatio`) already has direct member access |
| **`ScreenToWorld` is on `Display`, not `Camera`** | Minor | The unproject logic only uses camera state and window size — moving it to `Camera::ScreenToWorld(ndcX, ndcY)` would improve cohesion and testability |

---

## 8. Risks & Future-Proofing

### Risks
- **Public member mutation**: Code outside `Display` that writes to `camera.target` or `camera.orientation` directly will not set `cameraDirty`, causing the renderer to show a stale view until the next navigation event. This is a latent bug risk as the codebase grows.
- **Single projection mode**: If perspective mode is added later, the current architecture requires either extending `Camera` with a mode enum (and branching in `GetProjectionMatrix`) or replacing it with a polymorphic interface — both are moderate refactors.

### Future Considerations
- **Perspective mode**: `GetProjectionMatrix` could branch on a `bool perspective` flag; `Zoom` would need to adjust `fov` instead of `orthoSize` in that mode.
- **Camera presets / named views**: Standard isometric views (Top, Front, Right, Isometric) are a common CAD navigation feature; `FrameBounds` plus a fixed orientation quaternion would suffice.
- **Animated transitions**: Smooth camera interpolation (e.g., on `FrameBounds`) could be added by storing a target camera state and lerping each frame — no structural changes needed.
- **Encapsulation**: Making members private is a low-risk refactor that prevents the `cameraDirty` bypass problem and is recommended before the class gains more callers.

---

## 9. Recommendations

### Should-Have
- Make `Camera` members private; expose `GetOrthoSize()`, `GetOrientation()`, `GetTarget()`, `GetAspectRatio()` as `const` accessors. The only external mutators needed are the five navigation methods already present plus `SetTarget`, `SetDistance`, `SetAspectRatio`.
- Replace magic constants in the `Camera` constructor with `static constexpr` members (e.g., `DEFAULT_DISTANCE`, `DEFAULT_ORTHO_SIZE`, `DEPTH_RANGE`).

### Nice-to-Have
- Move `ScreenToWorld` from `Display` into `Camera` — it depends only on camera state and is generally useful (e.g., for hit-testing in future tools).
- Change `Display::GetCamera()` to return `const Camera&` to avoid the unnecessary copy.
- Add named-view presets (Top, Front, Right, Isometric) — each is a one-liner: `FrameBounds(min, max)` + set `orientation` to the corresponding quaternion.
