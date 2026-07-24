# tabli

**A self-contained table for LLM agents: engine, data and manual in a single executable file.**

*Built for users with no memory, no eyes, and a token budget.*

```
$ tabli init backlog.tbl
$ ./backlog.tbl a title='fix locking' status=open prio=2
id=1 title='fix locking' status=open prio=2 created_at=2026-07-24T14:12:05Z
$ ./backlog.tbl q status=open
id=1 title='fix locking' status=open prio=2
# 1 record
```

A `.tbl` file **is** its own database engine. Copy it anywhere, run it bare and it
prints its own manual. Delete the tabli project — every table you created keeps
working, because everything it needs lives inside the file itself.

Created by **Giuseppe Federico** ([giuseppefeder@gmail.com](mailto:giuseppefeder@gmail.com)).

## Why

LLM agents constantly need somewhere to put notes, backlogs, work queues and
shared state. Existing options are either too heavy (a database server), too
fragile (agents doing surgery on markdown tables), or too race-prone
(concurrent agents appending to the same file). The documented failure modes of
agent systems — lost coordination, stale state, forgotten context — are mostly
*infrastructure* failures, not intelligence failures.

tabli is that missing infrastructure, designed for an unusual user: **the user
is an LLM**. Every design choice follows from that.

- **One grammar everywhere.** The stored form, the input form and the output
  form of a record are the same line: `id=7 title='fix lock' status=open`.
  Query results are copy-pasteable back into commands. One thing to learn.
- **Errors that teach.** Every error contains the correct usage or the
  diagnosis. An agent that gets an error has, in the error itself, everything
  needed to self-correct on the next call.
- **Context-window guard.** `q` caps output by default and says so. `count`
  answers "how many" for near-zero tokens. Empty results are explicit
  (`# 0 records match`), never silent.
- **Engine-owned timestamps.** `created_at`/`updated_at` are written by the
  engine's clock, never by the agent — one field of ground truth that cannot
  be hallucinated. Disable per-table with `init <path> no-ts`.
- **Race-free multi-agent primitives.** `next <filter> set <fields>` atomically
  takes the next matching record — two agents calling it concurrently always
  receive different records, with no orchestrator. `s <id> ... if field=value`
  is compare-and-swap: stale beliefs become explicit, teachable errors instead
  of silent corruption.
- **Delta sync.** `cursor` returns an opaque token; `diff <cursor>` later
  returns only what changed — new, modified, deleted — so an agent waking up
  with no memory pays ten lines instead of the whole table.

## Interface

Run any table with no arguments to get this from the file itself:

| verb | usage | notes |
|---|---|---|
| `a` | `./t.tbl a title='fix lock' status=open [by=name]` | add; echoes the full record with its new id |
| `q` | `./t.tbl q status=open prio>1 [limit=N] [count] [ts]` | filter; `q` alone lists all (default cap 100) |
| `g` | `./t.tbl g 7` | get one record, timestamps included |
| `s` | `./t.tbl s 7 status=done [if status=open] [by=name]` | set fields; `if` = compare-and-swap |
| `d` | `./t.tbl d 7` | delete (echoes what it deleted; ids are never reused) |
| `i` | `./t.tbl i` | record count, last id, field histogram |
| `next` | `./t.tbl next status=open set status=claimed by=me` | atomic take: find first match and mark it, under one lock |
| `cursor` / `diff` | `./t.tbl cursor` → later `./t.tbl diff <token>` | what changed since |

Filters: `=` `!=` `>` `<` `>=` `<=` `~` (contains). Multiple filters are AND.
Numeric comparison when both sides are numbers, lexicographic otherwise — which
makes ISO timestamps directly filterable: `q updated_at>2026-07-20`.

Fields are free `key=value` pairs, no schema. Values with spaces use
single quotes; output is always shell-safe (quote-escaped), so data can never
smuggle command substitution into an agent's next shell call.

Reserved words: `id`, `by`, `if`, `set`, `limit`, `count`, `ts`,
`created_at`, `updated_at`, `created_by`, `updated_by`.

## How it works

```
┌──────────────────────────────┐
│ ELF engine (~40 KB)          │  identical for every table
├──────────────────────────────┤
│ 64-byte header               │  magic, version, flags, count,
├──────────────────────────────┤  committed data_len, last_id, crc32
│ records: text, 1 per line    │
└──────────────────────────────┘
```

- The engine locates its header at its own ELF size, computed at runtime from
  the ELF section-header table. No offset patching, no external state.
- Linux forbids writing to an executing binary (`ETXTBSY`), so every write
  builds the new version as a temp sibling and `rename()`s it over the
  original — the swap is atomic; readers and executors always see a coherent
  file. A table cannot be half-written.
- Writers serialize on a `flock()`ed sibling lockfile (`.name.tbl.lock`) with
  bounded retry; readers are lock-free.
- The record area is covered by a crc32; corruption is detected and reported,
  never silently parsed.
- `init` clones the running engine's own bytes: tables are created by budding.
  The seed and every table carry the same engine, so any table can be
  inspected with `strings`, and the data is readable even without executing
  anything.

## Design principles

1. **Mechanism, not policy.** The engine contains only what requires its
   unique properties (atomicity, clock, lock): the six verbs, `next`, `if=`,
   `cursor/diff`, timestamps. Everything else — decision logs, work-queue
   conventions, per-domain semantics — is `kind=...` field conventions layered
   on top by the agent. The engine stays generic and small.
2. **The user is an LLM.** Output is optimized for tokens and
   self-correction, not for human eyes. The file is its own documentation.
3. **Small enough to defend.** ~900 lines of C, no dependencies. Every
   feature is attack surface; the roadmap earns features through real use,
   not speculation.

## Limits (declared, not discovered)

- Linux only (x86-64 and ARM64 — build on the target arch). The design relies
  on ELF, `/proc/self/exe` and `flock`.
- 64 MB per table hard cap. Instant below ~100k records; sequential scan by
  design — if you need indexes, you need a real database (use SQLite).
- Every write rewrites the file (atomicity by construction): ideal for the
  KB–MB scale it targets, wrong for high-frequency logging.
- A `.tbl` is an executable: run only tables you created. Treat foreign
  tables as data (see [SECURITY.md](SECURITY.md)).

## Agent skill (Claude Code) — one-line install

Paste this into Claude Code:

```
Read https://raw.githubusercontent.com/digitex3d/tabli/main/skill/tabli/SKILL.md and follow its Install section
```

That's it: the skill's manual carries its own installer — Claude reads it,
fetches the four files into `~/.claude/skills/tabli`, and verifies the install
by creating a real table and reading it back. (The skill ships a prebuilt
x86-64 engine plus the source, compiled on-the-fly for other architectures.)

Prefer doing it by hand? `cp -r skill/tabli ~/.claude/skills/tabli` from a
clone does the same. Either way, tables created afterwards are self-sufficient:
they keep working even if the skill is removed.

## Build & test

```
make        # cc, hardened flags (FORTIFY, stack protector, PIE, RELRO)
make test   # 64-assertion suite: quoting round-trips, CAS, concurrency,
            # corruption detection, no-ts tables, cursor/diff
```

## License

[MIT](LICENSE) — © 2026 Giuseppe Federico. Attribution is required by the
license: keep the copyright notice.
