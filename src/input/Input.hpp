#pragma once

#include <algorithm>
#include <deque>
#include <unordered_map>

#include <SDL3/SDL.h>

#include "display.hpp"

class Input
{
public:
    Input(Display *display);

    bool handleEvents();

private:
    struct Touch
    {
        float dx, dy;
        float x, y;
    };

    Display *display;
    std::unordered_map<SDL_FingerID, Touch> activeTouches;

    enum class GestureType
    {
        None,
        Pan,
        Zoom,
        Orbit
    };
    GestureType currentGesture = GestureType::None;
    bool gestureLocked = false;
    SDL_FingerID orbitFingerID = 0;

    struct TouchAccum
    {
        float dx = 0, dy = 0;
    };
    std::unordered_map<SDL_FingerID, std::deque<TouchAccum>> touchHistory;
    int gestureFrames = 0;
    bool rightMouseDown = false;
    bool middleMouseDown = false;
    bool sliderDragging = false;
    static constexpr float MOUSE_SENSITIVITY = 0.003f;
    static constexpr int LOCK_FRAMES = 12;
    static constexpr int WINDOW_SIZE = 4;
    static constexpr float ORBIT_RATIO_THRESHOLD = 0.45f;
    static constexpr float TOUCH_DEADZONE = 0.001f;

    static Touch signTouch(const Touch &touch);
    GestureType classifyTwoFinger();
    void resetGestureState();
    void trackpadGestures();
    void mouseGestures(const SDL_Event &event);
};