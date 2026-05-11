#include "Input.hpp"
#include "imgui.h"
#include "imgui_impl_sdl3.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_touch.h>

#include <algorithm>
#include <cmath>

namespace
{

/// True if any SDL touch device currently reports ≥1 finger down (physical contact on trackpad / screen).
static bool anySdlTouchFingerDown()
{
    int nDevices = 0;
    SDL_TouchID *devices = SDL_GetTouchDevices(&nDevices);
    if (devices == nullptr)
        return false;
    for (int i = 0; i < nDevices; ++i)
    {
        int nFingers = 0;
        SDL_Finger **fgs = SDL_GetTouchFingers(devices[i], &nFingers);
        if (fgs != nullptr)
            SDL_free(fgs);
        if (nFingers > 0)
        {
            SDL_free(devices);
            return true;
        }
    }
    SDL_free(devices);
    return false;
}

/// True if SDL’s touch layer sees ≥2 fingers (e.g. trackpad; often still true when duplicate scroll arrives).
static bool sdlHasMultiTouchContact()
{
    int nDevices = 0;
    SDL_TouchID *devices = SDL_GetTouchDevices(&nDevices);
    if (devices == nullptr)
    {
        return false;
    }
    for (int i = 0; i < nDevices; ++i)
    {
        int nFingers = 0;
        SDL_Finger **fgs = SDL_GetTouchFingers(devices[i], &nFingers);
        if (fgs != nullptr)
        {
            SDL_free(fgs);
        }
        if (nFingers >= 2)
        {
            SDL_free(devices);
            return true;
        }
    }
    SDL_free(devices);
    return false;
}

}  // namespace

Input::Input(Display *display)
    : display(display)
{
}

void Input::NotifySdlEventQueueFlushed()
{
    clearTouchState();
    multiFingerSeenThisEventDrain = false;
    touchPanAppliedThisEventDrain = false;
    const Uint64 until = SDL_GetTicks() + kWheelSuppressAfterMultiTouchLiftMs;
    suppressCameraWheelUntilMs = std::max(suppressCameraWheelUntilMs, until);
}

void Input::clearTouchState()
{
    activeTouches.clear();
    fingerArrivalOrder.clear();
    touchPanHaveCentroidAnchor = false;
    touchPanPrevCentroidX = 0.0f;
    touchPanPrevCentroidY = 0.0f;
}

void Input::beginTouchPanAccumForFrame()
{
    multiFingerSeenThisEventDrain = false;
    touchPanAppliedThisEventDrain = false;
    touchPanHaveCentroidAnchor = false;
    if (activeTouches.size() >= 2U)
    {
        float sx = 0.0f, sy = 0.0f;
        for (const auto &kv : activeTouches)
        {
            sx += kv.second.x;
            sy += kv.second.y;
        }
        const float inv = 1.0f / static_cast<float>(activeTouches.size());
        touchPanCentroidStartX = sx * inv;
        touchPanCentroidStartY = sy * inv;
        touchPanHaveCentroidAnchor = true;
        touchPanPrevCentroidX = touchPanCentroidStartX;
        touchPanPrevCentroidY = touchPanCentroidStartY;
    }
}

void Input::ensureTouchPanCentroidAnchor()
{
    if (touchPanHaveCentroidAnchor || activeTouches.size() < 2U)
    {
        return;
    }
    float sx = 0.0f, sy = 0.0f;
    for (const auto &kv : activeTouches)
    {
        sx += kv.second.x;
        sy += kv.second.y;
    }
    const float inv = 1.0f / static_cast<float>(activeTouches.size());
    touchPanCentroidStartX = sx * inv;
    touchPanCentroidStartY = sy * inv;
    touchPanHaveCentroidAnchor = true;
    touchPanPrevCentroidX = touchPanCentroidStartX;
    touchPanPrevCentroidY = touchPanCentroidStartY;
}

