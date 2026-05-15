# Compound v1 — multi-solid group (2026-05-14)

## Goal

Introduce a **first-class** scene object that references **several existing `Solid`** instances (non-owning), as agreed for v1 before UX (tree, selection propagation).

## Design

- **`Compound`** (`src/scene/Geometry/Compound.hpp`): `std::vector<Solid *> solids` — pointers must refer to `Solid`s in the **same** `Scene::solids` deque; compound does not allocate solids.
- **`Scene::compounds`**: `std::deque<Compound>`; **`Scene::CreateCompound(std::vector<Solid *>)`** dedupes, drops nulls and pointers not in this scene, returns `nullptr` if nothing remains.
- **`FormPtr`**: extended with `Compound *` in `AllGeometry.hpp` for future selection / tools (still largely unused elsewhere).
- **`Scene::Clone`**: builds `solidMap`, then remaps each compound’s members to cloned solids and recreates compounds on the clone.

## Not in this change

- No UI, selection, or import path calling `CreateCompound` yet.
- No removal API for compounds (erase from deque) — add when needed.

## Outcome

Shipped as above; `cmake --build build --target CAD_OpenGL` succeeds.
