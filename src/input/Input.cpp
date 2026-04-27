#include "Input.hpp"
#include "imgui.h"
#include "imgui_impl_sdl3.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_touch.h>

#include <algorithm>
#include <cmath>

namespace
{

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

void Input::clearTouchState()
{
    activeTouches.clear();
    fingerArrivalOrder.clear();
    touchPanAccDx = 0.0f;
    touchPanAccDy = 0.0f;
    touchPanEventCount = 0;
}

void Input::beginTouchPanAccumForFrame()
{
    touchPanAccDx = 0.0f;
    touchPanAccDy = 0.0f;
    touchPanEventCount = 0;
    pendingMouseWheel.clear();
}

void Input::applyBatchedTwoFingerPan()
{
    if (touchPanEventCount <= 0)
    {
        return;
    }
    int w = 0, h = 0;
    SDL_GetWindowSize(display->GetWindow(), &w, &h);
    if (w > 0 && h > 0)
    {
        ImGuiIO &imguiIo = ImGui::GetIO();
        for (const auto &kv : activeTouches)
        {
            const float px = kv.second.x * static_cast<float>(w);
            const float py = kv.second.y * static_cast<float>(h);
            if (imguiIo.WantCaptureMouse || display->HitTestUI(px, py) || display->HitTestImGui(px, py))
                return;
        }
    }
    const float n = static_cast<float>(touchPanEventCount);
    const float cdx = touchPanAccDx / n;
    const float cdy = touchPanAccDy / n;
    if (std::abs(cdx) < kTouchDeadzone && std::abs(cdy) < kTouchDeadzone)
    {
        return;
    }
    const float s = display->mouseSensitivity / 30.0f;
    display->Pan(cdx * s, cdy * s, true);
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
    const bool want = rightMouseDown || middleMouseDown;
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
    if (event.wheel.which == SDL_TOUCH_MOUSEID)
    {
        return true;
    }
    if (activeTouches.size() >= 2U)
    {
        return true;
    }
    return sdlHasMultiTouchContact();
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
        float x = event.wheel.x;
        float y = event.wheel.y;
        bool hasModifier = (mod & SDL_KMOD_ALT) || (mod & SDL_KMOD_SHIFT) || (mod & SDL_KMOD_CTRL);
        if (hasModifier)
        {
            if (mod & SDL_KMOD_ALT)
            {
                display->Orbit(x * 0.05f, y * 0.05f);
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
            syncWindowRelativeMouseMode();
            display->UpdatePickHover(static_cast<float>(event.button.x), static_cast<float>(event.button.y));
        }
        else if (event.button.button == SDL_BUTTON_MIDDLE)
        {
            middleMouseDown = false;
            syncWindowRelativeMouseMode();
            display->UpdatePickHover(static_cast<float>(event.button.x), static_cast<float>(event.button.y));
        }
        break;
    case SDL_EVENT_MOUSE_MOTION:
    {
        // ImGui may not have wanted capture on the mouse-down frame; also skip when over custom UI
        // so a click + drag on widgets never moves the camera once the cursor is over the UI stack.
        float mx, my;
        SDL_GetMouseState(&mx, &my);
        const bool blockNav = io.WantCaptureMouse || display->HitTestUI(mx, my) || display->HitTestImGui(mx, my);
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
        clearTouchState();
        if (rightMouseDown || middleMouseDown)
        {
            rightMouseDown = false;
            middleMouseDown = false;
            syncWindowRelativeMouseMode();
        }
        break;
    case SDL_EVENT_FINGER_CANCELED:
    {
        if (io.WantCaptureMouse)
        {
            break;
        }
        clearTouchState();
        break;
    }
    case SDL_EVENT_FINGER_DOWN:
    {
        if (io.WantCaptureMouse)
        {
            break;
        }
        const SDL_TouchFingerEvent &tf = event.tfinger;
        if (std::find(fingerArrivalOrder.begin(), fingerArrivalOrder.end(), tf.fingerID) == fingerArrivalOrder.end())
        {
            fingerArrivalOrder.push_back(tf.fingerID);
        }
        activeTouches[tf.fingerID] = Touch{0.0f, 0.0f, tf.x, tf.y};
        break;
    }
    case SDL_EVENT_FINGER_UP:
    {
        if (io.WantCaptureMouse)
        {
            break;
        }
        const SDL_TouchFingerEvent &tf = event.tfinger;
        activeTouches.erase(tf.fingerID);
        fingerArrivalOrder.erase(
            std::remove(fingerArrivalOrder.begin(), fingerArrivalOrder.end(), tf.fingerID),
            fingerArrivalOrder.end());
        break;
    }
    case SDL_EVENT_FINGER_MOTION:
    {
        if (io.WantCaptureMouse)
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

        const size_t nContacts = activeTouches.size();
        if (nContacts == 1U && (rightMouseDown || middleMouseDown))
        {
            int w = 0, h = 0;
            SDL_GetWindowSize(display->GetWindow(), &w, &h);
            const float px = (w > 0) ? tf.x * static_cast<float>(w) : 0.0f;
            const float py = (h > 0) ? tf.y * static_cast<float>(h) : 0.0f;
            const bool blockTouchNav =
                io.WantCaptureMouse || display->HitTestUI(px, py) || display->HitTestImGui(px, py);
            if (!blockTouchNav &&
                (std::abs(tf.dx) >= kTouchDeadzone || std::abs(tf.dy) >= kTouchDeadzone))
            {
                twoFingerOrMouseBridgePanOrbit(event);
            }
        }
        else if (nContacts >= 2U)
        {
            // Two-finger scroll + Shift/Alt is handled via MOUSE_WHEEL (orbit/zoom); do not also pan from FINGER_MOTION.
            const SDL_Keymod mod = SDL_GetModState();
            const bool wheelOverridesTwoFingerPan =
                (mod & SDL_KMOD_ALT) != 0 || (mod & SDL_KMOD_SHIFT) != 0;
            if (!wheelOverridesTwoFingerPan &&
                (std::abs(tf.dx) >= kTouchDeadzone || std::abs(tf.dy) >= kTouchDeadzone))
            {
                touchPanAccDx += tf.dx;
                touchPanAccDy += tf.dy;
                ++touchPanEventCount;
            }
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
    case SDL_EVENT_MOUSE_MOTION:
        if (io.WantCaptureMouse)
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

    applyBatchedTwoFingerPan();

    display->renderDirty = true;
    return true;
}
