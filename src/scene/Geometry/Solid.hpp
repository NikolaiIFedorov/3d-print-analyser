#pragma once

#include <vector>
#include "GeometryValidity.hpp"
#include "AllGeometry.hpp"

struct Solid
{
    Solid() = default;

    std::vector<Face *> faces;

    /// Last `GeometryValidity::EvaluateAppInvalidTagsForSolid` result for this solid.
    GeometryValidity::AppInvalidTag cachedAppInvalidGeometryTags = GeometryValidity::AppInvalidTag::None;
    /// When false, `cachedAppInvalidGeometryTags` is stale (topology changed) until refresh.
    bool cachedAppInvalidGeometryTagsFresh = false;

    Solid(Face *face) : faces({face}) {}
};