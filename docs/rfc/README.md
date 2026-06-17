# Aria RFCs

Substantial framework changes — new modules, protocol designs, breaking
contract changes — go through a lightweight RFC process before landing.
RFCs live here as Markdown files numbered in the order they are accepted.

## Index

| Number | Title | Status |
|--------|-------|--------|
| [0001](0001-http-adapter.md) | HTTP/REST/SSE Adapter | Accepted (2026-06-08) |

## When to write an RFC

Open an RFC when you intend to:

* Add a new module under `modules/`.
* Define or change a protocol that crosses module boundaries
  (binding wire formats, error model, lifecycle contracts).
* Make a breaking ABI change.
* Replace a load-bearing third-party choice (e.g. swapping the
  cpp-httplib server in `adapters/http` for uWebSockets).

Routine bug fixes, doc updates, and small additive APIs do not need
an RFC.

## Template

```markdown
# RFC NNNN — Title

| Field | Value |
|-------|-------|
| Status | Draft / Accepted / Rejected / Superseded |
| Author | <name> |
| Created | YYYY-MM-DD |

## Summary
## Motivation
## Goals
## Non-goals
## Design
## Implementation notes
## Testing strategy
## Future work
## Decision log
```
