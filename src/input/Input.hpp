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

    /// Two-finger pan uses centroid displacement across one `handleEvents` pass (stable vs per-finger dx averaging).
    float touchPanCentroidStartX = 0.0f;
    float touchPanCentroidStartY = 0.0f;
    bool touchPanHaveCentroidAnchor = false;
    bool touchPanBlockedByWheelModsThisPass = false;
    /// True if `activeTouches` ever had ≥2 contacts while processing the current `handleEvents` drain.
    /// Used so `shouldSuppressRedundantTrackpadScroll` still drops duplicate wheel when contact count is
    /// momentarily inconsistent with SDL touch queries at wheel handling time.
    bool multiFingerSeenThisEventDrain = false;
    /// Processed after draining the event queue so FINGER* updates `activeTouches` before suppress checks.
    std::vector<SDL_Event> pendingMouseWheel;
    /// Alt-wheel orbit has no button-up event; snap once after the wheel batch has been applied.
    bool pendingWheelOrbitSnap = false;
    /// After RMB/MMB release or touch pan, ignore unmodified wheel zoom/roll briefly (trackpad inertia).
    Uint64 suppressCameraWheelUntilMs = 0;

    /// Normalized centroid / finger delta magnitude gate (bridge pan + batch pan).
    static constexpr float kTouchDeadzone = 0.00006f;
    /// Ignore unmodified camera wheel briefly after batched two-finger pan applies (trackpad inertia).
    static constexpr Uint64 kWheelSuppressAfterPanApplyMs = 220;
    /// After multi-touch drops below 2 (lift); inertia may use non-`SDL_TOUCH_MOUSEID` wheel `which`.
    static constexpr Uint64 kWheelSuppressAfterMultiTouchLiftMs = 400;

    void clearTouchState();
    void beginTouchPanAccumForFrame();
    void ensureTouchPanCentroidAnchor();
    void applyBatchedTwoFingerPan();
    void twoFingerOrMouseBridgePanOrbit(const SDL_Event &event);
    void syncWindowRelativeMouseMode();
    /// If two-finger trackpad is also sent as `MOUSE_WHEEL`, skip unmodified roll/zoom (FINGER already pans).
    /// Does not apply when Alt/Shift/Ctrl + wheel (explicit Orbit/Zoom/Roll).
    bool shouldSuppressRedundantTrackpadScroll(const SDL_Event &wheel) const;
    void mouseGestures(const SDL_Event &event);
    bool processEvent(const SDL_Event &event);
};
