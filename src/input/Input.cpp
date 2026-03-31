#include "Input.hpp"

Input::Touch Input::signTouch(Touch &touch)
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

    auto acc1_it = touchAccum.find(at1->first);
    auto acc2_it = touchAccum.find(at2->first);

    if (acc1_it == touchAccum.end() || acc2_it == touchAccum.end())
        return GestureType::None;

    TouchAccum &a1 = acc1_it->second;
    TouchAccum &a2 = acc2_it->second;

    float dot = a1.dx * a2.dx + a1.dy * a2.dy;
    return (dot >= 0) ? GestureType::Pan : GestureType::Zoom;
}

void Input::resetGestureState()
{
    currentGesture = GestureType::None;
    touchAccum.clear();
    gestureFrames = 0;
}

void Input::TrackPadGestures()
{
    if (activeTouches.size() >= 3)
    {
        // 3+ fingers = orbit, use only the finger with the largest delta
        float maxMag = 0;
        float orbitDx = 0, orbitDy = 0;
        for (auto &[id, touch] : activeTouches)
        {
            float mag = touch.dx * touch.dx + touch.dy * touch.dy;
            if (mag > maxMag)
            {
                maxMag = mag;
                orbitDx = touch.dx;
                orbitDy = touch.dy;
            }
        }
        currentGesture = GestureType::Orbit;
        display->Orbit(orbitDx, orbitDy);
    }
    else if (activeTouches.size() == 2)
    {
        LOG_DEBU("Two finger");
        Touch p1 = activeTouches.begin()->second;
        Touch p2 = std::next(activeTouches.begin())->second;
        Touch c = {(p1.dx + p2.dx) / 2.0f, (p1.dy + p2.dy) / 2.0f};

        if (currentGesture == GestureType::Orbit)
        {
            // Transitioning down from 3 fingers, reset for 2-finger gesture
            resetGestureState();
        }

        if (currentGesture == GestureType::None)
        {
            gestureFrames++;
            if (gestureFrames < CLASSIFY_FRAMES)
                return;
            currentGesture = classifyTwoFinger();
            if (currentGesture == GestureType::None)
                return;
        }

        switch (currentGesture)
        {
        case GestureType::Pan:
            display->Pan(c.dx, c.dy, false);
            break;
        case GestureType::Zoom:
        {
            float d1 = std::sqrt(p1.dx * p1.dx + p1.dy * p1.dy);
            float d2 = std::sqrt(p2.dx * p2.dx + p2.dy * p2.dy);

            float zoomAmount = d1 + d2;

            if (p1.y > p2.y)
                zoomAmount = zoomAmount * signTouch(p1).dy;
            else
                zoomAmount = zoomAmount * signTouch(p2).dy;

            display->Zoom(zoomAmount, glm::vec3(0, 0, 0));
            break;
        }
        default:
            break;
        }
    }
}

bool Input::HandleEvents()
{
    SDL_Event event;
    SDL_SetHint(SDL_HINT_TRACKPAD_IS_TOUCH_ONLY, "1");
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
        {
            if (std::abs(event.wheel.x) > 0 && activeTouches.empty())
                display->Roll(event.wheel.x * 0.05f);
            if (std::abs(event.wheel.y) > 0 && activeTouches.empty())
                display->Zoom(event.wheel.y * 0.05f, glm::vec3(0, 0, 0));
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (event.button.button == SDL_BUTTON_RIGHT)
                rightMouseDown = true;
            else if (event.button.button == SDL_BUTTON_MIDDLE)
                middleMouseDown = true;
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (event.button.button == SDL_BUTTON_RIGHT)
                rightMouseDown = false;
            else if (event.button.button == SDL_BUTTON_MIDDLE)
                middleMouseDown = false;
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (middleMouseDown)
            {
                display->Orbit(event.motion.xrel * MOUSE_SENSITIVITY,
                               -event.motion.yrel * MOUSE_SENSITIVITY);
            }
            else if (rightMouseDown)
            {
                display->Pan(event.motion.xrel * MOUSE_SENSITIVITY,
                             event.motion.yrel * MOUSE_SENSITIVITY, false);
            }
            break;
        case SDL_EVENT_FINGER_DOWN:
            activeTouches[event.tfinger.fingerID] = {event.tfinger.dx, event.tfinger.dy, event.tfinger.x, event.tfinger.y};
            touchAccum[event.tfinger.fingerID] = {0, 0};
            if (activeTouches.size() >= 2 && currentGesture != GestureType::Orbit)
            {
                // New finger arrived, reset classification
                currentGesture = GestureType::None;
                gestureFrames = 0;
                for (auto &[id, acc] : touchAccum)
                    acc = {0, 0};
            }
            break;
        case SDL_EVENT_FINGER_UP:
            activeTouches.erase(event.tfinger.fingerID);
            touchAccum.erase(event.tfinger.fingerID);
            if (activeTouches.size() < 2)
                resetGestureState();
            break;
        case SDL_EVENT_FINGER_MOTION:
        {
            activeTouches[event.tfinger.fingerID] = {event.tfinger.dx, event.tfinger.dy, event.tfinger.x, event.tfinger.y};
            touchAccum[event.tfinger.fingerID].dx += event.tfinger.dx;
            touchAccum[event.tfinger.fingerID].dy += event.tfinger.dy;
            TrackPadGestures();
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
