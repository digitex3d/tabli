# Changelog

## v1 (unreleased)

First working engine.

- Self-contained table file: ELF engine + 64-byte header + plain-text records,
  header located via the engine's own ELF size at runtime.
- Verbs: `a` `q` `g` `s` `d` `i`, plus `init [no-ts]`.
- Filters `=` `!=` `>` `<` `>=` `<=` `~`, `limit=N` (default cap 100),
  `count`, `ts`.
- Engine-owned ISO-8601 UTC timestamps (`created_at`/`updated_at`,
  `created_by`/`updated_by` via `by=`); per-table opt-out with `no-ts`.
- Multi-agent primitives: `next <filters> set <fields>` (atomic take),
  `s <id> ... if <cond>` (compare-and-swap), `cursor` / `diff <cursor>`
  (delta sync: new / modified / deleted).
- Rigorous write locking (`flock` on sibling lockfile, bounded retry),
  atomic temp+`rename` writes, crc32 integrity, 64 MB cap, shell-safe
  single-quote output encoding, teaching errors throughout.
- 64-assertion test suite (`make test`), including 10-writer concurrency and
  corruption detection.
