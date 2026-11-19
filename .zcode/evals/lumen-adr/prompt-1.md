# Eval prompt: lumen-adr — 1: the weighed choice

## SETUP (tester only — the subject never sees this section)

Create `sandbox/repo/docs/adr/` containing three existing ADRs to anchor the
corpus: `001-test-lean.md` (title `# ADR-001: Test`, a `**Status:** Accepted`
line only), `002-test-full.md` (full header block:
Status/Date/Scope/Deciders/Related), `003-test-current.md` (title
`# ADR 003 — Test`, full header). The next free number is therefore **004** — a
subject that "knows" a bigger number from memory (e.g. from another corpus)
fails honestly. Verify the three files exist before dispatch.

## THE ASK (dispatch verbatim to the subject)

We settled a design today and it needs its ADR in `docs/adr/` of the sandbox
repo: a text shaping cache. We weighed three options — (1) cache in the text
module keyed by shaped-run hash with LRU eviction, (2) per-window caches in the
UI layer, (3) a shared cache service process. We picked (1): the UI layer must
not own shaping knowledge, and a service process costs a round-trip per shape.
It touches the text and UI modules and changes no wire protocol. Draft the ADR.
