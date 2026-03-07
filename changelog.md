# Changelog

## 2026-03-07

- Native self-tail-call optimization (OCaml-style loop lowering) added in C backend:
  - applies to top-level `let` lambda functions and `decl fn` functions when parameters are
    variable patterns and tail position contains self-call.
  - rewrites tail-recursive self-calls into `for (;;)` + parallel temp rebind + `continue`.
  - supports tail positions through `if` branches, `match` branches, and `let-in` wrapper
    passthrough.
  - added `examples/tco_sum_tail.fxsh`.
  - added `examples/tco_match_int_tail.fxsh`.

- Language spec convergence (remove shell syntax sugar design):
  - DESIGN/README now explicitly define: no language-level shell sugar.
  - removed/retired spec references to `$()` / `run!` / `try!` / `cap!`.
  - shell capability is standardized as library/runtime builtins:
    `exec_*`, `capture_*`, `glob`, `grep_lines`.

- Record literal field type annotation implemented:
  - parser now accepts `name: type = expr` inside record literals.
  - type inference now validates each annotated field type by unification.
  - added `examples/record_field_annotation.fxsh`.

- `do { ... }` / `let!` parser lowering implemented:
  - lexer now recognizes `do` keyword and standalone `!` token.
  - parser lowers do-block statements to `let-in` chains:
    - `let! x = expr;`
    - `expr;` (discarded via fresh tmp binding)
    - final expression as block result.
  - do-block now also supports regular `let` statements (with `rec/comptime`, param sugar,
    and type annotations) before the final expression.
  - do-block expression statements now also support line-separated form (without mandatory `;`);
    only the last expression is kept as block result.
  - `let!` now lowers to Result-style short-circuit match (`Ok` continue, `Err` early return).
  - added `let?` in do-block for Option-style short-circuit (`Some` continue, `None` early return).
  - added builtin constructor typing for `Some`/`None`/`Ok`/`Err` in `constr_env` to improve
    Option/Result inference precision.
  - improved do-block parser diagnostics for invalid `let`/`let!` forms and statement boundaries.
  - invalid do-block statements now fail run reliably (non-zero) instead of silently continuing.
  - parser now tracks `had_error`; parse errors stop pipeline before type/comptime/runtime stages.
  - added `examples/do_block.fxsh`.
  - added `examples/do_block_let.fxsh`.
  - added `examples/do_block_expr_lines.fxsh`.
  - added `examples/do_block_bind_result_ok.fxsh` and `examples/do_block_bind_result_err.fxsh`.
  - added `examples/do_block_bind_option_some.fxsh` and `examples/do_block_bind_option_none.fxsh`.

- String literal feature completion (spec alignment):
  - lexer now supports:
    - multiline strings: `"""..."""`
    - raw strings: `r"..."`
    - byte strings: `b"..."`
    - f-strings: `f"...{expr}..."`
  - parser lowers `f"...{expr}..."` into string concat AST (`++`) with embedded expression parsing.
  - native codegen string emission now uses centralized C escaping helper (handles newline/tab/CR/NUL
    and non-printable bytes), fixing multiline/raw/byte literal compile issues.
  - added `examples/string_literals_extended.fxsh`.

- Extended shell runtime/process builtins with richer capture primitives:
  - `exec_stdin_code`, `exec_stdin_stderr`
  - `exec_pipe_code`, `exec_pipe_stderr`
  - wired through interpreter, type environment, and native codegen builtin lowering.
- Added single-run shell capture handle primitives:
  - `exec_capture : string -> int` (capture id)
  - `capture_code : int -> int`
  - `capture_stdout : int -> string`
  - `capture_stderr : int -> string`
  - `capture_release : int -> bool`
  - wired through interpreter, type environment, and native codegen builtin lowering.
  - capture slots now auto-evict the oldest entry when full (prevents hard slot exhaustion).
- Extended single-run capture to stdin/pipe flows:
  - `exec_stdin_capture : (string, string) -> int`
  - `exec_pipe_capture : (string, string) -> int`
  - `exec_pipefail_capture : (string, string) -> int` (2-stage pipefail semantics)
  - `exec_pipefail3_capture : (string, string, string) -> int`
  - `exec_pipefail4_capture : (string, string, string, string) -> int`
  - wired through interpreter, type environment, and native codegen builtin lowering.
- Upgraded `stdlib/shell.fxsh` high-level API:
  - `run : string -> { code, stdout, stderr }`
  - `run_stdin : string -> string -> { code, stdout, stderr }`
  - `pipe : string -> string -> { code, stdout, stderr }`
  - `pipefail : string -> string -> { code, stdout, stderr }` (2-stage)
  - `pipeline2/3/4` multi-stage pipeline helpers with unified result record
  - `pipeline2_pipefail` helper.
  - `pipeline3_pipefail` / `pipeline4_pipefail` helpers.
