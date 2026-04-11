#include "Input.hpp"

Input::Input(Display *display)
    : display(display)
{
    SDL_SetHint(SDL_HINT_TRACKPAD_IS_TOUCH_ONLY, "1");
}

Input::Touch Input::signTouch(const Touch &touch)
{
    float dx = (touch.dx < 0) ? -1.0f : (touch.dx > 0) ? 1.0f
                                                       : 0.0f;
    float dy = (touch.dy < 0) ? -1.0f : (touch.dy > 0) ? 1.0f
                                                       : 0.0f;
    return {dx, dy, 0, 0};
}

Input::GestureType Input::classifyTwoFinger()
{
    if (activeTouches.size() != 2)
        return GestureType::None;

    auto at1 = activeTouches.begin();
    auto at2 = std::next(at1);

    auto h1_it = touchHistory.find(at1->first);
    auto h2_it = touchHistory.find(at2->first);

    if (h1_it == touchHistory.end() || h2_it == touchHistory.end())
        return GestureType::None;

    auto &h1 = h1_it->second;
    auto &h2 = h2_it->second;

    if (h1.empty() || h2.empty())
        return GestureType::None;

    int n1 = static_cast<int>(std::min(h1.size(), static_cast<size_t>(WINDOW_SIZE)));
    int n2 = static_cast<int>(std::min(h2.size(), static_cast<size_t>(WINDOW_SIZE)));

    float sum1dx = 0, sum1dy = 0, sum2dx = 0, sum2dy = 0;
    for (int i = static_cast<int>(h1.size()) - n1; i < static_cast<int>(h1.size()); i++)
    {
        sum1dx += h1[i].dx;
        sum1dy += h1[i].dy;
    }
    for (int i = static_cast<int>(h2.size()) - n2; i < static_cast<int>(h2.size()); i++)
    {
        sum2dx += h2[i].dx;
        sum2dy += h2[i].dy;
    }

    float mag1 = std::sqrt(sum1dx * sum1dx + sum1dy * sum1dy);
    float mag2 = std::sqrt(sum2dx * sum2dx + sum2dy * sum2dy);

    float maxMag = std::max(mag1, mag2);
    float minMag = std::min(mag1, mag2);

    if (maxMag < 1e-6f)
        return GestureType::None;

    float ratio = minMag / maxMag;

    if (ratio < ORBIT_RATIO_THRESHOLD)
        return GestureType::Orbit;

    float dot = sum1dx * sum2dx + sum1dy * sum2dy;
    return (dot >= 0) ? GestureType::Pan : GestureType::Zoom;
}

void Input::resetGestureState()
{
    currentGesture = GestureType::None;
    gestureLocked = false;
    orbitFingerID = 0;
    touchHistory.clear();
    gestureFrames = 0;
}

void Input::trackpadGestures()
{
    SDL_Keymod mod = SDL_GetModState();
    if ((mod & SDL_KMOD_ALT) || (mod & SDL_KMOD_SHIFT))
        return;

    if (activeTouches.size() == 2)
    {
        Touch p1 = activeTouches.begin()->second;
        Touch p2 = std::next(activeTouches.begin())->second;
        Touch c = {(p1.dx + p2.dx) / 2.0f, (p1.dy + p2.dy) / 2.0f};

        if (!gestureLocked)
        {
            gestureFrames++;
            GestureType detected = classifyTwoFinger();
            if (detected != GestureType::None)
                currentGesture = detected;
            if (gestureFrames >= LOCK_FRAMES && currentGesture != GestureType::None)
                gestureLocked = true;
        }

        if (currentGesture == GestureType::None)
            return;

        switch (currentGesture)
        {
        case GestureType::Pan:
            display->Pan(c.dx, c.dy, true);
            break;
        case GestureType::Orbit:
        {
            // Re-evaluate moving finger until gesture is locked
            if (!gestureLocked || orbitFingerID == 0)
            {
                auto it1 = activeTouches.begin();
                auto it2 = std::next(it1);
                auto h1 = touchHistory.find(it1->first);
                auto h2 = touchHistory.find(it2->first);
                float hm1 = 0, hm2 = 0;
                if (h1 != touchHistory.end())
                    for (auto &s : h1->second)
                        hm1 += std::sqrt(s.dx * s.dx + s.dy * s.dy);
                if (h2 != touchHistory.end())
                    for (auto &s : h2->second)
                        hm2 += std::sqrt(s.dx * s.dx + s.dy * s.dy);
                orbitFingerID = (hm1 > hm2) ? it1->first : it2->first;
            }
            auto movingIt = activeTouches.find(orbitFingerID);
            if (movingIt != activeTouches.end())
            {
                Touch &moving = movingIt->second;
                display->Orbit(moving.dx * 1.0f, -moving.dy * 1.0f);
            }
            break;
        }
        case GestureType::Zoom:
        {
            float d1 = std::sqrt(p1.dx * p1.dx + p1.dy * p1.dy);
            float d2 = std::sqrt(p2.dx * p2.dx + p2.dy * p2.dy);

            float zoomAmount = d1 + d2;

            if (p1.y > p2.y)
                zoomAmount = zoomAmount * signTouch(p1).dy;
            else
                zoomAmount = zoomAmount * signTouch(p2).dy;

            float mx, my;
            SDL_GetMouseState(&mx, &my);
            glm::vec3 cursorWorld = display->ScreenToWorld(mx, my);
            display->Zoom(zoomAmount, cursorWorld);
            break;
        }
        default:
            break;
        }
    }
}

