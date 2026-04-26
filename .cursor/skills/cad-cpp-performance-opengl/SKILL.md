---
name: cad-cpp-performance-opengl
description: >-
  Memory, CPU, data layout, and OpenGL rendering performance for CAD_OpenGL:
  batching, VAOs/VBOs, no per-frame heap in hot paths, profiling first. Use when
  optimizing, writing render/update loops, or reviewing GPU/CPU cost.
---

# CAD_OpenGL — C++ and OpenGL performance

Canonical detail: [practices/best_practices.md](../../../practices/best_practices.md) (Performance Considerations).

## Memory

- Prefer stack/value types when lifetime is bounded.
- `std::unique_ptr` for owning heap; no raw `new`/`delete`.
- `vector::reserve` when size is known or estimable.
- Pass large objects by `const&` or `std::move`.

## Rendering

- Batch draws; minimize GL state changes between draws.
- Use VAOs/VBOs; no immediate mode.
- Upload geometry once; update only when data changes.
- Keep uniform updates out of innermost loops where possible.

## Computation

- Prefer `<algorithm>` over hand-rolled loops when it fits.
- No per-frame heap in render/update — reuse buffers.
- Profile before micro-optimizing (Instruments, etc.).
- Use `constexpr` / compile-time work where it helps (C++23 available).

## Data layout

- Prefer contiguous containers (`std::vector`) over node-based structures in hot paths.
- Consider struct-of-arrays for many entities on hot paths.