- Added `examples/shell_stdin_pipe_capture.fxsh` as end-to-end validation.
- Added `examples/shell_pipeline_multi.fxsh` for multi-stage pipeline validation.
- Added `examples/shell_pipefail.fxsh` for pipeline exit-code semantics validation.
- Added `examples/shell_pipeline_pipefail_multi.fxsh` for 3/4-stage pipefail validation.
- Added `examples/shell_capture_release.fxsh` for capture lifecycle validation.
- Fixed native codegen boxed-value lowering for record fields/call expressions:
  - `gen_boxed_expr` now uses strict type inference for `AST_CALL` and `AST_FIELD_ACCESS`
    instead of always boxing as `i64`, so string-valued shell capture fields compile correctly.
- Extended stdlib wrappers (`os/process/io`) to expose the new primitives:
  - `with_stdin_code`, `with_stdin_stderr`
  - `pipe_code`, `pipe_stderr`
- Added high-level shell stdlib:
  - `stdlib/shell.fxsh` with:
    - `run : string -> { code, stdout, stderr }`
    - `code/stdout/stderr : capture_id -> ...`
    over single-run capture.
- Added native-safe shell pipeline combinators in `stdlib/shell.fxsh`:
  - `pipeline2/pipeline3`
  - `pipeline2_code/pipeline2_stdout/pipeline2_stderr`
  - `pipeline3_code/pipeline3_stdout/pipeline3_stderr`.
- Added `examples/shell_capture.fxsh` as end-to-end usage sample.
- Added `examples/shell_capture_handle.fxsh` for `Shell.run` record usage.
- Added `examples/shell_pipeline.fxsh`.

- Added source-level cross-file import expansion in the CLI front-end:
  - scans `import Name` before lexing/parsing
  - auto-loads `stdlib/name.fxsh` (lowercased) or `<current-dir>/name.fxsh`
  - injects loaded source as:
    - `module Name = struct ... end`
    - `import Name`
  - recursively expands imports in loaded modules (depth-limited) and avoids duplicate injection.
- Added unified OS stdlib module:
  - `stdlib/os.fxsh` (system/path/process helpers in one place)
  - compatibility layers retained in:
    - `stdlib/system.fxsh`
    - `stdlib/path.fxsh`
    - `stdlib/process.fxsh`
    - `stdlib/io.fxsh`
- Added examples:
  - `examples/os_basic.fxsh`
  - `examples/import_os.fxsh`

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
  - tuple pattern full-chain increment:
    - type inference now handles tuple expressions (`AST_TUPLE -> TYPE_TUPLE`)
    - interpreter/runtime now support tuple runtime values and tuple pattern binding
    - native-codegen now lowers tuple values (`fxsh_tuple_t`) and tuple pattern checks/bindings
      in match fallback chain.
  - native-codegen pattern binding now selects unbox path by inferred basic type hint for
    tuple/record pattern variables:
    - supports `int` / `float` / `bool` / `string` bindings
    - covered by `examples/match_tuple_basic_types.fxsh` and
      `examples/match_record_basic_types.fxsh`.
  - native-codegen pattern variable binding upgraded from shape-guessing to strict
    type-inference injection in `match` arms:
    - derives per-arm pattern variable types by unifying pattern type with inferred
      scrutinee type
    - injects inferred variable type into boxed unbox path selection
      (`int` / `float` / `bool` / `string`)
    - adds regression for call-scrutinee pattern binding:
      `examples/match_call_tuple_strict_bind.fxsh`.
  - list full-chain increment:
    - parser: list literal now accepts `;` separators (`[1; 2; 3]`) and supports
      right-associative cons expression `::`.
    - parser pattern: supports list patterns `[]`, `[p1; p2]`, and `p1 :: p2`.
    - type inference: adds `AST_LIST`/`AST_PAT_CONS` typing and `::` rule
      (`a :: list a -> list a`).
    - interpreter/runtime: adds immutable runtime list value and pattern matching.
    - native-codegen: lowers list literal/cons/match-pattern to real C runtime list nodes.
    - regressions:
      `examples/list_basic.fxsh`, `examples/list_pattern_literal.fxsh`.
  - stdlib list core refresh:
    - rewrote `stdlib/list.fxsh` to current executable subset and kept core APIs:
      `head` / `tail` / `map` / `fold` / `filter`.
    - added `stdlib/list.fxsh` to interpreter/native consistency suite.
- Added `tests/unit/smoke.c` so `make test` has a baseline executable test target.

## 2026-03-06

### Comptime metaprogramming increment
- Fixed compile-time environment lookup/update to use `sp_str_equal` key matching,
  avoiding missed bindings when identifier slices differ by pointer.
