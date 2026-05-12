# Quit hang: OpenGL second shutdown after context destroyed

## Problem

App appeared to **freeze on quit**, often after CGAL stderr noise (structure carve / mesh paths). `[shutdown]` breadcrumbs could stop after `Display::Shutdown` completed, implicating **static teardown** of `Display` / renderers.

## Cause

`Display::Shutdown()` calls `renderer.Shutdown()` → `OpenGLRenderer::Shutdown()` while the GL context is still current — correct.

`OpenGLRenderer::Shutdown()` deleted the **main triangle and line** VAO/VBO/IBO but **did not zero** those GLuint names. `~OpenGLRenderer()` always calls `Shutdown()` again.

After `SDL_GL_DestroyContext`, static destruction runs `~Display` → `~SceneRenderer` → `~OpenGLRenderer` → second `Shutdown()` with **no current context**, still seeing **non-zero** triangle/line handles → **invalid `glDelete*`** / driver UB — consistent with a hang or long stall.

## Approach

1. Zero triangle and line GL names and counts after their deletes in `OpenGLRenderer::Shutdown()` (idempotent second call; no GL work when handles are 0).
2. Set `glContext` and `window` to `nullptr` in `Display::Shutdown()` after destroy (avoid dangling pointers if anything reads them during teardown).

## Files

- `src/display/rendering/OpenGL/OpenGLRenderer.cpp`
- `src/display/display.cpp`

## Outcome

Clean rebuild of `CAD_OpenGL`. Second `Shutdown()` is a no-op for mesh buffers; shaders already zero `programID` in `OpenGLShader::Delete`.

## Mini retro

- **Worked:** Small, localized fix; matches how other renderers already zero handles after delete.
- **Follow-up:** If hangs remain, next suspects are other GL-owning types or global order vs `SDL_Quit`.
