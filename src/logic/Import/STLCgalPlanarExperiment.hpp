#pragma once

class Scene;
struct Solid;
struct MergeCoplanarDiagnostics;

/// Optional CGAL Polygon_mesh_processing::remesh_planar_patches spike (CMake + CGAL install required).
namespace STLCgalPlanarExperiment
{
/// Builds a CGAL Surface_mesh from `solid`'s triangular faces, remeshes planar patches, replaces `solid`'s face list with new topology (still triangulated patches).
/// On failure returns `false` — caller runs `Scene::MergeCoplanarFaces` on the original soup. On success, caller still runs `MergeCoplanarFaces` on the rebuilt mesh.
bool TryRemeshPlanarPatchesReplacingSolidFaces(Scene *scene, Solid *solid, MergeCoplanarDiagnostics *diagOut);
}
