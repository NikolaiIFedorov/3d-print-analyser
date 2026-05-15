#pragma once

#include "Point.hpp"
#include "Edge.hpp"
#include "Curve.hpp"
#include "Surface.hpp"
#include "Face.hpp"
#include "Solid.hpp"
#include "Compound.hpp"

using FormPtr = std::variant<Point *, Edge *, Curve *, Face *, Solid *, Compound *>;

#include "Geometry.hpp"