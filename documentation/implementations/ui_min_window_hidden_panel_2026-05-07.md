# UI minimum window width — hidden root panels (2026-05-07)

## Problem

With Analysis active, Calibrate stays `visible = false`, but `ResolveAnchors` still ran full `computeBox` on Calibrate: header + **subtitle** (very long single-line description) contributed `box.outerWidth`. `ComputeMinGridSize()` takes the max horizontal extent of all panels → SDL minimum window width matched the **inactive** Calibrate copy, not the visible Analysis card. The FILES strip (left+right anchored) then stretched across that inflated width, so most of the bar looked empty.

## Approach

1. **`UIRenderer::ResolveAnchors` — `computeBox` for `RootPanel`:** if `!item.visible`, set content box to zero and `outerWidth`/`outerHeight` to margin+padding only; return before measuring header/subtitle/children.
2. **`Display::RefreshUIMinWindowSize()`:** centralize `ComputeMinGridSize` + `SDL_SetWindowMinimumSize`; call from `InitUI`, after tool switch, and when collapsing/expanding a tool panel from the toolbar so the floor updates when visibility changes.

## Outcome

Minimum window width follows visible tool UI; FILES bar still fills toolbar→right edge but the **window** is no longer forced wide by hidden Calibrate text.

## Mini retro

Root-cause pattern: layout metrics for off-screen/hidden chrome should not feed global constraints. Consider documenting in Architecture_UI.md under panel visibility.
