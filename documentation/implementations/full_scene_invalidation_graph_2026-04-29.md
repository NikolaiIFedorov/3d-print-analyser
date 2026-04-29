# Full scene invalidation graph (2026-04-29)

## Problem

- Medium split fixed accent latency but frame invalidation logic still relies on broad inline flows.
- As edit scenarios grow, dependency ordering and fallback safety need explicit graph contracts.

## Plan implemented

- Add explicit invalidation graph contracts and node stats in `Display`.
- Route event-triggered dirty helpers through node scheduling.
- Add geometry invalidation scope API in `SceneRenderer` (solid/face/edge metadata with safe fallback).
- Introduce node guardrails for pick-before-geometry hazards with full rebuild fallback.
- Surface graph diagnostics in status strip output.
- Keep CPU-first style recolor path while leaving shader-style track as a deferred follow-up.

## Outcome

- Graph contracts now exist (nodes, masks, schedule/exec/skip counters, guardrail/fallback counters).
- Dirty helper APIs now schedule graph nodes rather than only toggling booleans.
- `SceneRenderer::RebuildScope(...)` provides bounded edit invalidation entrypoint with conservative escalation.
- Runtime status line includes renderer counters and graph execution/guardrail signals.

## Follow-up

- Move the remaining inline frame sections into dedicated node handlers (`Geometry`, `Analysis`, `Pick`, `UI`) fully.
- Add shader/material style propagation behind a feature flag after graph behavior stabilizes.
