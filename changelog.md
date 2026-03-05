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
- Native backend prep:
  - codegen now records constructor->type mapping during ADT generation
  - match codegen emits real `case fxsh_tag_<type>_<ctor>` when constructor can be resolved
  - avoids duplicate `default` labels in generated match switches
  - added `--native` mode prototype: codegen -> clang compile -> execute generated binary
  - added `make test-consistency` smoke suite (`tests/integration/consistency.sh`)
    to compare interpreter/native exit-code consistency on supported examples.
  - native codegen now runs internal type inference and emits type-driven C signatures
    for top-level functions/lambda-lifted `let fn` bindings (instead of fixed `s64`).
  - native codegen now lowers string literal to `sp_str_t` and `++` to `fxsh_str_concat`.
  - native codegen now flattens curried calls (`f a b`) into one C call emission.
  - native consistency suite expanded to include `match/adt/regression_stage2` examples.
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
- Added closure regression examples:
  - `examples/closure_make_adder.fxsh` (returned closure captures parameter)
  - `examples/closure_lexical_scope.fxsh` (lexical scope capture survives shadowing)
  - `examples/closure_recursion.fxsh` (`let rec` function value recursion)
- Added `make test-closure` (`tests/integration/closure.sh`) to assert closure behavior in
  interpreter output.
- Native execution now includes a closure-safe fallback runner:
  - if direct C codegen compile fails, native mode regenerates a runner that
    executes through lexer/parser/type-infer/interpreter inside a compiled binary,
    preserving closure semantics under `--native`.
- Added `make test-closure-native` (`tests/integration/closure_native.sh`) to verify
  closure examples keep interpreter/native exit-code consistency.
- Native mode routing updated:
  - `--native` now always uses closure-safe native runtime runner by default.
  - `--native-codegen` added for direct C codegen native execution path.
- `test-consistency` now includes closure regression examples.
- Native codegen closure work (incremental):
  - generalized nested-lambda lowering to inferred signatures:
    - supports multi-parameter inner lambda calls
    - supports non-`int` closure signatures (e.g. `string -> string`)
    - supports multi-level closure-return chains (e.g. `fn a -> fn b -> fn c -> ...`)
      with staged generated closure structs/functions.
  - added free-identifier collection for closure body capture analysis.
  - fixed top-level shadowing in native codegen by assigning unique static symbols and
    freezing RHS generation at declaration order.
  - added closure value propagation for top-level `let`:
    - alias propagation (`let g2 = g1`) retains closure type information
    - stage propagation (`let g2 = g1 arg`) advances to next closure stage type.
  - extended closure alias/stage propagation into non-top-level `let-in` scope:
    - local closure aliases inside `let-in` preserve closure stage metadata
    - local stage advancement (`let s2 = s1 arg`) is tracked for downstream calls.
    - chained local alias/stage propagation is now covered by regression example:
      `examples/closure_codegen_letin_alias_chain.fxsh`.
  - added `make test-native-codegen` smoke suite (`tests/integration/native_codegen.sh`)
    to guard direct native-codegen path on supported subset.
  - added let type annotations for declarations and let-in bindings:
    - parser now accepts `let x : T = ...`
    - type inference now enforces annotation/type consistency on let bindings
      (including `let rec` and `let-in` local bindings).
  - added type-annotation integration tests:
    - success: `examples/type_annotation_ok.fxsh`, `examples/type_annotation_letin_ok.fxsh`
    - failure: `examples/type_annotation_mismatch.fxsh`
    - `make test-type-annotations` target.
  - added row-polymorphic record core support:
    - parser now stores record literal fields in AST (`{ x = 1, y = 2 }`)
    - interpreter/runtime now supports record values and field access (`r.x`)
    - type inference now supports:
      - record literal typing as closed records
      - field projection with open-row constraint (`{ x : a | r } -> a`)
      - record unification with row-variable propagation.
  - added record integration tests:
    - success: `examples/record_basic.fxsh`, `examples/record_row_poly.fxsh`
    - failure: `examples/record_missing_field.fxsh`
    - `make test-records` target.
  - extended type annotation parser/type lowering for record types:
    - supports record type annotations such as `{ age: int }` and open rows
      `{ age: int; .. 'r }`
    - supports simple type application in annotations (`'a list`).
  - native-codegen now lowers record literal/projection in let-in subset to real C:
    - record literal lowers to `fxsh_record_t` runtime value
    - field projection lowers via `fxsh_record_get` + unbox path
    - now covers:
      - let-in record flows
      - top-level `let` record binding (`examples/record_codegen_top_let.fxsh`)
      - record-as-parameter flow (`examples/record_codegen_param.fxsh`)
    - covered by `examples/record_codegen_*.fxsh` and `make test-native-codegen`.
  - pattern matching increment:
    - parser now keeps concrete literal pattern kinds (`AST_LIT_*`) instead of retagging to
      `AST_PAT_LIT`.
    - native-codegen match now supports non-ADT pattern fallback via condition chain, including
      int/string literal pattern checks and guards.
    - added regression examples: `examples/match_literal.fxsh`, `examples/match_string.fxsh`.
  - record pattern matching increment:
    - parser now supports record patterns: `{ field = pat, ... }`
    - interpreter pattern binding now supports `AST_PAT_RECORD`
    - type inference now infers record pattern type as open-row constraint
    - added regression examples: `examples/match_record.fxsh`,
      `examples/match_record_literal.fxsh`.
  - native-codegen pattern matching increment:
    - record patterns now participate in direct native match fallback chain
      (field presence + field literal checks + variable binding support)
    - native-codegen smoke now includes `match_record` and `match_record_literal`.
- Added `tests/unit/smoke.c` so `make test` has a baseline executable test target.