void Input::mouseGestures(const SDL_Event &event)
{
    switch (event.type)
    {
    case SDL_EVENT_MOUSE_WHEEL:
    {
        SDL_Keymod mod = SDL_GetModState();
        float x = event.wheel.x;
        float y = event.wheel.y;
        bool hasModifier = (mod & SDL_KMOD_ALT) || (mod & SDL_KMOD_SHIFT) || (mod & SDL_KMOD_CTRL);
        bool trackpadActive = activeTouches.size() >= 2;
        if (hasModifier || !trackpadActive)
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
                // macOS swaps scroll axes when Shift is held
                float val = (y != 0.0f) ? y : x;
                float mx, my;
                SDL_GetMouseState(&mx, &my);
                glm::vec3 cursorWorld = display->ScreenToWorld(mx, my);
                display->Zoom(val * 0.05f, cursorWorld);
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
        }
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (event.button.button == SDL_BUTTON_LEFT)
        {
            if (display->HandleMouseDown(event.button.x, event.button.y))
                sliderDragging = true;
        }
        else if (event.button.button == SDL_BUTTON_RIGHT)
        {
            if (!display->HitTestUI(event.button.x, event.button.y))
            {
                rightMouseDown = true;
                SDL_SetWindowRelativeMouseMode(display->GetWindow(), true);
            }
        }
        else if (event.button.button == SDL_BUTTON_MIDDLE)
        {
            if (!display->HitTestUI(event.button.x, event.button.y))
            {
                middleMouseDown = true;
                SDL_SetWindowRelativeMouseMode(display->GetWindow(), true);
            }
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (event.button.button == SDL_BUTTON_LEFT)
        {
            if (sliderDragging)
            {
                display->HandleMouseUp();
                sliderDragging = false;
            }
        }
        else if (event.button.button == SDL_BUTTON_RIGHT)
        {
            rightMouseDown = false;
            if (!middleMouseDown)
                SDL_SetWindowRelativeMouseMode(display->GetWindow(), false);
        }
        else if (event.button.button == SDL_BUTTON_MIDDLE)
        {
            middleMouseDown = false;
            if (!rightMouseDown)
                SDL_SetWindowRelativeMouseMode(display->GetWindow(), false);
        }
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (sliderDragging)
            display->HandleMouseMotion(event.motion.x, event.motion.y);
        else if (middleMouseDown)
            display->Orbit(event.motion.xrel * MOUSE_SENSITIVITY,
                           -event.motion.yrel * MOUSE_SENSITIVITY);
        else if (rightMouseDown)
            display->Pan(event.motion.xrel * MOUSE_SENSITIVITY,
                         event.motion.yrel * MOUSE_SENSITIVITY, false);
        break;
    default:
        break;
    }
}

bool Input::handleEvents()
{
    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
        switch (event.type)
        {
        case SDL_EVENT_QUIT:
            return false;
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
        case SDL_EVENT_MOUSE_WHEEL:
            mouseGestures(event);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
        case SDL_EVENT_MOUSE_MOTION:
            if (activeTouches.size() < 2)
                mouseGestures(event);
            break;
        case SDL_EVENT_FINGER_DOWN:
            activeTouches[event.tfinger.fingerID] = {event.tfinger.dx, event.tfinger.dy, event.tfinger.x, event.tfinger.y};
            touchHistory[event.tfinger.fingerID].clear();
            if (activeTouches.size() >= 2 && currentGesture != GestureType::Orbit)
            {
                currentGesture = GestureType::None;
                gestureFrames = 0;
                for (auto &[id, hist] : touchHistory)
                    hist.clear();
            }
            break;
        case SDL_EVENT_FINGER_UP:
            activeTouches.erase(event.tfinger.fingerID);
            touchHistory.erase(event.tfinger.fingerID);
            if (activeTouches.size() < 2)
                resetGestureState();
            break;
        case SDL_EVENT_FINGER_MOTION:
        {
            activeTouches[event.tfinger.fingerID] = {event.tfinger.dx, event.tfinger.dy, event.tfinger.x, event.tfinger.y};
            float fdx = event.tfinger.dx;
            float fdy = event.tfinger.dy;
            float fmag = std::sqrt(fdx * fdx + fdy * fdy);
            if (fmag < TOUCH_DEADZONE)
            {
                fdx = 0.0f;
                fdy = 0.0f;
            }
            auto &hist = touchHistory[event.tfinger.fingerID];
            hist.push_back({fdx, fdy});
            if (hist.size() > WINDOW_SIZE)
                hist.pop_front();
            trackpadGestures();
            break;
        }
        case SDL_EVENT_FINGER_CANCELED:
            activeTouches.clear();
            resetGestureState();
            break;
        }
    }

    return true;
}
