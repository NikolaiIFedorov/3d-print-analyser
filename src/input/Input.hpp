#pragma once

#include <unordered_map>
#include <vector>

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
    bool rightMouseDown = false;
    bool middleMouseDown = false;

    /// Tracked for two-finger trackpad (touch) pan. Order = contact order; use first two when ≥2.
    std::vector<SDL_FingerID> fingerArrivalOrder;
    std::unordered_map<SDL_FingerID, Touch> activeTouches;

    /// Sums 2-finger FINGER_MOTION in one `handleEvents` pass; applied after draining the queue.
    float touchPanAccDx = 0.0f;
    float touchPanAccDy = 0.0f;
    int touchPanEventCount = 0;
    /// Processed after draining the event queue so FINGER* updates `activeTouches` before suppress checks.
    std::vector<SDL_Event> pendingMouseWheel;
    /// After RMB/MMB release or touch pan, ignore unmodified wheel zoom/roll briefly (trackpad inertia).
    Uint64 suppressCameraWheelUntilMs = 0;

    static constexpr float kTouchDeadzone = 0.0005f;

    void clearTouchState();
    void beginTouchPanAccumForFrame();
    void applyBatchedTwoFingerPan();
    void twoFingerOrMouseBridgePanOrbit(const SDL_Event &event);
    void syncWindowRelativeMouseMode();
    /// If two-finger trackpad is also sent as `MOUSE_WHEEL`, skip unmodified roll/zoom (FINGER already pans).
    /// Does not apply when Alt/Shift/Ctrl + wheel (explicit Orbit/Zoom/Roll).
    bool shouldSuppressRedundantTrackpadScroll(const SDL_Event &wheel) const;
    void mouseGestures(const SDL_Event &event);
    bool processEvent(const SDL_Event &event);
};
