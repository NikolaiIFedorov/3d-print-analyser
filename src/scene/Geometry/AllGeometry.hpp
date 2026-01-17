#pragma once

#include "Point.hpp"
#include "Edge.hpp"
#include "Curve.hpp"
#include "Face.hpp"
#include "Solid.hpp"

using FormPtr = std::variant<Point *, Edge *, Curve *, Face *, Solid *>;

#include "Geometry.hpp"