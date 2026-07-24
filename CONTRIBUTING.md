# Contributing to tabli

Thanks for your interest! tabli is small on purpose — please read this before
opening a PR.

## Ground rules

1. **Mechanism, not policy.** The engine only accepts features that *require*
   its unique properties: atomicity, the clock, the lock, the journal. If a
   feature can be expressed as a `kind=...` field convention on top of the
   existing verbs, it belongs in documentation, not in C. This is the test
   every proposal must pass; most don't, and that's by design.
2. **The user is an LLM.** Output changes are judged on token economy and
   self-correction value. Every error message must contain the correct usage
   or a diagnosis — an agent reading the error should be able to fix its next
   call without other help.
3. **Keep it defendable.** No dependencies. New parsing code must validate
   against the real file size, cap allocations and fail loudly. If you touch
   the parser, fuzz it.

## Practical bits

- Build: `make` · Test: `make test` (the suite must stay green; add
  assertions for anything you change).
- One grammar: any new output must be re-parseable as input.
- The on-disk format (64-byte header, text record lines) is versioned; format
  changes need a version bump and a migration story.
- C style: match the existing file — plain C, early `die()` with teaching
  errors, no cleverness.

## License of contributions

By contributing you agree that your contribution is licensed under the
project's MIT license. The project is © Giuseppe Federico; the copyright
notice must be preserved in redistributions, per the license terms.
