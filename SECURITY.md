# Security model

tabbli's design is unusual — data files are executables — so its trust model is
worth stating explicitly.

## The one rule

**Execute only tables you created. Treat any other `.tbl` as data.**

A `.tbl` file found on disk may be a genuine tabbli table or a binary that
merely looks like one. No amount of in-engine verification can protect you if
you execute a malicious file: the decision happens before tabbli's code runs.
The engine can operate on a table passed as data without executing it (planned
`verify` verb will compare a table's engine bytes against a trusted engine);
until then, inspect foreign tables with `strings` or a hex viewer — the record
area is plain text by design.

## What the engine defends against

- **Shell injection via data.** Output values are always single-quote encoded
  (`'\''` for embedded quotes), so a stored value like `$(rm -rf ~)` stays
  inert when an agent copies query output into its next shell command.
- **Malicious or corrupt table files.** Header fields are validated against
  the real file size, allocations are capped (64 MB), the record area is
  crc32-checked, and unparseable data is an explicit error — never silently
  interpreted.
- **Torn writes.** All mutations are temp-file + atomic `rename()`; a crash at
  any point leaves either the old or the new table, never a hybrid.
- **Timestamp forgery.** `created_at`/`updated_at`/`created_by`/`updated_by`
  are engine-owned; user writes to them are rejected with a teaching error.
- **Lock/tmp hygiene.** Lockfiles are opened with `O_NOFOLLOW`, temp files are
  `mkstemp`ed next to the table (never in shared `/tmp`).

Build hardening: `-D_FORTIFY_SOURCE=2 -fstack-protector-strong -fPIE
-Wl,-z,relro,-z,now`.

## What it cannot defend against

- Executing a fake `.tbl` (see the one rule above).
- Prompt injection *content*: record values are untrusted text; an agent
  should treat table content as data, not instructions. That policy belongs in
  the agent's own guardrails.
- A hostile local user with write access to your files (as with any local
  tool).

## Reporting

Found a vulnerability? Email Giuseppe Federico at
<giuseppefeder@gmail.com>. Please include a reproduction; parser findings
(crafted header/record area) are especially welcome.
