# Best Practices

Guidelines for writing code in this project.

---

## SOLID Principles

> **Performance takes priority.** When applying SOLID principles introduces a noticeable performance cost (extra indirection, virtual dispatch in hot paths, unnecessary allocations), prioritize performance over architectural purity.

### Single Responsibility Principle (SRP)
- Each class should have one reason to change.
- Separate rendering, input handling, scene management, and business logic into distinct classes.
- Keep shaders, geometry, and display concerns in their own modules.

### Open/Closed Principle (OCP)
- Design classes to be extended without modifying existing code.
- Use abstract base classes or interfaces for renderable objects, input handlers, and geometry types.
- Prefer virtual methods or templates over switch/if-else chains on type tags.

### Liskov Substitution Principle (LSP)
- Derived classes must be usable wherever their base class is expected without altering correctness.
- Do not override methods in ways that violate the base class contract.
- Avoid narrowing preconditions or widening postconditions in subclasses.

### Interface Segregation Principle (ISP)
- Keep interfaces small and focused — clients should not depend on methods they don't use.
- Split fat interfaces (e.g., a single `IRenderable` with both 2D and 3D methods) into smaller ones.

### Dependency Inversion Principle (DIP)
- High-level modules (scene logic, analysis) should not depend on low-level modules (OpenGL calls, GLFW).
- Depend on abstractions (interfaces/abstract classes), not concrete implementations.
- Inject dependencies through constructors or factory functions rather than creating them internally.

---

## Performance Considerations

### Memory
- Prefer stack allocation and value types over heap allocation when object lifetime is bounded.
- Use `std::unique_ptr` for owning heap resources; avoid raw `new`/`delete`.
- Reserve container capacity (`std::vector::reserve`) when the size is known or estimable.
- Avoid unnecessary copies — pass large objects by `const&` or move them with `std::move`.

### Rendering
- Batch draw calls — minimize OpenGL state changes between draws.
- Use Vertex Array Objects (VAOs) and Vertex Buffer Objects (VBOs); avoid immediate-mode rendering.
- Upload geometry to the GPU once; update only when the data actually changes.
- Keep shader uniform updates outside tight loops where possible.

### Computation
- Prefer algorithms from `<algorithm>` over hand-written loops.
- Avoid per-frame heap allocations in the render/update loop — reuse buffers.
- Profile before optimizing; use instruments or similar tools to find real bottlenecks.
- Use `constexpr` and compile-time evaluation where applicable (C++23 features are available).

### Data Layout
- Prefer contiguous containers (`std::vector`) over node-based ones (`std::list`, `std::map`) for cache friendliness.
- Consider data-oriented design (struct-of-arrays) for hot paths with many entities.

---

## Cross-Platform Compatibility

### Build System
- Use CMake as the single source of truth for builds — do not rely on IDE-specific project files.
- Specify minimum CMake and compiler versions in `CMakeLists.txt`.
- Use generator expressions and target properties instead of global flags (`target_compile_options` over `add_compile_options`).

### Compiler & Language
- Target C++23 (`-std=c++23`) but avoid compiler-specific extensions (`__attribute__`, `__declspec`) unless wrapped behind a macro.
- Treat warnings as errors in CI (`-Werror` / `/WX`). At minimum, compile with `-Wall -Wextra`.
- Do not rely on undefined or implementation-defined behavior (strict aliasing, signed overflow, uninitialized reads).

### File System & Paths
- Use `std::filesystem` for path manipulation — never hard-code path separators.
- Use forward slashes in CMake paths; they work on all platforms.
- Load assets (shaders, textures) relative to the executable or a configurable base directory, not absolute paths.

### Windowing & Graphics
- Access windowing through GLFW — do not call platform-native APIs directly.
- Load OpenGL functions through GLAD; do not assume any function pointer is available at link time.
- Query OpenGL capabilities at runtime before using extensions or features beyond the core profile.

### Dependencies
- Keep third-party libraries in `include/` with their version number in the directory name.
- Document required system libraries (e.g., Cocoa, IOKit on macOS) in CMake so they resolve automatically.
- Prefer header-only or source-bundled libraries to reduce external build dependencies.

### Types & Portability
- Use fixed-width types (`int32_t`, `uint32_t`) when binary layout matters (file formats, GPU buffers).
- Use `size_t` for container sizes and loop indices tied to container sizes.
- Do not assume `sizeof(long)` or pointer size — they differ between platforms.

---

## Pre-Implementation Critique

- Before writing code, briefly question the approach: what edge cases does it miss? What existing behavior could it silently break?
- When replacing or unifying existing code, diff the old paths side-by-side and list every behavioral difference. Each difference needs an explicit decision: keep, drop, or generalize.
- Consider at least one alternative approach. If the chosen approach has no clear advantage over the alternative, it may not be the right one.
- This is not about blocking progress — a few minutes of scrutiny prevents hours of debugging.

---

## DRY (Don't Repeat Yourself)

- When the same computation or pattern appears in 2+ places, extract it into a shared helper.
- Copy-pasting a block and tweaking it is a signal to extract a function parameterized by the differences.
- For tree or hierarchical data structures, use recursive traversal instead of hardcoding a fixed depth. Hardcoded depth leads to duplicated per-level logic that silently diverges.
- When unifying special-cased code paths into one, explicitly list every distinct behavior the old paths handled. Missing a case (e.g., a header splitter that was implicit in a type check) causes silent regressions.

---

## Post-Implementation Review

- After a feature or change compiles and works correctly, re-read the diff before considering it done.
- Look for: duplicated blocks that can be extracted, magic numbers that should be constants, inconsistent naming, and leftover dead code.
- If the new code introduces a pattern that already exists elsewhere in a different form, unify both into one shared implementation.
- This step is cheap when the code is fresh in mind and expensive when revisited months later.

---

## Consistency

- When a pattern or approach is established for one piece of functionality, related code should follow the same approach.
- Deviations are acceptable when justified by a concrete reason (e.g., performance), not by convenience.

---

## Naming

- Prefer concise names, qualified just enough to disambiguate within the surrounding context.
- Place the qualifying part at the beginning of the name (e.g., `patchIndices`, `wireframeIndices`, `surfaceNormals`).
- Avoid redundant context — if a variable lives inside a `Wireframe` class, name it `indices` rather than `wireframeIndices`.
- Use camelCase for variables and functions, PascalCase for types and classes.
