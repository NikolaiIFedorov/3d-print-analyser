---
name: cad-naming-style
description: >-
  Naming and consistency conventions for CAD_OpenGL: camelCase, PascalCase,
  qualifying prefixes, avoiding redundant context. Use when naming symbols,
  reviewing diffs for style drift, or aligning new code with existing patterns.
---

# CAD_OpenGL — Naming and consistency

Naming *is* project-specific — canonical detail: [practices/project_practices.md](../../../practices/project_practices.md) → Naming. Consistency below is general practice (canonical version: the user's global `global_practices.md`, outside this repo).

## Naming

- Concise names, qualified only enough for local clarity.
- Put the qualifier first: `patchIndices`, `wireframeIndices`, `surfaceNormals`.
- Inside a type (e.g. `Wireframe`), prefer `indices` over `wireframeIndices` — drop redundant type context.
- **Variables and functions:** camelCase. **Types and classes:** PascalCase.

## Consistency

- Match established patterns for related functionality.
- Deviations need a concrete justification (e.g. measured performance), not convenience.