- Implemented closure-aware compile-time function application:
  - lambda now captures comptime env snapshot
  - call supports partial application (returns residual function)
  - call supports chained/over-application across nested call nodes.
- Updated compile-time function storage layout to carry generic closure payload.
- Added/verified reflection operators on executable path:
  - `@typeOf`, `@sizeOf`, `@alignOf`, `@hasField`, `@fieldsOf`.
- Added `examples/comptime_reflection.fxsh` and `make test-comptime`
  (`tests/integration/comptime.sh`) coverage.
- Added compile-time AST metaprogramming core operators:
  - parser/type/comptime support for `@quote(expr)`, `@unquote(ast_expr)`,
    `@splice(ast_expr)`.
  - `@quote` returns `CT_AST`; `@unquote`/`@splice` evaluate quoted AST in comptime context.
  - added compatibility path for uppercase comptime identifiers in operator arguments
    (e.g. `@unquote(Q)` parsed as constructor-form token).
- Added `examples/comptime_quote.fxsh` and integrated checks in `test-comptime`.
- Added expression-level `comptime` evaluation syntax:
  - `comptime expr`
  - `comptime { expr }`
  - wired as `AST_CT_EVAL` through parser/type/comptime/interpreter paths.
- Added compile-time diagnostics builtins:
  - `@compileLog(msg)` prints compile-time diagnostics and returns unit
  - `@compileError(msg)` aborts compilation with error
  - `@panic(msg)` aborts compilation with panic error.
- Main pipeline now executes comptime declarations before runtime/native execution so
  compile-time failures stop the run early.
- Added compile-time AST expansion pass (`fxsh_ct_expand_program`) before execution:
  - folds top-level `let x = comptime { ... }` into concrete AST values
    (literals/records/lists/functions).
  - exposes expanded non-comptime top-level `let` bindings to subsequent
    `let comptime` declarations in the same file.
  - supports top-level comptime expression lowering into AST for downstream pipeline use.
- Added compile-time specialization flow for curried functions via expansion:
  - partial application at comptime can materialize as normal function AST
    (closure bindings lowered through generated `let-in` wrappers).
- Fixed compile-time record field growth crash:
  - removed invalid `sp_free` on arena-backed buffers in `fxsh_ct_record_add_field`.
- Added examples:
  - `examples/comptime_block.fxsh`
  - `examples/comptime_compile_error.fxsh`
  - `examples/comptime_panic.fxsh`
  - `examples/comptime_specialize.fxsh`
- Native-codegen monomorphization increment:
  - added callsite-driven specialization collection for polymorphic top-level lambda `let`
    bindings (`TYPE_VAR` in function type).
  - native codegen now emits specialized C functions per concrete callsite signature
    (`fxsh_mono_<fn>_<type...>`), and rewrites matching callsites to specialized symbols.
  - allows mixed-type usage of polymorphic identity-style functions in direct C backend
    without collapsing to one fallback ABI.
  - initial specialization target focuses on concrete scalar/list/record/tuple-friendly
    signatures inferred from call argument hints; closure-generic path remains unchanged.
  - added regression example: `examples/mono_id_int_string.fxsh`.
  - added native smoke coverage in `tests/integration/native_codegen.sh`.
- Module system baseline support:
  - parser now supports `module M = struct ... end` and `import M`.
  - module declarations are front-end lowered to prefixed top-level symbols
    (`M__name`) so existing type/interpreter/native backends can execute without
    backend-specific module runtime.
  - qualified access `M.name` is lowered at parse/postfix stage to the prefixed symbol.
  - initial module body support targets `let` declarations in `struct` body.
  - added regression example: `examples/module_basic.fxsh`.
  - added smoke coverage to:
    - `tests/integration/consistency.sh`
    - `tests/integration/native_codegen.sh`.
- Module ADT increment:
  - module body now keeps `type` declarations (not only `let`) during flattening.
  - module lowering now prefixes type names and constructor names as well.
  - module symbol rewrite now covers constructor expressions and match patterns.
  - parser now supports qualified constructor pattern form `M.Ctor`.
  - parser qualified lookup now preserves constructor identity (`M.Ctor` lowers to
    constructor AST, not plain identifier).
  - added regression example: `examples/module_adt.fxsh`.
