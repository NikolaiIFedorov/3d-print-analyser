# RootPanel Subtitle

## Idea

Tool panels (and potentially other `RootPanel` instances) need a brief description line below the bold header. The existing approach created a "desc" `Paragraph` as the first child of the panel, but this caused an unwanted splitter between the header and the description because the splitter logic unconditionally draws a separator before the first child when a header exists.

The chosen approach adds a dedicated `subtitle` slot directly on `RootPanel` — outside the `children` vector — so the renderer handles it as a fixed layout element between the header and the first real child, with no splitter before it. The splitter after it (before child 0) is preserved.

## Implementation Plan

- `Panel.hpp` — Add `std::optional<Paragraph> subtitle` to `RootPanel`
- `UIRenderer.cpp` — 5 touch points:
  1. `computeBox`: compute subtitle box, add to `contentH`/`contentW`
  2. Guard at line ~710: include `|| panel.subtitle.has_value()`
  3. `placeChildrenVertical`: place subtitle after header, advance cursor; update early-return guard
  4. `emitPanelChildren`: advance `prevVisualBottom` past subtitle; update `needsSplitter` condition to also fire when subtitle-only
  5. Render pass: `renderParagraph` the subtitle after the header
  6. Debug pass: `drawDebugElement` the subtitle
- `ToolPanel.cpp` — Remove the "desc" paragraph child; set `panel.subtitle` from `def.description` instead; drop the desc slot from `totalChildren`

## Bugs Encountered

_none yet_

## Patch Attempts

_none yet_

## Outcome

_pending_