void Input::tryApplyIncrementalTwoFingerPan()
{
    if (!touchPanHaveCentroidAnchor || activeTouches.size() < 2U)
    {
        return;
    }
    float sx = 0.0f, sy = 0.0f;
    for (const auto &kv : activeTouches)
    {
        sx += kv.second.x;
        sy += kv.second.y;
    }
    const float inv = 1.0f / static_cast<float>(activeTouches.size());
    const float cx = sx * inv;
    const float cy = sy * inv;

    // Shift/Alt + two-finger: explicit orbit/zoom comes from `MOUSE_WHEEL`; only track centroid, do not pan.
    const SDL_Keymod mod = SDL_GetModState();
    if ((mod & SDL_KMOD_ALT) != 0 || (mod & SDL_KMOD_SHIFT) != 0)
    {
        touchPanPrevCentroidX = cx;
        touchPanPrevCentroidY = cy;
        return;
    }

    const float dnx = cx - touchPanPrevCentroidX;
    const float dny = cy - touchPanPrevCentroidY;
    if (std::hypot(dnx, dny) < kTouchDeadzone)
    {
        return;
    }
    int w = 0, h = 0;
    SDL_GetWindowSize(display->GetWindow(), &w, &h);
    if (w > 0 && h > 0)
    {
        ImGuiIO &imguiIo = ImGui::GetIO();
        const float px = cx * static_cast<float>(w);
        const float py = cy * static_cast<float>(h);
        if (imguiIo.WantCaptureMouse || display->HitTestUI(px, py) || display->HitTestImGui(px, py))
        {
            touchPanPrevCentroidX = cx;
            touchPanPrevCentroidY = cy;
            return;
        }
    }
    const float s = display->mouseSensitivity / 30.0f;
    display->Pan(dnx * s, dny * s, true);
    touchPanAppliedThisEventDrain = true;
    touchPanPrevCentroidX = cx;
    touchPanPrevCentroidY = cy;
    const Uint64 until = SDL_GetTicks() + kWheelSuppressAfterPanApplyMs;
    suppressCameraWheelUntilMs = std::max(suppressCameraWheelUntilMs, until);
}

void Input::twoFingerOrMouseBridgePanOrbit(const SDL_Event &event)
{
    const float sm = display->mouseSensitivity / 30.0f;
    if (middleMouseDown)
    {
        display->Orbit(event.tfinger.dx * sm, event.tfinger.dy * sm);
    }
    else
    {
        display->Pan(event.tfinger.dx * sm, event.tfinger.dy * sm, true);
    }
}

void Input::syncWindowRelativeMouseMode()
{
    // Relative mode hides/warps the cursor (good for uninterrupted MMB orbit at screen edges).
    // RMB pan does not need it; retaining it delayed the first pans while the WM adjusted grab.
    const bool want = middleMouseDown;
    SDL_SetWindowRelativeMouseMode(display->GetWindow(), want);
}

bool Input::shouldSuppressRedundantTrackpadScroll(const SDL_Event &event) const
{
    if (event.type != SDL_EVENT_MOUSE_WHEEL)
    {
        return false;
    }
    // Modifier + wheel is an explicit Orbit/Zoom/Roll, not a duplicate of 2-finger trackpad pan.
    const SDL_Keymod mod = SDL_GetModState();
    if ((mod & SDL_KMOD_ALT) || (mod & SDL_KMOD_SHIFT) || (mod & SDL_KMOD_CTRL))
    {
        return false;
    }
    // Two-finger trackpad: pan comes from FINGER_MOTION centroid; scroll is also emitted as wheel with
    // inconsistent `which` across SDL/OS paths. Suppress unmodified zoom/roll for any wheel device while
    // a multi-finger gesture is active (trade-off: external mouse wheel may be ignored until contacts drop).
    const bool multifingerGesture = activeTouches.size() >= 2U || sdlHasMultiTouchContact()
                                    || multiFingerSeenThisEventDrain || touchPanAppliedThisEventDrain;
    if (multifingerGesture)
    {
        return true;
    }
    return false;
}

