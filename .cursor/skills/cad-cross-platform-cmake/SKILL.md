---
name: cad-cross-platform-cmake
description: >-
  CMake-first builds, C++23 portability, std::filesystem, GLFW/GLAD usage, and
  dependency layout for CAD_OpenGL. Use when editing CMakeLists, paths, assets,
  third-party includes, or platform-sensitive code.
---

# CAD_OpenGL — Cross-platform and build

Canonical detail: [practices/best_practices.md](../../../practices/best_practices.md) (Cross-Platform Compatibility).

## Build (CMake)

- CMake is the single source of truth; no IDE-only project files as the authority.
- Declare minimum CMake and compiler versions in `CMakeLists.txt`.
- Prefer `target_*` properties and generator expressions over global `add_compile_options`.

## Compiler and language

- Target C++23; avoid vendor extensions unless behind a macro wrapper.
- Aim for clean warnings (`-Wall -Wextra`; CI may use `-Werror` / `/WX`).
- No UB/implementation-defined reliance (strict aliasing, signed overflow, uninitialized reads).

## Paths and assets

- Paths: `std::filesystem`; never hard-code `\` vs `/`.
- Forward slashes in CMake paths.
- Load shaders/textures relative to executable or configurable base — not absolute paths.

## Windowing and OpenGL

- Windowing through GLFW only — not raw platform APIs.
- Load GL entry points via GLAD; do not assume link-time GL symbols.
- Query capabilities at runtime before relying on extensions beyond core profile.

## Dependencies

- Third-party headers under `include/` with version in the directory name when applicable.
- Document/link system libs in CMake (e.g. Cocoa, IOKit on macOS).
- Prefer header-only or bundled sources to reduce external build deps.

## Types

- Fixed-width integers (`int32_t`, etc.) when layout matters (files, GPU buffers).
- `size_t` for container sizes and loops tied to them.
- Do not assume `sizeof(long)` or pointer width.
