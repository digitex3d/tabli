---
name: tabeli
description: Self-contained table files (.tbl) for persistent agent state — backlogs, notes, work queues, shared state across sessions and parallel agents. Use when you need to persist structured key=value records, coordinate multiple agents on shared work (atomic take, compare-and-swap), or sync only what changed between sessions (cursor/diff). Each table is a single executable file carrying its own engine, data and manual.
---

# tabeli — self-contained tables for agents

A `.tbl` file is **engine + data + manual in one executable**. Tables you create
keep working even if this skill is deleted — everything lives in the file.

## Get the engine (once per machine)

```bash
ENGINE=$(<this-skill-dir>/scripts/ensure-engine.sh)
```

The script prints the path to `bin/tabeli-<arch>` (x86_64 prebuilt; other
architectures are compiled on first use from `src/tabeli.c`, needs `cc`).
Linux only.

## Create a table

```bash
"$ENGINE" init ./backlog.tbl          # engine-written timestamps (default)
"$ENGINE" init ./log.tbl no-ts        # without timestamps
```

## Use it — talk to the file itself, the engine is no longer needed

Run any table bare to get its built-in manual: `./backlog.tbl`

```bash
./backlog.tbl a title='fix lock' status=open prio=2 by=me   # add → echoes record + id
./backlog.tbl q status=open prio>1                          # filter (AND); q alone = all, cap 100
./backlog.tbl q status!=done count                          # count only (cheapest)
./backlog.tbl g 7                                           # one record, timestamps included
./backlog.tbl s 7 status=done if status=open by=me          # set; 'if' = compare-and-swap
./backlog.tbl d 7                                           # delete (ids never reused)
./backlog.tbl i                                             # count, last id, field histogram
```

Filters: `=` `!=` `>` `<` `>=` `<=` `~`(contains). Values with spaces in
single quotes. Fields are free `key=value`, no schema. Numeric compare when
both sides numeric, else lexicographic — so `q updated_at>2026-07-20` works.
**Errors teach the correct usage: on any error, read it and retry as told.**

## Multi-agent / multi-session patterns

- **Work queue** (race-free, no orchestrator):
  `./t.tbl next status=open set status=claimed by=<you>` — atomic take; two
  agents calling concurrently always get different records. Exit 1 = queue empty.
- **Guard against stale beliefs**: use `if` on every state transition
  (`s 7 status=done if status=claimed`). Exit 4 = your view was stale → `g 7`
  and reconsider; never retry blindly.
- **Session wake-up**: at end of work run `./t.tbl cursor` and save the token
  in your notes; next session `./t.tbl diff <token>` returns only new/modified/
  deleted records instead of the whole table.
- Timestamps (`created_at`/`updated_at`/`created_by`/`updated_by`) are
  engine-written and cannot be set by you; pass `by=<agent-name>` on writes to
  record authorship.

## Conventions (policy lives here, the engine stays generic)

- Decision log: `a kind=decision title='crc32 over sha' because='...'` — recall
  with `q kind=decision` at session start to avoid re-litigating settled choices.
- Contextual notes: `a kind=note about=7 text='...'` — check `q kind=note` for
  a topic before acting on it.
- Failed attempts: `a kind=attempt target=7 how='regex approach' outcome=failed`
  — check `q kind=attempt target=7` before retrying a task.

## Security — the one rule

**Execute only `.tbl` files you created.** A foreign `.tbl` is an untrusted
binary: inspect it with `strings` (records are plain text near the end of the
file) — do not run it.

## Exit codes

`0` ok · `1` not found / queue empty · `2` usage error (the message shows the
fix) · `3` lock busy → retry shortly · `4` if-condition failed → reload with
`g` first · `5` corrupt table

## Install (only if you are reading this OUTSIDE ~/.claude/skills — e.g. from a URL)

This file installs itself. Run exactly this (Linux):

```bash
BASE=https://raw.githubusercontent.com/digitex3d/tabeli/main/skill/tabeli
DEST=~/.claude/skills/tabeli
mkdir -p "$DEST/bin" "$DEST/src" "$DEST/scripts"
curl -fsSL "$BASE/SKILL.md"                 -o "$DEST/SKILL.md"
curl -fsSL "$BASE/bin/tabeli-x86_64"         -o "$DEST/bin/tabeli-x86_64"
curl -fsSL "$BASE/src/tabeli.c"              -o "$DEST/src/tabeli.c"
curl -fsSL "$BASE/scripts/ensure-engine.sh" -o "$DEST/scripts/ensure-engine.sh"
chmod +x "$DEST/bin/tabeli-x86_64" "$DEST/scripts/ensure-engine.sh"
```

Then verify the tabeli way — create a real table, write, read back:

```bash
ENGINE=$("$HOME/.claude/skills/tabeli/scripts/ensure-engine.sh")
T=$(mktemp -u).tbl && "$ENGINE" init "$T" && "$T" a check=ok && "$T" q count && rm -f "$T"
```

Expected: the record echoes back and `# 1 record`. Tell the user the skill is
installed (it loads in new sessions; the engine is usable immediately in this
one), and that tables created with it are self-sufficient — they keep working
even if the skill is removed.

(c) Giuseppe Federico — MIT license.