- C FFI MVP (native-codegen):
  - added top-level FFI declaration convention:
    - `let c_fn : T1 -> ... -> TR = "c:<symbol>"`
  - type checker treats above declarations as externally-provided function symbols and
    uses the annotated function type directly.
  - native codegen now emits `extern` declaration + wrapper function and routes calls
    to the wrapped C symbol.
  - FFI wrapper now supports:
    - `unit -> T` lowered to C no-arg `(<void>)` call shape
    - `... -> unit` lowered to C `void` return wrapper.
    - `string` ABI bridge for C interop:
      - FFI extern-side type is emitted as `const char *`
      - wrapper auto-marshals `sp_str_t -> NUL-terminated char*` for arguments
      - wrapper maps C `const char *` return into `sp_str_t`.
    - integer ABI refinement for C interop:
      - added explicit C integer types: `c_int`, `c_uint`, `c_long`, `c_ulong`, `c_size`, `c_ssize`
      - FFI extern prototypes now emit precise C integer ABI for these types
      - added integer ABI cast builtins: `int_to_*` and `*_to_int` families for above types.
  - added pointer-oriented builtins for FFI plumbing:
    - `c_null : unit -> 'a ptr`
    - `c_malloc : int -> unit ptr`
    - `c_free : unit ptr -> unit`
    - `c_cast_ptr : 'a ptr -> 'b ptr`
    - `c_callback0 : (unit -> unit) -> unit ptr` (callback symbol to opaque C pointer)
  - native codegen maps `a ptr` to C `void*`.
  - native compile now accepts extra flags via environment:
    - `FXSH_CFLAGS` for compile flags
    - `FXSH_LDFLAGS` for link flags (e.g. `-luv`).
  - added regression example: `examples/ffi_abs.fxsh`.
  - added string-interop example: `examples/ffi_atol.fxsh`.
  - added precise-int ABI example: `examples/ffi_puts_cint.fxsh`.
  - added pointer smoke example: `examples/ffi_ptr_memset.fxsh`.
  - added callback-pointer smoke example: `examples/ffi_callback0.fxsh`.

### Tensor MVP (interpreter + native-codegen)
- Added built-in `tensor` runtime type (2D float) across type/interpreter/native-codegen.
- Added builtins:
  - `tensor_new2 : int -> int -> float -> tensor`
  - `tensor_from_list2 : int -> int -> float list -> tensor`
  - `tensor_shape2 : tensor -> (int, int)`
  - `tensor_get2 : tensor -> int -> int -> float`
  - `tensor_set2 : tensor -> int -> int -> float -> tensor`
  - `tensor_add : tensor -> tensor -> tensor`
  - `tensor_dot : tensor -> tensor -> tensor`
- Added regression example: `examples/tensor_basic.fxsh`.
- Added typed-shape transformer sketch example: `examples/transformer_shape_safe.fxsh` (wrapper-type-checked wiring).
- Added mismatch negative example: `examples/transformer_shape_mismatch_error.fxsh` (type error expected).
- Added smoke coverage updates in:
  - `tests/integration/consistency.sh`
  - `tests/integration/native_codegen.sh`

### System library MVP (effects)
- Added core system/effect builtins end-to-end (type/interpreter/native-codegen):
  - `print : string -> unit`
  - `getenv : string -> string`
  - `file_exists : string -> bool`
  - `read_file : string -> string`
  - `write_file : string -> string -> bool`
  - `exec : string -> int`
- Type inference now seeds builtin type environment at entry.
- Interpreter call path now evaluates builtins directly with runtime checks.
- Native-codegen call lowering now emits direct C runtime helpers for the above builtins,
  with backend prelude support for env/file/process operations.
- Added stdlib wrapper module:
  - `stdlib/system.fxsh`
- Added effect regression example:
  - `examples/sys_basic.fxsh`
- Added smoke coverage:
  - `tests/integration/consistency.sh`
  - `tests/integration/native_codegen.sh`
- Extended `tests/integration/comptime.sh` with new success/failure checks for above.
- Regression status:
  - `make test-comptime`: pass
  - `make test-consistency`: pass (sequential)
  - `make test-native-codegen`: pass

### Result-style error model and API layering
- Unified system wrappers to result-style return values and split APIs by domain files:
  - `stdlib/system.fxsh`
  - `stdlib/path.fxsh`
  - `stdlib/process.fxsh`
- Current native-codegen-safe wrapper shape is monomorphic:
  - `type result = Ok of string | Err of string`
  - wrapper payloads normalize success values to strings (`"ok"`, `"true"`, `"false"`, text body).
- Added/updated effect regression examples:
  - `examples/sys_basic.fxsh`
  - `examples/sys_result_api.fxsh`
- Added smoke coverage updates in:
  - `tests/integration/consistency.sh`
  - `tests/integration/native_codegen.sh`

### ADT type parsing/inference correctness fixes
- Fixed ADT constructor argument type parsing to use type-expression parser in `type ... = ... of ...`.
- Fixed type-parameter identity propagation in constructor type building:
  - constructor arg `'a` now maps to declared type parameter `'a` consistently.
- Fixed parser type-parameter AST nodes to retain parameter identifiers.

### Regression status (incremental)
- `make test-consistency`: pass
- `make test-native-codegen`: pass
