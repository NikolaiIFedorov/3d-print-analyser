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

On any failure (`LOG_WARN`), import falls back to `Scene::MergeCoplanarFaces`.

## Licensing

CGAL is LGPL/GPL depending on linking mode — legal review required before distributing binaries with this flag on.

## Outcome / follow-ups

- Default build path unchanged (CGAL off).
- Tune parameters (named CGAL vs homegrown knobs), compare `session_log.json` `stl_merge_diagnostics` and timings.

### 2026-05-01 — Configure / CGAL 6.1

- **CMake:** `target_link_libraries(CAD_OpenGL …)` must use one style for the target. Main + Apple blocks now use `PRIVATE` (same as CGAL and WIN32) so `cmake .. -DCAD_EXPERIMENTAL_CGAL_PLANAR_REMESH=ON` configures cleanly.
- **Includes:** Homebrew CGAL 6.1 no longer ships `CGAL/boost/graph/functions.h`; `STLCgalPlanarExperiment.cpp` relies on `CGAL/Surface_mesh.h` (which pulls `graph_traits_Surface_mesh.h`) for BGL-style `vertices` / `faces` / `halfedge` / `next` / `target`.
- **CGAL “Debug” notice:** CGAL warns when `CMAKE_BUILD_TYPE` is not `Release`. With the experiment enabled we default `CGAL_DO_NOT_WARN_ABOUT_CMAKE_BUILD_TYPE` to ON (cache) so routine Debug configures stay quiet; use `-DCMAKE_BUILD_TYPE=Release` for benchmarking, or `cmake -UCGAL_DO_NOT_WARN_ABOUT_CMAKE_BUILD_TYPE ..` then `-DCGAL_DO_NOT_WARN_ABOUT_CMAKE_BUILD_TYPE=OFF` to see CGAL’s reminder again.
