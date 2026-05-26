#pragma once

#include <string>

class Scene;
struct Solid;

/// Imports a STEP file using OpenCASCADE, resolves booleans, tessellates the result,
/// and bridges it into the existing flat-triangle Scene topology.
/// Returns the created Solid, or nullptr on failure.
Solid *ImportSTEP(Scene *scene, const std::string &filepath);
