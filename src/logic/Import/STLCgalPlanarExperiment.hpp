#pragma once

class Scene;
struct Solid;
struct MergeCoplanarDiagnostics;

/// Optional CGAL Polygon_mesh_processing::remesh_planar_patches spike (CMake + CGAL install required).
namespace STLCgalPlanarExperiment
{
/// Builds a CGAL Surface_mesh from `solid`'s triangular faces, remeshes planar patches, replaces `solid`'s face list with new planar/triangle topology.
/// On failure (`pm_out` empty, CGAL errors, degenerate polygons) returns `false` — caller falls back to `Scene::MergeCoplanarFaces`.
bool TryRemeshPlanarPatchesReplacingSolidFaces(Scene *scene, Solid *solid, MergeCoplanarDiagnostics *diagOut);
}