void Input::mouseGestures(const SDL_Event &event)
{
    ImGuiIO &io = ImGui::GetIO();
    switch (event.type)
    {
    case SDL_EVENT_MOUSE_WHEEL:
    {
        float mx, my;
        SDL_GetMouseState(&mx, &my);
        if (io.WantCaptureMouse || display->HitTestUI(mx, my) || display->HitTestImGui(mx, my))
            break;
        SDL_Keymod mod = SDL_GetModState();
        const bool hasModifier = (mod & SDL_KMOD_ALT) || (mod & SDL_KMOD_SHIFT) || (mod & SDL_KMOD_CTRL);
        const Uint64 now = SDL_GetTicks();
        if (now < suppressCameraWheelUntilMs && !hasModifier)
            break;
        // Trackpad scroll synthesized as wheel with no finger contact is usually inertia after a gesture lift.
        if (!hasModifier && event.wheel.which == SDL_TOUCH_MOUSEID && !anySdlTouchFingerDown())
            break;
        float x = event.wheel.x;
        float y = event.wheel.y;
        if (hasModifier)
        {
            if (mod & SDL_KMOD_ALT)
            {
                if (!pendingWheelOrbitSnap)
                    display->BeginOrbitSnapGesture();
                display->Orbit(x * 0.05f, y * 0.05f);
                pendingWheelOrbitSnap = true;
            }
            else if (mod & SDL_KMOD_CTRL)
            {
                if (x != 0.0f)
                    display->Roll(x * 0.05f);
            }
            else if (mod & SDL_KMOD_SHIFT)
            {
                float val = (y != 0.0f) ? y : x;
                float mx, my;
                SDL_GetMouseState(&mx, &my);
                glm::vec3 cursorWorld = display->ScreenToWorld(mx, my);
                display->Zoom(val * 0.05f, cursorWorld);
            }
        }
        else
        {
            if (x != 0.0f)
                display->Roll(x * 0.05f);
            if (y != 0.0f)
            {
                float mx, my;
                SDL_GetMouseState(&mx, &my);
                glm::vec3 cursorWorld = display->ScreenToWorld(mx, my);
                display->Zoom(y * 0.05f, cursorWorld);
            }
        }
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (event.button.button == SDL_BUTTON_LEFT)
        {
            display->TryCommitCalibrateFacePick(static_cast<float>(event.button.x),
                                                static_cast<float>(event.button.y));
            display->TryCommitStructureFacePick(static_cast<float>(event.button.x),
                                                static_cast<float>(event.button.y));
        }
        else if (event.button.button == SDL_BUTTON_RIGHT)
        {
            const float bx = static_cast<float>(event.button.x);
            const float by = static_cast<float>(event.button.y);
            if (!io.WantCaptureMouse && !display->HitTestUI(bx, by) && !display->HitTestImGui(bx, by))
            {
                rightMouseDown = true;
                syncWindowRelativeMouseMode();
            }
        }
        else if (event.button.button == SDL_BUTTON_MIDDLE)
        {
            const float bx = static_cast<float>(event.button.x);
            const float by = static_cast<float>(event.button.y);
            if (!io.WantCaptureMouse && !display->HitTestUI(bx, by) && !display->HitTestImGui(bx, by))
            {
                middleMouseDown = true;
                display->BeginOrbitSnapGesture();
                syncWindowRelativeMouseMode();
            }
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (event.button.button == SDL_BUTTON_LEFT)
        {
            // UI interaction will be handled by ImGui later
        }
        else if (event.button.button == SDL_BUTTON_RIGHT)
        {
            rightMouseDown = false;
            suppressCameraWheelUntilMs =
                std::max(suppressCameraWheelUntilMs, SDL_GetTicks() + kWheelSuppressAfterPanApplyMs);
            syncWindowRelativeMouseMode();
            display->UpdatePickHover(static_cast<float>(event.button.x), static_cast<float>(event.button.y));
        }
        else if (event.button.button == SDL_BUTTON_MIDDLE)
        {
            const bool wasOrbiting = middleMouseDown;
            middleMouseDown = false;
            suppressCameraWheelUntilMs =
                std::max(suppressCameraWheelUntilMs, SDL_GetTicks() + kWheelSuppressAfterPanApplyMs);
            syncWindowRelativeMouseMode();
            if (wasOrbiting)
                display->FinishOrbitSnap();
            display->UpdatePickHover(static_cast<float>(event.button.x), static_cast<float>(event.button.y));
        }
        break;
    case SDL_EVENT_MOUSE_MOTION:
    {
        // While RMB/MMB orbit or pan is active, ignore UI/ImGui under the cursor so a drag that
        // started on the viewport is not cancelled when the pointer crosses panels (WantCaptureMouse
        // and hit-tests still gate starting a new drag on mouse-down).
        float mx, my;
        SDL_GetMouseState(&mx, &my);
        const bool viewportNavDrag = middleMouseDown || rightMouseDown;
        const bool blockNav = !viewportNavDrag &&
                              (io.WantCaptureMouse || display->HitTestUI(mx, my) || display->HitTestImGui(mx, my));
        if (middleMouseDown && !blockNav)
            display->Orbit(event.motion.xrel * display->mouseSensitivity * 1e-4f,
                           event.motion.yrel * display->mouseSensitivity * 1e-4f);
        else if (rightMouseDown && !blockNav)
            display->Pan(event.motion.xrel * display->mouseSensitivity * 1e-4f,
                         event.motion.yrel * display->mouseSensitivity * 1e-4f, false);
        else
            display->UpdatePickHover(static_cast<float>(event.motion.x), static_cast<float>(event.motion.y));
        break;
    }
    default:
        break;
    }
}

bool Input::processEvent(const SDL_Event &event)
{
    ImGui_ImplSDL3_ProcessEvent(&event);
    ImGuiIO &io = ImGui::GetIO();

    switch (event.type)
    {
    case SDL_EVENT_QUIT:
        return false;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        if (middleMouseDown)
            display->FinishOrbitSnap();
        clearTouchState();
        if (rightMouseDown || middleMouseDown)
        {
            rightMouseDown = false;
            middleMouseDown = false;
            syncWindowRelativeMouseMode();
        }
        break;
    case SDL_EVENT_FINGER_CANCELED:
        // Always sync hardware touch model; skipping here desyncs nContacts vs real fingers.
        if (activeTouches.size() >= 2U)
        {
            suppressCameraWheelUntilMs = std::max(
                suppressCameraWheelUntilMs, SDL_GetTicks() + kWheelSuppressAfterMultiTouchLiftMs);
        }
        if (middleMouseDown)
            display->FinishOrbitSnap();
        clearTouchState();
        break;
    case SDL_EVENT_FINGER_DOWN:
    {
        const SDL_TouchFingerEvent &tf = event.tfinger;
        if (std::find(fingerArrivalOrder.begin(), fingerArrivalOrder.end(), tf.fingerID) == fingerArrivalOrder.end())
        {
            fingerArrivalOrder.push_back(tf.fingerID);
        }
        activeTouches[tf.fingerID] = Touch{0.0f, 0.0f, tf.x, tf.y};
        if (activeTouches.size() >= 2U || sdlHasMultiTouchContact())
            multiFingerSeenThisEventDrain = true;
        ensureTouchPanCentroidAnchor();
        break;
    }
    case SDL_EVENT_FINGER_UP:
    {
        const size_t before = activeTouches.size();
        const SDL_TouchFingerEvent &tf = event.tfinger;
        activeTouches.erase(tf.fingerID);
        fingerArrivalOrder.erase(
            std::remove(fingerArrivalOrder.begin(), fingerArrivalOrder.end(), tf.fingerID),
            fingerArrivalOrder.end());
        if (before >= 2U && activeTouches.size() < 2U)
        {
            suppressCameraWheelUntilMs = std::max(
                suppressCameraWheelUntilMs, SDL_GetTicks() + kWheelSuppressAfterMultiTouchLiftMs);
        }
        if (activeTouches.size() >= 2U || sdlHasMultiTouchContact())
            multiFingerSeenThisEventDrain = true;
        break;
    }
    case SDL_EVENT_FINGER_MOTION:
    {
        if (io.WantCaptureMouse && !rightMouseDown && !middleMouseDown)
        {
            break;
        }
        const SDL_TouchFingerEvent &tf = event.tfinger;
        if (activeTouches.find(tf.fingerID) == activeTouches.end())
        {
            if (std::find(fingerArrivalOrder.begin(), fingerArrivalOrder.end(), tf.fingerID) == fingerArrivalOrder.end())
            {
                fingerArrivalOrder.push_back(tf.fingerID);
            }
        }
        activeTouches[tf.fingerID] = Touch{tf.dx, tf.dy, tf.x, tf.y};
        if (activeTouches.size() >= 2U || sdlHasMultiTouchContact())
            multiFingerSeenThisEventDrain = true;
        ensureTouchPanCentroidAnchor();

        const size_t nContacts = activeTouches.size();
        if (nContacts == 1U && (rightMouseDown || middleMouseDown))
        {
            // Mouse button started navigation on the viewport; do not cancel when the finger centroid
            // moves over UI (same policy as MOUSE_MOTION + HitTest*).
            if (std::hypot(tf.dx, tf.dy) >= kTouchDeadzone)
            {
                twoFingerOrMouseBridgePanOrbit(event);
            }
        }
        else if (nContacts >= 2U)
        {
            tryApplyIncrementalTwoFingerPan();
        }
        break;
    }
    case SDL_EVENT_WINDOW_RESIZED:
    {
        int width = event.window.data1;
        int height = event.window.data2;
        if (height > 0)
        {
            display->SetAspectRatio(width, height);
        }
        break;
    }
    case SDL_EVENT_SYSTEM_THEME_CHANGED:
        if (display->themeMode == Display::ThemeMode::System)
            display->ApplyTheme();
        break;
    case SDL_EVENT_KEY_DOWN:
        // Bug marker must work even when ImGui wants the keyboard (e.g. focused DragFloat).
        if (event.key.scancode == SDL_SCANCODE_SLASH && !event.key.repeat)
        {
            display->MarkBug();
            break;
        }
        if (io.WantCaptureKeyboard)
            break;
        if (event.key.scancode == SDL_SCANCODE_GRAVE)
        {
            UIRenderer *ui = display->GetUIRenderer();
            ui->SetDebugLayout(!ui->GetDebugLayout());
            break;
        }
        if (event.key.scancode == SDL_SCANCODE_SPACE)
        {
            if (!event.key.repeat)
                display->ResetCameraView();
            break;
        }
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        pendingMouseWheel.push_back(event);
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        // Always release navigation buttons even when ImGui has capture (otherwise RMB stays "down").
        mouseGestures(event);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (io.WantCaptureMouse)
            break;
        mouseGestures(event);
        break;
    case SDL_EVENT_MOUSE_MOTION:
        // During viewport orbit/pan, ImGui may take WantCaptureMouse once the cursor is over a
        // widget; still deliver motion so the in-progress gesture does not stop mid-drag.
        if (io.WantCaptureMouse && !rightMouseDown && !middleMouseDown)
            break;
        mouseGestures(event);
        break;
    default:
        break;
    }
    return true;
}

bool Input::handleEvents()
{
    beginTouchPanAccumForFrame();
    SDL_Event event;
    const ImGuiIO &io = ImGui::GetIO();
    const Sint32 timeoutMs = io.WantTextInput      ? 50
                             : io.WantCaptureMouse ? 16
                                                   : -1;
    if (!SDL_WaitEventTimeout(&event, timeoutMs))
    {
        display->renderDirty = true;
        return true;
    }

    if (!processEvent(event))
        return false;

    while (SDL_PollEvent(&event))
    {
        if (!processEvent(event))
            return false;
    }

    for (const SDL_Event &we : pendingMouseWheel)
    {
        if (!shouldSuppressRedundantTrackpadScroll(we))
        {
            mouseGestures(we);
        }
    }
    pendingMouseWheel.clear();
    if (pendingWheelOrbitSnap)
    {
        pendingWheelOrbitSnap = false;
        display->FinishOrbitSnap();
    }

    display->renderDirty = true;
    return true;
}
