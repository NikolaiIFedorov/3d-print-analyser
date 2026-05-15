# Compound v1 — multi-solid group (2026-05-14)

## Goal

Introduce a **first-class** scene object that references **several existing `Solid`** instances (non-owning), as agreed for v1 before UX (tree, selection propagation).

## Design

- **`Compound`** (`src/scene/Geometry/Compound.hpp`): `std::vector<Solid *> solids` — pointers must refer to `Solid`s in the **same** `Scene::solids` deque; compound does not allocate solids.
- **`Scene::compounds`**: `std::deque<Compound>`; **`Scene::CreateCompound(std::vector<Solid *>)`** dedupes, drops nulls and pointers not in this scene, returns `nullptr` if nothing remains.
- **`FormPtr`**: extended with `Compound *` in `AllGeometry.hpp` for future selection / tools (still largely unused elsewhere).
- **`Scene::Clone`**: builds `solidMap`, then remaps each compound’s members to cloned solids and recreates compounds on the clone.

## Not in this change

- No **explicit** UI tree row or click-to-select on `Compound` yet; `TryCreateCompoundWrappingAllSolidsIfNone` runs automatically on import attach when applicable.
- No removal API for compounds (erase from deque) — add when needed.

---

## Update (same file): import + dirty propagation

- **`Scene::TryCreateCompoundWrappingAllSolidsIfNone()`** — after a successful import attach, if the new scene has **≥2 solids** and **no compounds** yet, creates one compound listing **all** solids in deque order.
- **`Scene::InsertSolidWithCompoundMembers`** — used by **`Display::MarkGeometryDirtySolid`** so incremental mesh rebuild / analysis invalidation touches **every solid in the same compound** as the dirtied solid (proxy for “select group” until explicit selection state exists).


## Outcome

Shipped as above; `cmake --build build --target CAD_OpenGL` succeeds.
