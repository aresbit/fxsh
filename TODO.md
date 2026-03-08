# TODO

This file tracks issues exposed while building and demoing `fxsh 1.0` with the `apps/fx-doctor` workload.

## Priority 0

- [x] Add minimal CLI argument support to the language runtime
  - Why: current apps need a shell launcher to translate argv into env vars.
  - MVP surface:
    - `argv0() -> string`
    - `argc() -> int`
    - `argv_at(i: int) -> string`
  - Affected areas:
    - [src/main.c](/data/data/com.termux/files/home/yyscode/dev0304/fxsh/src/main.c)
    - [src/interp/interp.c](/data/data/com.termux/files/home/yyscode/dev0304/fxsh/src/interp/interp.c)
    - [src/codegen/codegen.c](/data/data/com.termux/files/home/yyscode/dev0304/fxsh/src/codegen/codegen.c)
    - [src/runtime/shell.c](/data/data/com.termux/files/home/yyscode/dev0304/fxsh/src/runtime/shell.c)
    - [include/fxsh.h](/data/data/com.termux/files/home/yyscode/dev0304/fxsh/include/fxsh.h)
  - Acceptance:
    - a pure `fxsh` CLI can read arguments without a wrapper for common cases.
  - Status:
    - interpreter, `--native`, and `--native-codegen` now receive script argv consistently
    - reference example: [examples/argv_basic.fxsh](/data/data/com.termux/files/home/yyscode/dev0304/fxsh/examples/argv_basic.fxsh)

- [x] Make `fxsh <file.fxsh>` quiet by default for normal execution
  - Why: interpreter mode currently prints `Type`, `Comptime evaluation`, `Interpreter`, `Success` banners, which is wrong for end-user CLI apps.
  - Desired behavior:
    - default run prints only program output
    - debug banners move behind explicit flags
  - Affected areas:
    - [src/main.c](/data/data/com.termux/files/home/yyscode/dev0304/fxsh/src/main.c)
  - Acceptance:
    - a CLI script can be run directly without output filtering.
  - Status:
    - normal file execution now prints only program output
    - `-e/--eval` still prints the final expression result
    - `apps/fx-doctor` no longer needs shell-side output filtering

## Priority 1

- [x] Fix `--codegen` to emit pure C code only
  - Why: current `--codegen` output is polluted by front-end banners before the generated C payload.
  - Affected areas:
    - [src/main.c](/data/data/com.termux/files/home/yyscode/dev0304/fxsh/src/main.c)
  - Acceptance:
    - `./bin/fxsh --codegen file.fxsh > out.c` yields compilable C without post-processing.
  - Status:
    - `--codegen` now starts directly with generated C instead of front-end banners

- [ ] Stabilize multi-file CLI workloads in `native-codegen`
  - Why: string/list-heavy app code still fails in generated C on real workloads.
  - Observed symptoms:
    - generated C type mismatches around helper functions and list/string helpers
    - runtime app path had to fall back to interpreter + launcher filtering
  - Affected areas:
    - [src/codegen/codegen.c](/data/data/com.termux/files/home/yyscode/dev0304/fxsh/src/codegen/codegen.c)
    - [src/types/types.c](/data/data/com.termux/files/home/yyscode/dev0304/fxsh/src/types/types.c)
  - Acceptance:
    - `apps/fx-doctor` can build and run through `native-codegen` without a shell-side output filter.
  - Status:
    - `apps/fx-doctor/src/main.fxsh` now runs through `./bin/fxsh --native-codegen ...`
    - string equality in generated C now respects inferred `string` types instead of only string literals
    - `apps/fx-doctor/Makefile` includes `check-native` to keep the path exercised

- [x] Fix top-level `unit` bindings in codegen
  - Why: `let _ = print ...` still generates invalid C like `static void ___0;`.
  - Observed while validating:
    - `examples/argv_basic.fxsh` had to be rewritten to a single final `print` to avoid this backend bug.
  - Affected areas:
    - [src/codegen/codegen.c](/data/data/com.termux/files/home/yyscode/dev0304/fxsh/src/codegen/codegen.c)
  - Acceptance:
    - top-level side-effect bindings that return `unit` compile cleanly in `--native-codegen`.
  - Status:
    - fixed for top-level `let _ = ...` style bindings
    - reference example: [examples/codegen_unit_top_let.fxsh](/data/data/com.termux/files/home/yyscode/dev0304/fxsh/examples/codegen_unit_top_let.fxsh)

- [x] Execute bare top-level expressions in codegen/native-codegen
  - Why: a trailing top-level expression like `print "ok"` was ignored in generated C while the interpreter executed it.
  - Affected areas:
    - [src/codegen/codegen.c](/data/data/com.termux/files/home/yyscode/dev0304/fxsh/src/codegen/codegen.c)
  - Acceptance:
    - interpreter and `--native-codegen` execute the same set of top-level expressions in program order.
  - Status:
    - fixed for mixed top-level `let` / bare-expression programs
    - reference example: [examples/codegen_top_expr.fxsh](/data/data/com.termux/files/home/yyscode/dev0304/fxsh/examples/codegen_top_expr.fxsh)

