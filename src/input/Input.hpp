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

    /// Call after `SDL_FlushEvents` removes finger/mouse events so `activeTouches` matches hardware (e.g. post-import).
    void NotifySdlEventQueueFlushed();

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

    /// Two-finger pan uses centroid displacement per `FINGER_MOTION` (avoids net-zero deadzone on direction reversals).
    float touchPanCentroidStartX = 0.0f;
    float touchPanCentroidStartY = 0.0f;
    float touchPanPrevCentroidX = 0.0f;
    float touchPanPrevCentroidY = 0.0f;
    bool touchPanHaveCentroidAnchor = false;
    /// True if `activeTouches` ever had ≥2 contacts while processing the current `handleEvents` drain.
    /// Used so `shouldSuppressRedundantTrackpadScroll` still drops duplicate wheel when contact count is
    /// momentarily inconsistent with SDL touch queries at wheel handling time.
    bool multiFingerSeenThisEventDrain = false;
    /// True if incremental two-finger pan applied `Display::Pan` during this `handleEvents` drain.
    /// Suppresses duplicate `SDL_TOUCH_MOUSEID` wheel even when finger-contact snapshots disagree mid-gesture.
    bool touchPanAppliedThisEventDrain = false;
    /// Processed after draining the event queue so FINGER* updates `activeTouches` before suppress checks.
    std::vector<SDL_Event> pendingMouseWheel;
    /// Alt-wheel orbit has no button-up event; snap once after the wheel batch has been applied.
    bool pendingWheelOrbitSnap = false;
    /// After RMB/MMB release or touch pan, ignore unmodified wheel zoom/roll briefly (trackpad inertia).
    Uint64 suppressCameraWheelUntilMs = 0;

    /// Normalized centroid / finger delta magnitude gate (bridge pan + per-motion two-finger pan).
    static constexpr float kTouchDeadzone = 0.00006f;
    /// Ignore unmodified camera wheel briefly after a two-finger pan step (trackpad inertia).
    static constexpr Uint64 kWheelSuppressAfterPanApplyMs = 220;
    /// After multi-touch drops below 2 (lift); inertia may use non-`SDL_TOUCH_MOUSEID` wheel `which`.
    static constexpr Uint64 kWheelSuppressAfterMultiTouchLiftMs = 400;

    void clearTouchState();
    void beginTouchPanAccumForFrame();
    void ensureTouchPanCentroidAnchor();
    void tryApplyIncrementalTwoFingerPan();
    void twoFingerOrMouseBridgePanOrbit(const SDL_Event &event);
    void syncWindowRelativeMouseMode();
    /// During ≥2-finger trackpad contact, skip unmodified wheel zoom/roll (pan uses FINGER centroid; wheel is duplicate).
    /// SDL may tag that wheel with a non-touch device id — suppress anyway. Alt/Shift/Ctrl + wheel still runs.
    bool shouldSuppressRedundantTrackpadScroll(const SDL_Event &wheel) const;
    void mouseGestures(const SDL_Event &event);
    bool processEvent(const SDL_Event &event);
};
