# Prerequisite card gap inconsistency + background colour

**Date:** 2026-04-22

---

## Idea

Refine the visual presentation of prerequisite cards in the tool panel:

1. The vertical gaps are inconsistent — the title has a small visible gap from the
   card edge while the 'why' subtitle has none (or overflows past the bottom edge).
2. The card background uses `colorDepth = 1`, the same depth as the parent RootPanel,
   so the card is invisible against its parent. Should use `parent.depth + 1 = 2`.

---

## Root Cause

### Gap inconsistency — two compounding problems

**a) Layout/render mismatch for scaled lines**

`computeParagraphBox` uses `bearingPx * fontScale` as the minimum slot height for a
scaled line:
```cpp
float scaledBearingPx = bearingPx * line.fontScale;
float inkH = ink.valid() ? std::max(ink.height() * fontScale, scaledBearingPx)
                         : scaledBearingPx;
```

But `renderParagraph` uses the unscaled `bearingY` as the floor:
```cpp
float baseH = std::max(inkH, bearingY); // bearingY is NOT scaled
```

For the subtitle at `fontScale = 0.85`, layout allocates `bearingPx × 0.85` but render
uses `bearingPx`, meaning the subtitle slot overflows the computed background bottom by
`bearingPx × 0.15`. This eliminates the bottom gap entirely.

**b) Zero padding on a coloured-background paragraph**

`PaddingForLayer(2) = 0`. Since the prerequisite paragraph is `layer = 2`, its padding
is 0 by default. Zero padding means `lineGap = 0` and the text is flush with the
background edges — there is no breathing room.

The small gap the user sees above the title is caused solely by glyph centering within
the `bearingY` slot (ink height < bearingY), not from actual padding.

### Background colour

`colorDepth = 1` matches the parent RootPanel colour (`GetUI(1)`), so the card
background is invisible. `colorDepth = 2 = GetUI(2)` is one step lighter in dark mode
— a subtle neutral raise, the standard "card on panel" pattern.
Accent colour would tint the entire card background with hue, inappropriate here.

---

## Implementation Plan

1. **`UIRenderer.cpp` — `computeParagraphBox`**
   Change the min-height floor for non-imguiContent lines from `scaledBearingPx` to
   `bearingPx`, matching `renderParagraph`:
   ```cpp
   float inkH = ... std::max(ink.height() * line.fontScale, bearingPx) : scaledBearingPx;
   ```

2. **`ToolPanel.cpp` — `BuildPrerequisiteParagraph`**
   - `p.colorDepth = 2` (was `1`)
   - `p.padding = UIElement::PaddingForLayer(1)` — adds `UIGrid::GAP × INSET_RATIO =
     0.22 cells` of symmetric breathing room; `lineGap` is derived from this automatically.

---

## Outcome

Both changes compile cleanly. The layout/render mismatch fix ensures the subtitle slot
is correctly accounted for in `computeParagraphBox`, giving symmetric top and bottom
gaps. The padding addition provides consistent breathing room around the content, and
`colorDepth = 2` makes the card background visible against the parent panel.

---

## Revision — 2026-04-23

**Observation:** The card design felt inconsistent with the rest of the panel language,
which uses flat rows (no background) with hover/active tinting.

**Change:** Removed the card treatment from prerequisites entirely — `bgParentDepth`
and `padding` no longer set, leaving both at their default values (`-1` and `0`).
The paragraph-level `accentBar` and `dimFill` are retained; they still span both title
and subtitle lines via the paragraph's bounding area. The `computeParagraphBox` fix
from the previous pass is still in place.

Build: clean.

---

## Revision — 2026-04-23 — Prerequisite checkbox interactivity

**Idea:** Make the leading checkbox slot clickable. Import fires `DoFileImport()`
as before; Point1/Point2 select the clicked paragraph (and deselect the other).

**Implementation plan:**
1. `Panel.hpp` — Add `std::function<void()> onClick` to `Paragraph`; fires when the
   leading slot is clicked.
2. `ToolPanel.cpp` — `p.onClick = def.onClick` in `BuildPrerequisiteParagraph` so the
   leading slot and title line share the same action.
3. `UIRenderer.cpp` — After `leadingDraw` call, if `item.onClick` is set, spawn an
   ImGui window (`##lead<path>`) over the leading slot area and place an `InvisibleButton`
   that calls `item.onClick()` on click; hand cursor on hover.
4. `display.cpp` — After panel build, assign `selectPoint1`/`selectPoint2` lambdas
   to both `calibPara_PointN->onClick` (leading slot) and
   `calibLine_PointNPrimary->onClick` (title line) so both hit targets are wired.

**Outcome:** Clean build. Clicking the checkbox or title of a point prerequisite
selects it and deselects the other. Import still opens the file dialog.
