# CGAL `remesh_planar_patches` STL experiment

## Goal

Optional path to compare CGAL Polygon Mesh Processing planar remesh against the homegrown `MergeCoplanarFaces` workflow (tolerance story, wall slivers, performance).

## How to enable

1. Install CGAL (and its deps, typically GMP/MPFR) so CMake can resolve `CGAL::CGAL` (`find_package(CGAL REQUIRED CONFIG)`).
   - Example (macOS/Homebrew): `brew install cgal`.
2. Configure with `-DCAD_EXPERIMENTAL_CGAL_PLANAR_REMESH=ON`.
3. In `GeometryExperiments.hpp`, set `kUseCgalRemeshPlanarPatchesForStl` to **`true`** (only defined when CGAL is compiled in).
4. Rebuild.

## Behaviour

After STL triangle soup builds a solid, `MergeStlCoplanarMaybe` may call:

- ` CGAL::Polygon_mesh_processing::polygon_soup_to_polygon_mesh`
- `remesh_planar_patches` using `cosine_of_maximum_angle` tied to `kMergeCoplanarNormalDotSlack`
- Rebuild Scene faces/edges from the CGAL mesh (new Scene points added for CGAL-internal vertices).

CGAL’s planar remesh uses a per-patch **triangulation**; it does not replace the need for `Scene::MergeCoplanarFaces` if you want fewer faces / cleaner wireframe on flat regions. When the CGAL experiment succeeds, import **chains** `MergeCoplanarFaces` afterward.

On any failure (`LOG_WARN`), import falls back to `Scene::MergeCoplanarFaces` on the original solid.

## Licensing

CGAL is LGPL/GPL depending on linking mode — legal review required before distributing binaries with this flag on.

## Outcome / follow-ups

- Default build path unchanged (CGAL off).
- Tune parameters (named CGAL vs homegrown knobs), compare `session_log.json` `stl_merge_diagnostics` and timings.

### 2026-05-02 — `kUseCgalRemeshPlanarPatchesForStl` default

- Leave **`false`** for normal STL work (merge-only after import); validated on large tessellated models. Set **`true`** only when comparing CGAL remesh behaviour.

### 2026-05-02 — Coplanar merge on sliver tessellation

- **Face plane:** `CalculatePlanarData` used only the first two edges of the outer loop. On sliver triangles that cross is unstable, so adjacent coplanar tris could disagree on `normal`/`d` and never pass `findMergePair`. Triangles now use the **largest-area** edge cross; larger loops use **Newell** normal.
- **Plane distance:** `planeTol` was `~1e-7 * diagonal`, often below float-STL quantization. Merge now floors tolerance with `FLT_EPSILON * diagonal` and a small `2e-6 * diagonal` import term (diagnostics use the same helper).

### 2026-05-01 — Configure / CGAL 6.1

- **CMake:** `target_link_libraries(CAD_OpenGL …)` must use one style for the target. Main + Apple blocks now use `PRIVATE` (same as CGAL and WIN32) so `cmake .. -DCAD_EXPERIMENTAL_CGAL_PLANAR_REMESH=ON` configures cleanly.
- **Includes:** Homebrew CGAL 6.1 no longer ships `CGAL/boost/graph/functions.h`; `STLCgalPlanarExperiment.cpp` relies on `CGAL/Surface_mesh.h` (which pulls `graph_traits_Surface_mesh.h`) for BGL-style `vertices` / `faces` / `halfedge` / `next` / `target`.
- **CGAL “Debug” notice:** CGAL warns when `CMAKE_BUILD_TYPE` is not `Release`. With the experiment enabled we default `CGAL_DO_NOT_WARN_ABOUT_CMAKE_BUILD_TYPE` to ON (cache) so routine Debug configures stay quiet; use `-DCMAKE_BUILD_TYPE=Release` for benchmarking, or `cmake -UCGAL_DO_NOT_WARN_ABOUT_CMAKE_BUILD_TYPE ..` then `-DCGAL_DO_NOT_WARN_ABOUT_CMAKE_BUILD_TYPE=OFF` to see CGAL’s reminder again.