- [x] Audit record-heavy helper functions in type inference
  - Why: generic helper layers using records hit inference boundaries during the `fx-doctor` implementation.
  - Observed symptoms:
    - helper modules built around `{ code, stdout, stderr }` style records became fragile in larger compositions
  - Affected areas:
    - [src/types/types.c](/data/data/com.termux/files/home/yyscode/dev0304/fxsh/src/types/types.c)
    - [src/interp/interp.c](/data/data/com.termux/files/home/yyscode/dev0304/fxsh/src/interp/interp.c)
  - Acceptance:
    - a medium-size multi-module CLI can structure shell results with records without type-system surprises.
  - Status:
    - native codegen record field projection now restores the projected field type instead of defaulting to `i64`
    - reference example: [examples/record_helper_native.fxsh](/data/data/com.termux/files/home/yyscode/dev0304/fxsh/examples/record_helper_native.fxsh)

- [x] Audit tuple / constructor ergonomics for medium-arity data flow
  - Why: this workload exposed practical limits or confusing behavior when using higher-arity tuple-like flows in app code.
  - Affected areas:
    - [src/parser/parser.c](/data/data/com.termux/files/home/yyscode/dev0304/fxsh/src/parser/parser.c)
    - [src/types/types.c](/data/data/com.termux/files/home/yyscode/dev0304/fxsh/src/types/types.c)
    - [src/codegen/codegen.c](/data/data/com.termux/files/home/yyscode/dev0304/fxsh/src/codegen/codegen.c)
  - Acceptance:
    - tuple/constructor arity behavior is documented, tested, and consistent across interpreter and codegen.
  - Status:
    - constructor tuple sugar now normalizes by declared constructor arity instead of flattening tuples blindly in the parser
    - generic constructor payload patterns now inherit concrete tuple element hints in native codegen
    - reference example: [examples/ctor_tuple_payload.fxsh](/data/data/com.termux/files/home/yyscode/dev0304/fxsh/examples/ctor_tuple_payload.fxsh)

## Priority 2

- [ ] Clarify stdlib import semantics in docs
  - Why: `import String` is easy to misread as a system library import.
  - Needed doc updates:
    - imports first try `stdlib/<lowercase>.fxsh`
    - imported modules are source-expanded, not dynamically linked
  - Affected areas:
    - [README.md](/data/data/com.termux/files/home/yyscode/dev0304/fxsh/README.md)
    - [docs/half_hour_fxsh.md](/data/data/com.termux/files/home/yyscode/dev0304/fxsh/docs/half_hour_fxsh.md)

- [ ] Add a first-class app example section to docs
  - Why: `apps/fx-doctor` is now a meaningful reference workload.
  - Needed docs:
    - app layout
    - launcher pattern
    - env-backed bridge pattern
    - when to prefer pure `fxsh` vs thin shell launcher

- [ ] Add regression coverage for app-facing runtime patterns
  - Needed tests:
    - quiet normal execution
    - pure `--codegen` output
    - argv builtins
    - multi-file app import path smoke tests

- [x] Specialize generic curried closures in native-codegen
  - Why: curried generic helpers that capture values across closure stages were erasing later-stage argument types to `s64`.
  - Fixed by:
    - flattening curried lambda chains into mono-spec native helpers when a call site has fully concrete arguments
    - letting `gen_call` prefer those mono helpers before falling back to generic closure stubs
    - tightening codegen type fallback so unresolved row-polymorphic field access can still recover concrete field types from local hints
  - Regression:
    - [examples/closure_curried_generic.fxsh](/data/data/com.termux/files/home/yyscode/dev0304/fxsh/examples/closure_curried_generic.fxsh)

- [x] Relax parser sensitivity around multiline parenthesized expressions
  - Why: a multiline `print (a ++ (if ...))` form in [examples/argv_basic.fxsh](/data/data/com.termux/files/home/yyscode/dev0304/fxsh/examples/argv_basic.fxsh)
    triggered parse errors until flattened to one line.
  - Affected areas:
    - [src/parser/parser.c](/data/data/com.termux/files/home/yyscode/dev0304/fxsh/src/parser/parser.c)
  - Acceptance:
    - ordinary multiline parenthesized expressions parse the same as their single-line form.
  - Status:
    - binary/logical/pipe expression parsing now skips internal newlines consistently
    - reference example: [examples/parser_multiline_paren.fxsh](/data/data/com.termux/files/home/yyscode/dev0304/fxsh/examples/parser_multiline_paren.fxsh)

## Current workaround in repository

- `apps/fx-doctor` uses a stable hybrid path:
  - shell launcher for argv + process orchestration
  - `fxsh` for formatting / output modes
- This is acceptable for the current demo, but it should not be the long-term language story.
