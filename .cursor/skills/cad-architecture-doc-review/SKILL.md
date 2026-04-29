---
name: cad-architecture-doc-review
description: >-
  Read-only architecture review for a subsystem: summarize behaviour, map how
  it works, record strengths and gaps, and log gaps in documentation/TODO.
  Use when asked to document a module, write or refresh Architecture_*.md,
  architecture pass, subsystem review, or “what does X do / where does it fail”.
---

# CAD_OpenGL — Architecture documentation & review

## Purpose

Produce or update **`documentation/Architecture_<Subsystem>.md`** (top-level only — not `documentation/debug/` or `documentation/implementations/`) by **reading the code**, then keep **`documentation/TODO`** honest: anything that “does not work,” is fragile, incomplete, or violates project norms should surface as a tracked item.

This skill is **review + documentation + TODO triage**, not feature implementation. For change logs and build notes, use **`cad-feature-workflow`** (`documentation/implementations/`).

## When to apply

- User asks for an architecture doc, module overview, or “review subsystem X.”
- User wants to know what a part of the code does, how, and what is wrong or missing.
- Refreshing an existing `Architecture_*.md` after meaningful code changes.

## Canonical examples (match tone and depth)

Skim at least one finished doc before writing:

- [documentation/Architecture_Input.md](../../../documentation/Architecture_Input.md) — tables, data-flow diagram, verdict, ranked concerns.
- [documentation/Architecture_UI.md](../../../documentation/Architecture_UI.md) — hierarchical UI layout.

Adjust section headings to fit the subsystem; keep the doc **scannable** (tables and short paragraphs over walls of prose).

## Workflow

### 1. Scope

- Name the subsystem, entry points (files, public classes), and boundaries (what is in / out of scope).
- List sources to read; prefer following call chains and data flow over skimming filenames only.

### 2. Read and verify

- Trace real behaviour in code (not only comments).
- Note **what works** (invariants, clean boundaries, tests if any) and **what does not** (bugs, dead code, duplication, missing APIs, platform assumptions, singletons blocking tests, silent failure paths).

### 3. Write or update `documentation/Architecture_<Subsystem>.md`

Include, when relevant:

- **Executive summary** — one paragraph what it does; **Verdict** (e.g. Approve / Approve with reservations); top strengths and top concerns (numbered, short).
- **Requirements & motivation** — functional vs non-functional; constraints.
- **Solution description** — components, responsibilities, data flow (ASCII or table is fine).
- **Design principles** — what conforms; what is deliberately relaxed and why (align with **`cad-architecture-solid-dry`** where useful).
- **Gaps** — explicit “Non-Conforming” / “Known limitations” / “Risks” tables with **severity** and **remediation** (or “filed in TODO” with pointer).

Do **not** put implementation diaries here — those belong under `documentation/implementations/`.

### 4. Update `documentation/TODO`

**Mandatory:** For every substantive gap (bug, tech debt, missing behaviour, unclear contract), add or refresh an entry in [documentation/TODO](../../../documentation/TODO).

- Under **`## Known Issues (from architecture reviews)`**, add bullets under the correct subsystem heading (create a `### <Subsystem>` block if missing). Use `- [ ]` checklist lines; keep one issue per line when possible.
- For a large review batch, optionally add a dated section at the bottom (see existing **`## [YYYY-MM-DD] Camera`** / File Import examples): link **`Source: documentation/Architecture_<Subsystem>.md`**, group into Must-Have / Should-Have / Nice-to-Have if it helps prioritization.
- If this pass **writes a new** top-level architecture doc, update **`## Architecture Documents`** — check **`### Written`** (or add a line there); do not use `documentation/debug/` or `documentation/implementations/` for this checklist unless the user explicitly asked for those areas.

Also bump **`Last updated:`** at the top of `documentation/TODO` when you change it.

### 5. Consistency check

- Every **high-severity** or **user-visible broken** item in the Architecture doc appears on `documentation/TODO`.
- File names and symbol names in TODO entries match the codebase (paths under `src/`).

## Boundaries

| Output | Location |
|--------|----------|
| Subsystem architecture review | `documentation/Architecture_*.md` (root of `documentation/` only) |
| Gaps and follow-ups | `documentation/TODO` |
| Feature / patch narrative | `documentation/implementations/` (**cad-feature-workflow**) |

## Related skills

- **`cad-feature-workflow`** — implementation and retro; not a substitute for Architecture + TODO.
- **`cad-architecture-solid-dry`** — vocabulary for principles and tradeoffs sections.

Canonical project habits: [practices/best_practices.md](../../../practices/best_practices.md).
