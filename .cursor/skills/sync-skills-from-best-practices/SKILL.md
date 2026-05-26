---
name: sync-skills-from-best-practices
description: Synchronizes project skill guidance with changes in practices/best_practices.md. Use when best_practices.md is edited and related skills may need updates, creation, or drift checks.
disable-model-invocation: true
---

# Sync Skills From Best Practices

Source of truth: `practices/best_practices.md`.

Goal: keep `.cursor/skills/*/SKILL.md` aligned with best-practice policy without blind rewriting.

## When to use

- `practices/best_practices.md` changed.
- A workflow or architecture rule was added, removed, or reworded.
- You suspect drift between skills and the best-practices policy.

## Scope map

Use this default mapping, then expand only if needed:

- `Development Workflow`, `Mini retrospective`, `Debugging Evidence Policy` -> `cad-feature-workflow/SKILL.md`
- `SOLID`, `DRY`, `Pre-Implementation Critique`, `Post-Implementation Review`, `Consistency`, `Threading and Ownership` -> `cad-architecture-solid-dry/SKILL.md`
- `Performance Considerations` -> `cad-cpp-performance-opengl/SKILL.md`
- `Cross-Platform Compatibility` -> `cad-cross-platform-cmake/SKILL.md`
- `Naming` -> `cad-naming-style/SKILL.md`

If no existing skill covers a new section, create a new project skill under `.cursor/skills/<skill-name>/SKILL.md`.

## Required workflow (plan first, then apply)

1. Read `practices/best_practices.md` and target skill files.
2. Build a sync plan with three buckets:
   - `must-update`: policy mismatch that changes behavior/process.
   - `nice-to-update`: wording clarity or examples only.
   - `no-change`: already aligned.
3. Show a short proposed edit list before patching:
   - section changed
   - target skill file
   - one-line intent
4. Apply minimal edits to target skills.
5. Verify:
   - no contradictions with `best_practices.md`
   - consistent terminology across skills (`session_log.json`, main thread, workers, mini retro, etc.)
   - concise skill text (avoid duplication of long policy prose)
6. Report outcome:
   - updated skills
   - created skills (if any)
   - unmatched policy items (if any)

## Editing rules

- Keep `best_practices.md` as the canonical policy; skills are operational summaries.
- Preserve each skill's purpose; do not dump the entire policy into every skill.
- Prefer additive bullet updates over full rewrites.
- Keep names and terms stable unless policy explicitly changed.
- Keep each `SKILL.md` concise (target under 500 lines).

## Output template

Use this response format after sync:

```markdown
Skill sync result
- Updated: <skill path> — <what changed>
- Updated: <skill path> — <what changed>
- Created: <skill path> — <why created>   # only if needed
- Unmatched: <policy section> -> <follow-up>   # only if needed
```
