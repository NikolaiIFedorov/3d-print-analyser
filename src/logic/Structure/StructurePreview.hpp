#pragma once

#include <utility>
#include <vector>

#include <glm/glm.hpp>

class Scene;

namespace StructurePreview
{

// Phase A of the structure pivot (see documentation/implementations/structure_face_triangulation_2026-05-11.md)
// removed the retired infill generators (BuildAdjacentFaceMidpoints / BuildInteriorFaceRibs /
// BuildInsetFaceLoops / BuildCenterStruts) and their preview parameter structs. Anonymous-namespace
// helpers in StructurePreview.cpp are kept for reuse when Phase B introduces the face-triangulation
// algorithm (CGAL straight-skeleton inset + corner strip + fillet + vertical extrusion).

} // namespace StructurePreview
