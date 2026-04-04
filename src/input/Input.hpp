#pragma once

#include <algorithm>
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

    struct TouchAccum
    {
        float dx = 0, dy = 0;
    };
    std::unordered_map<SDL_FingerID, TouchAccum> touchAccum;
    int gestureFrames = 0;
    bool rightMouseDown = false;
    bool middleMouseDown = false;
    static constexpr float MOUSE_SENSITIVITY = 0.005f;
    static constexpr int CLASSIFY_FRAMES = 10;

    static Touch signTouch(const Touch &touch);
    GestureType classifyTwoFinger();
    void resetGestureState();
    void trackpadGestures();
    void mouseGestures(const SDL_Event &event);
};