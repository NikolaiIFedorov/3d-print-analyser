#pragma once

#include <vector>

struct Solid;

/// v1: groups several **existing** scene solids for selection / hierarchy UX. Does not own
/// `Solid` storage — members live in `Scene::solids` and must outlive the compound or be removed
/// from the compound before the solid is destroyed.
struct Compound
{
    std::vector<Solid *> solids;
};
