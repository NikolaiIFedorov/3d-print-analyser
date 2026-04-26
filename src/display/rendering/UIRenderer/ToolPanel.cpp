#include "ToolPanel.hpp"

Paragraph BuildPrerequisiteParagraph(const PrerequisiteDef &def)
{
    Paragraph p;
    p.id = def.id;
    // Keep default outer margin so a single prerequisite still has comfortable
    // spacing from surrounding splitters/sections.
    p.padding = UIGrid::GAP * UIElement::INSET_RATIO * 0.5f;
    p.borderRadius = UIElement::RadiusForLayer(2) * 0.5f;    // less rounded than default layer-2
    p.selected = def.active && !def.completed;
    p.dimFill = def.completed;
    p.onClick = def.onClick; // leading slot fires the same action as the title line
    if (def.leadingDraw)
    {
        p.leadingDraw  = def.leadingDraw;
        p.leadingWidth = UIGrid::GAP; // reserves space for the checkbox; renderer applies a fixed pixel gap after it
    }
    p.values.reserve(def.subtitle.empty() ? 1 : 2);

    // Title line
    SectionLine &title = p.values.emplace_back();
    title.text = def.title;
    title.textDepth = def.completed ? 1 : 2; // dim when completed
    title.onClick = def.onClick;

    // Subtitle line — omitted when empty
    if (!def.subtitle.empty())
    {
        SectionLine &sub = p.values.emplace_back();
        sub.text = def.subtitle;
        sub.fontScale = 0.85f;
        sub.textDepth = (def.active && !def.completed) ? 1 : 0; // readable when active, dim otherwise
        sub.onClick = def.onClick; // full row is interactive, not just title
    }

    return p;
}

RootPanel BuildToolPanel(const ToolPanelDef &def)
{
    const int totalChildren = 1                              // Prerequisites
                              + 1                            // Parameters
                              + (def.hasCalculator ? 1 : 0); // Calculator

    RootPanel panel;
    panel.id = def.id;
    panel.bgParentDepth = 0; // parent is the viewport (depth 0); renderer adds +1 → GetUI(1)
    panel.header = Header{def.name, 1.0f, 2};
    panel.children.reserve(totalChildren);

    // ── Subtitle (optional) ───────────────────────────────────────────────
    if (!def.description.empty())
    {
        Paragraph &sub = panel.subtitle.emplace();
        sub.values.reserve(1);
        SectionLine &line = sub.values.emplace_back();
        line.text = def.description;
        line.textDepth = 1; // dim
    }

    // ── Prerequisites section ─────────────────────────────────────────────
    Section &prereqSec = panel.AddSection("Prerequisites");
    prereqSec.noChildSplitters = true;
    prereqSec.children.reserve(def.prerequisites.size());
    for (const auto &pd : def.prerequisites)
        prereqSec.AddParagraph(pd.id) = BuildPrerequisiteParagraph(pd);

    // ── Parameters section ────────────────────────────────────────────────
    Section &paramSec = panel.AddSection("Parameters");
    // Stack measurement + derived rows without an inner rail (one visual block).
    paramSec.noChildSplitters = true;
    if (def.showSectionHeaders)
    {
        paramSec.header = Header{"Parameters", 1.0f, 2};
        paramSec.tightHeader = true;
    }
    paramSec.children.reserve(def.parameters.size());
    for (const auto &pm : def.parameters)
    {
        Paragraph &p = paramSec.AddParagraph(pm.id);
        p.values.reserve(1);
        p.values.push_back(pm.line);
    }

    // ── Calculator section (optional) ─────────────────────────────────────
    if (def.hasCalculator)
    {
        Section &calcSec = panel.AddSection("Calculator");
        if (def.showSectionHeaders)
        {
            calcSec.header = Header{"Result", 1.0f, 2};
            calcSec.tightHeader = true;
        }
        calcSec.children.reserve(def.maxCalculatorLines);
    }

    return panel;
}

Section *FindSection(RootPanel &panel, const std::string &id)
{
    for (auto &child : panel.children)
    {
        if (std::holds_alternative<Section>(child))
        {
            Section &sec = std::get<Section>(child);
            if (sec.id == id)
                return &sec;
        }
    }
    return nullptr;
}
