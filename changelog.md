# Changelog

## 2026-03-05

### Stability fixes (Phase 1 continuation)
- Fixed parser error recovery to avoid repeated parse-error spam on invalid inputs.
- Added parser synchronization on no-progress loops at program level.
- Fixed compile-time AST/type field mismatches in `ct_type_op` and function-value layout.
- Migrated key `comptime` allocations from `sp_alloc` to arena-backed `fxsh_alloc0`.
- Fixed constructor type instantiation path in type inference for ADT constructor application.

### Next-stage features (Phase 2 incremental)
- Added tree-walking interpreter runtime path (MVP execution before native backend):
  - executes AST directly after type inference
  - supports `let`/`let rec`/`let-in`, `fn`/call/pipe, ADT constructors and `match`
  - reports runtime non-exhaustive match as execution error
- Extracted shared runtime value model (`fxsh_rt_*`) into `src/runtime/runtime.c`
  so interpreter/native/bytecode backends can converge on one value representation.
- Added function application by juxtaposition syntax: `f x y`.
- Added parser support for string concat operator token `++` in additive precedence.
- Added type inference rule for `++` (`string ++ string -> string`).
- Added compile-time evaluator support for string concatenation.
- Improved top-level `let ... in ...` expression handling.
- Allowed uppercase identifiers (`TYPE_IDENT`) in `let` bindings and params.
- Pattern matching parser enhancements:
  - `match ... with ... end` accepted (optional `end` terminator)
  - constructor tuple pattern sugar `Cons(x, xs)` flattened to constructor args
  - optional `end` accepted for `if` and `let ... in ...` expressions
  - fixed optional-`end` boundary handling so expressions no longer consume the next declaration line
  - added match diagnostics in type inference:
    - warns on duplicate constructor arms
    - warns on unreachable arms after catch-all
    - warns on non-exhaustive ADT matches (lists missing constructors)

### Examples / regression
- Updated `examples/comptime.fxsh` to a supported executable subset.
- Updated `examples/adt.fxsh` to a supported executable subset.
- Updated `examples/vector.fxsh` to a supported executable subset.
- Added `examples/regression_stage2.fxsh` for:
  - top-level `let-in`
  - juxtaposition call syntax
  - string concatenation
- Added `examples/match.fxsh` to cover constructor/wildcard match patterns and explicit `end`.
- Added `examples/match_warning.fxsh` to demonstrate match diagnostics.
- Added `tests/unit/smoke.c` so `make test` has a baseline executable test target.
