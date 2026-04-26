#pragma once

#include "Panel.hpp"
#include "Icons.hpp"
#include <functional>
#include <optional>
#include <string>
#include <vector>

// ─── Prerequisite ────────────────────────────────────────────────────────────
//
// A prerequisite is a two-line Paragraph (title + subtitle) with a colored
// background box.  Its active/completed state is expressed via paragraph-level
// accentBar / dimFill so the decoration spans both lines seamlessly.
//
// State mapping:
//   active    → p.selected = true  (persistent accent tint over the full paragraph)
//   completed → dimFill    = true  (neutral fill, no tint)
//   pending   → neither flag set

struct PrerequisiteDef
{
    std::string id;                      // paragraph id (must be unique within its section)
    std::string title;                   // primary line,   e.g. "Plot measurement point"
    std::string subtitle;                // secondary line, e.g. "to calibrate against"
    Icons::LeadingDrawFn leadingDraw;    // full-height leading checkbox (CheckBox factory)
    bool completed = false;              // true → dimFill (takes priority over active)
    bool active = false;                 // true → p.selected
    std::function<void()> onClick;       // optional: makes the title line clickable
};

// Builds a Paragraph representing one prerequisite step.
// Returns a fully configured Paragraph ready to be added to a Section's children.
// Caller must ensure the parent Section's children vector has sufficient capacity.
Paragraph BuildPrerequisiteParagraph(const PrerequisiteDef &def);

// ─── Parameter ───────────────────────────────────────────────────────────────
//
// One parameter = one Paragraph with one configured SectionLine.
// The SectionLine is the full definition — caller sets imguiContent, iconDraw,
// getMinContentWidthPx, etc. before placing the ParameterDef into ToolPanelDef.

struct ParameterDef
{
    std::string id;   // paragraph id, e.g. "OverhangAngle"
    SectionLine line; // fully configured line — lambdas, icon, min-width, etc.
};

// ─── Tool panel ──────────────────────────────────────────────────────────────
//
// Complete definition for a tool panel:
//
//   RootPanel  (bgParentDepth=0, header = name)
//     Paragraph "desc"           — single dim subtitle line
//     Section   "Prerequisites"  — no header; children built from prerequisites
//     Section   "Parameters"     — collapsible; children built from parameters
//     Section   "Calculator"     — optional; collapsible
//
// Anchor fields are NOT set by the factory — the caller must set them before
// passing the panel to UIRenderer::AddPanel().

struct ToolPanelDef
{
    std::string id;
    std::string name;                           // bold panel header, e.g. "Calibrate"
    std::string description;                    // dim subtitle,      e.g. "Scale model to real-world units"
    std::vector<PrerequisiteDef> prerequisites; // built into Prerequisites section automatically
    std::vector<ParameterDef> parameters;       // built into Parameters section automatically
    bool hasCalculator = false;                 // true = add a third "Calculator" section
    int maxCalculatorLines = 4;                 // reserve hint for Calculator section children
    bool showSectionHeaders = false;            // true = render labeled collapsible headers on Parameters / Calculator sections
};

// Returns a pointer to the first Section child of panel with the given id,
// or nullptr if not found. Use this instead of hardcoded child indices since
// the "desc" paragraph is only present when description is non-empty.
Section *FindSection(RootPanel &panel, const std::string &id);

// Builds a fully populated RootPanel from a ToolPanelDef.
// All prerequisites and parameters are embedded in the returned panel;
// no post-construction population is needed for those sections.
//
// The "desc" paragraph is only emitted when description is non-empty.
// Use FindSection() to locate sections by id rather than hardcoded indices.
RootPanel BuildToolPanel(const ToolPanelDef &def);
