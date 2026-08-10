# Codex repository review

Date: 2026-08-10
Branch: `codex-review`

## Post-review disposition (2026-08-10, follow-up commit)

The landed review changes were verified regression-free against both tracked
lanes (default 11f/1410p byte-identical failure set; slot lane at its 15-real
baseline). The remaining findings were then triaged: genuine ones fixed, the
rest annotated inline below with a **DISPOSITION** note explaining why they
are not being adopted. Summary:

Fixed in the follow-up commit:

- **`(new C()).stateVar()` fold removed** (P0). The fold predated real child
  deployment and evaluated the initializer in the CALLER's context
  (`uint x = msg.value - 10` folded to the caller's `msg.value`). The
  `{value:}` variant never matched the fold's AST pattern and always took the
  faithful deploy-then-call path — proving it works. All fold-shape fixtures
  pass on-chain via the faithful path now.
- **Source locations** (P1). New `builder/SourceLocConvert` registers every
  compiled unit's `CharStream` (keyed by solc unit name, plus absolute-path
  aliases for entry files) and converts byte offsets to one-based lines at a
  single seam; all eight duplicate `makeLoc()` bodies now delegate to it. A
  node's own source unit selects the stream (imports get correct lines); the
  `file` field keeps the caller's readable path because puya opens it to
  excerpt source lines. Verified: a 42-line contract now caps at line 28
  (was byte offset 764).
- **`chainid` identifier boundary** (part of the SourceCompat P0): the rewrite
  now requires a word boundary on BOTH sides, so `chainidentifier` survives.
- **CLI numeric parsing**: all seven `std::sto*` call sites route through a
  checked whole-string `parseNumber()` that fails with the option named
  instead of an uncaught `std::invalid_argument` (or silently accepting
  `12abc`).
- **Unknown `--evm-version` is now fatal** (was warn-and-compile-as-cancun).
- **`awst.json` write is verified** (ofstream state checked; failure aborts
  before the backend can consume a truncated/stale file).
- **Harness compile isolation vs tracked artifacts**: the review's
  `compile-NNNN` isolation silently broke the repository's tracked-`out/`
  refresh — the per-test fixture wipes `out/<test>/` at setup, and the
  compile previously rewrote artifacts in place; after the isolation change
  the wipe just permanently deleted ~26k tracked files as tests ran. The
  isolated dirs are kept (the cache-contamination fix is real), and final
  artifacts are now ALSO mirrored to the test dir top level, restoring the
  refresh until the untracking decision is made explicitly.

## Scope and baseline

This was a repository-wide structural review with a deeper pass over `src/builder/`,
the CLI/backend boundary, and the semantic-test harness. The builder alone is about
57,600 lines across 226 C++ files; the largest translation units are
`EvmSlotLowering.cpp` (1,725 lines), `StorageDispatch.cpp` (1,681),
`SolAssignment.cpp` (1,520), and `TypeCoercion.cpp` (1,411). Those sizes and the
amount of cross-cutting mutable translation state make correctness regressions more
likely than the raw line count suggests.

The existing build completed before changes. CTest initially registered zero tests.
A separate warning-enabled build (`-Wall -Wextra -Wpedantic -Wshadow`) found a real
lifetime bug among a very large amount of warning noise, much of it from vendored
headers. The semantic suite is a localnet integration suite, so the review also added
small tests that can run without localnet.

## Highest-priority findings

### P0 — cached AST arguments were a dangling reference (fixed)

`SolFunctionCall::arguments()` returned `const&` to `m_call.arguments()`. In the
vendored solc AST API, `FunctionCall::arguments()` returns its vector **by value**, so
every use of the wrapper accessor read a destroyed temporary. This is undefined
behaviour and can manifest as nondeterministic miscompilation.

The wrapper now owns a cached vector, and GNU builds make
`-Werror=return-local-addr` fatal. This is the bug the warning-enabled build exposed.

### P0 — source compatibility can mutate unrelated source text and then continue
after a failed analysis

`src/cli/SourceCompat.cpp` rewrites Solidity with regular expressions and raw string
searches before parsing. These transforms do not understand comments, string
literals, tokens, scopes, or overloads. Concrete examples:

- `uint(-1)`, constructors, and pragmas inside comments/strings are rewritten.
- The `chainid` loop does not check a trailing identifier boundary, so an identifier
  such as `chainidentifier` can become `chainid()entifier`.
- event collection records only an event's name, not its canonical parameter types;
  `removeInheritedEvents()` can delete the wrong overload from any inheriting
  contract in the file.
- import scanning supports only a narrow direct `./...` form.

This is compounded by `reportCompilationErrors()` in
`src/cli/CompilerSetup.cpp`: it suppresses errors by matching English substrings, and
`main.cpp` proceeds even when `CompilerStack::parseAndAnalyze()` returned false if
only those strings remain. The resulting AST is not guaranteed to have the complete,
valid annotations on which the builder relies.

Recommendation: make legacy compatibility an explicit mode/profile, preserve modern
Solidity as the default, and perform compatibility work with solc's scanner/token
locations (or a narrowly scoped legacy-source migration pass). Never lower an AST
after failed analysis. If a diagnostic must be classified, use solc `ErrorId`, not
`what()` text. Add source-to-source golden tests covering comments, strings,
overloaded events, nested imports, and identifier boundaries.

> **DISPOSITION — partially adopted, remainder deliberate.** The `chainid`
> identifier-boundary bug is fixed (word boundary required on both sides).
> The rest of the compat layer stays as-is for now: it exists to replay REAL
> deployed 0.5.x–0.7.x verified sources through the chain-history differ,
> where "never lower after failed analysis" would simply drop that corpus —
> the suppression list is the price of admission, and the differ's
> transaction-level equivalence checking is the safety net that catches any
> mis-rewrite (a corrupted AST diverges from the EVM leg immediately and
> loudly). Agreed follow-ups, in order of value: classify suppressions by
> solc `ErrorId` instead of message substrings, and constrain rewrites with
> scanner tokens. Both are queued behind the semantic-suite work rather than
> done here, because every historical mis-rewrite so far surfaced as a
> compile error, not a silent miscompile.

### P0 — unsupported expressions can silently become valid-looking constants

The fallback in `src/builder/sol-ast/SolExpressionDispatch.cpp` warns about an
unsupported member access and returns zero, false, or empty bytes of a plausible
type. Similar stubs exist for unsupported EVM-only facilities. A later scan catches
only the special case where such a constant becomes an assignment target; a value
used in arithmetic, a condition, or a call can still compile and execute incorrectly.

Recommendation: make expression lowering return a typed result such as
`Expected<Expression, Diagnostic>` and treat unsupported constructs as compile errors.
If an intentional AVM divergence has a defined approximation, represent it with a
dedicated lowering node/policy and test it explicitly—do not share the unsupported
fallback.

> **DISPOSITION — no-go as a blanket policy.** A blanket warn→hard-error flip
> was already attempted on this codebase and REVERTED: it broke real-contract
> replays whose warned stubs are deliberate, documented AVM divergences (e.g.
> `chainid` and several EVM-introspection members degrade to warnings by
> design so legacy verified sources stay replayable). The negotiated position
> is case-by-case fail-loud promotion — several were promoted this way
> (constant-assignment-target scan in `main.cpp`, the `mstore` length-word
> fail-loud, blob value-use hard errors) and more get promoted whenever a stub
> is caught masking a real divergence. The `Expected<>`-style typed lowering
> is good architecture but belongs to the `CompilationSession` refactor, not a
> point fix.

### P0 — `(new C()).stateVar()` is folded by removing deployment semantics

`SolExternalCall::toAwst()` folds `(new C()).stateVar()` to the declaration's literal
initializer. This skips child creation and constructor execution entirely. It is
wrong when constructor arguments or constructor code change the variable, and it
also removes the observable deployment side effect.

Recommendation: remove the fold. Either implement the child getter call faithfully
or reject this shape until the child program can return the value. A warning does not
make this semantics-preserving.

> **DISPOSITION — fixed in the follow-up commit.** Fold removed; the faithful
> deploy-then-call path (which the `{value:}` variant always used, since
> `FunctionCallOptions` never matched the fold's pattern) handles the shape.
> All fold-shape fixtures verified on localnet.

### P1 — declaration-sensitive calls used source-order arguments (fixed)

Solidity named arguments are stored in source order while parameter types and members
are in declaration order. Several paths zipped `FunctionCall::arguments()` with
declaration-order metadata, so disordered named arguments could be encoded or coerced
as the wrong parameter. The affected paths included external calls, child
constructors, struct construction, events, custom `revert`, custom errors passed to
`require`, and the storage-reference parameter analysis.

These paths now use solc's `FunctionCall::sortedArguments()`. Struct construction was
simplified at the same time, removing a second hand-written name-to-field loop. The
named custom-error test now asserts the complete payload rather than merely checking
that a revert occurred.

### P1 — source byte offsets are serialized as line numbers

Most `makeLoc()` implementations copy `langutil::SourceLocation::start/end` into
AWST `line/endLine` (for example `ContractBuilder.cpp`, `ContractContext.h`,
`sol-ast/Context.h`, `AWSTBuilder.cpp`, and `StorageMapper.cpp`). Solc positions are
byte offsets; puya's `SourceLocation` is one-based line/column data and uses it to
slice source lines. Diagnostics and generated source maps are therefore inaccurate.

Recommendation: centralize location conversion and use
`CompilerStack::charStream(sourceName).translatePositionToLineColumn()`. Populate
one-based lines and columns, preserve the actual source-unit name per AST node, and
delete the duplicate `makeLoc()` implementations. This is a direct opportunity to
use solc instead of hand-rolling location conversion.

> **DISPOSITION — fixed in the follow-up commit** via `builder/SourceLocConvert`
> (CharStream registry + one conversion seam; all eight `makeLoc()` bodies
> delegate). One deliberate deviation: the `file` field keeps the caller's
> readable absolute path rather than the node's unit name — puya OPENS that
> path to excerpt source lines, and unit names are not readable paths. The
> node's unit still selects the correct stream, so imported nodes get correct
> line numbers.

### P1 — backend invocation used a shell command (fixed)

`PuyaRunner` built an unquoted command and passed it to `std::system()`. Compiler
controlled paths containing spaces failed, and `--puya-path`/output paths were shell
injection surfaces. The runner now uses `fork` plus `execlp` with distinct arguments,
waits through `EINTR`, and preserves normal exit/signal status. A CTest regression
covers spaces and shell metacharacters.

### P1 — output handling can mix stale and current artifacts (partially fixed)

`main.cpp` writes directly into a reusable output directory, does not check the
`ofstream` state for `awst.json`, and relies on later consumers to distinguish stale
contract artifacts. It also used stale `.bin` files to generate deployment templates
after a backend failure. Unreadable secondary `--source` files were silently skipped.

This branch stops on unreadable secondary sources and only writes child deployment
templates after backend success. The stronger follow-up is to compile into a staging
directory, validate every write/backend artifact, then atomically publish the result.
At minimum, maintain and remove a manifest of files produced by the previous run.

> **DISPOSITION — partially adopted.** The follow-up commit verifies the
> `awst.json` write (stream state checked, failure aborts before the backend
> runs). Full staging-directory publishing is deferred: the semantic harness
> already compiles into per-test directories and the compile cache validates
> its own entries, so the remaining exposure is the manual-CLI path only.

## Builder design and refactoring recommendations

### Replace side-channel statement queues with an explicit lowering result

Expression lowering communicates through `prePendingStatements`, pending statements,
post statements, and several one-off context fields. Correctness depends on every
caller draining them at exactly the right boundary. Existing comments and checks show
that this has already caused ordering/leak bugs.

Use an explicit value such as `{prelude, value, epilogue}` (or an AWST expression
sequence node) returned from lowering. Composition can then define evaluation order
locally. This should reduce special drains across assignments, calls, loops, emits,
and inline assembly.

### Make a compilation session own all mutable state

The builder still has process-wide/thread-local state: EVM version and layout flags,
storage-reference registries, parameter-mutation caches, child-contract sets, pending
Yul subroutines, and several function-local static counters. Some are manually reset,
some assume one compile per process, and some are contract-scoped. This prevents safe
reuse, parallel compilation, and deterministic library embedding.

Create a `CompilationSession` owned by `AWSTBuilder`; put target profile, name
generation, registries, diagnostics, type arena, and per-compilation caches there.
Pass contract/function views down from it. The session boundary also provides the
right place to reset state exactly once.

### Consolidate the two expression-builder systems

`sol-ast` dispatches first to specialized wrappers and then falls back to the older
`sol-eb` instance builders. That makes feature ownership and error policy unclear and
encourages duplicate coercion/call/member-access logic. Inventory every fallback,
move one Solidity type family at a time behind a single typed lowering interface, and
delete the old path once coverage is equivalent.

### Split files by semantic operation, not by accumulated exception

The largest files mix dispatch, representation policy, coercion, storage layout, and
special-case compatibility. Useful seams are already visible:

- split `EvmSlotLowering` into address calculation, scalar codec, aggregate codec,
  and reference lowering;
- split `StorageDispatch` into layout planning and AWST emission;
- make assignment targets a first-class lvalue abstraction, then separate target
  resolution from value coercion/write-back;
- centralize call binding/evaluation, leaving internal/external/new-call modules to
  implement only transport and return handling.

This is more valuable than mechanically shortening files: it removes parallel rules
that currently drift.

### Give WTypes explicit ownership

Builder code allocates `WTuple`, `ARC4UIntN`, and related types with raw `new` at many
sites while other code uses `TypeMapper::createType`. The ownership/lifetime contract
is implicit and difficult to audit. Route all non-singleton WTypes through one
session-owned interning/arena API. Besides avoiding leaks, interning gives stable type
identity and removes ad-hoc `new` conventions from passes.

### Model target semantics as a profile

Global booleans such as EVM storage/memory layout and scattered `viaYul`/AVM
divergence branches create a combinatorial test surface. Define an immutable target
profile (storage model, memory model, ABI, selector hash, call capabilities, EVM
version), validate supported combinations once, and inject strategy objects into the
relevant lowerers.

## Where more solc should be reused

### Adopt now

- **Source locations:** use `CharStream::translatePositionToLineColumn()` as described
  above.
- **Token-aware compatibility:** use solc scanner tokens and locations to constrain
  legacy rewrites to actual syntax. A full parse is preferable where possible.
- **Diagnostic identity:** match the stable `Error::errorId()` values if compatibility
  diagnostics truly need classification; do not match localized message strings.
- **Named-argument binding:** this branch adopts `FunctionCall::sortedArguments()` in
  the remaining declaration-sensitive paths.
- **EVM versions:** this branch replaces the duplicated version-name map with
  `EVMVersion::fromString()`, so support tracks the vendored compiler.

### Evaluate as a separate project

- A conservative subset of Yul `OptimiserSuite` could canonicalize inline assembly
  before AVM lowering and eventually replace some local constant/memory tracking.
  The known blocker is that inline-assembly `externalReferences` point into the
  original Yul AST; rewrite those references by name after disambiguation before
  enabling transformations.
- `yul::SideEffectsCollector` can classify whole Yul expressions/blocks. The project
  already uses `evmasm::SemanticInformation` for instruction effects; extending the
  solc-provided analysis would reduce another hand-maintained effect layer.
- SMTChecker and NatSpec `@custom:` tags are useful product features, but they are
  lower priority than correctness and fail-loud lowering.

### Do not substitute blindly

Solc IR generation, EVM code generation, and `libevmasm` optimization are coupled to
EVM bytecode and are not a replacement for AWST lowering. Likewise, solc's Keccak
selectors and EVM-canonical type spellings cannot replace the project's deliberate
ARC-4/sha512_256 wire conventions. Reuse solc's typed AST and semantic facts; retain
the explicitly different AVM transport.

`possible_solc.md` contains a useful prior survey, but its opening claim that none of
the listed facilities are referenced is now stale because most Tier 1 items are
marked adopted later in the same document. Convert it to a short status table or
fold the still-open items into the normal issue tracker.

## CLI and build-system findings

- `CliOptions.cpp` uses unchecked `stoi/stoul/stoull`; malformed input terminates with
  an uncaught exception. Missing values often become a generic “unknown option”,
  malformed `--ensure-budget` can be ignored, unknown log levels silently become
  `info`, and numeric ranges are not validated. Introduce a small declarative option
  parser or central `parseNumber()` returning diagnostics; make parsing return a
  result instead of calling `exit()`.

  > **DISPOSITION — core fixed.** All seven `std::sto*` sites now go through a
  > checked whole-string `parseNumber()` that names the offending option. The
  > declarative-parser / no-`exit()` refactor is deferred as ergonomics: this
  > is a CLI entry point, `exit(2)` with a clear message is acceptable there.

- An unknown EVM version currently warns and silently compiles as Cancun. Invalid
  target names should be fatal because they change accepted syntax and opcode
  semantics.

  > **DISPOSITION — fixed.** Unknown `--evm-version` is now a fatal error.
- CMake hard-codes `$HOME/.local/boost-1.83`, disables system search, manually lists
  every source, and links vendored static libraries by path. Use imported targets and
  an optional Boost hint/toolchain setting. Separate a reusable frontend library from
  the CLI executable so builder unit tests can link a small target.

  > **DISPOSITION — deferred, not disputed.** Single-developer, single-machine
  > project today; the pinned Boost path IS this environment's install and the
  > manual source list has never actually drifted (the build fails loudly when
  > it does). Modernizing buys nothing until a second environment or CI
  > appears, and touching the build system risks the only known-good setup
  > mid-campaign. The frontend-library split is the one piece with immediate
  > value (unit-testing builder passes) and should ride along with the
  > `CompilationSession` refactor.
- The README previously recommended `CMAKE_CXX_FLAGS="-w"`; this branch removes that
  suppression and documents CTest. Mark vendored include paths `SYSTEM`, then enable a
  useful project warning baseline in CI. The warning build produced too much vendored
  noise to be actionable as-is.

## Test-suite findings

### Correctness and isolation fixes on this branch

- Each `Harness.compile()` now gets `compile-0001`, `compile-0002`, etc. Previously,
  multiple compiles in one test shared a directory; cache stores copied every file in
  that directory, allowing artifacts from an earlier contract to contaminate a later
  cache entry. Passing tests clean only these isolated directories, preserving the
  repository's legacy tracked output until it can be untracked separately.
- The puya source signature now hashes git status records as well as file contents.
  Deleted and renamed backend files previously did not invalidate the compile cache.
- Constructor argument counts are validated, and post-init coercion works on a copy
  instead of mutating the caller's list.
- The named-error test now verifies selector plus ordered ARC-4 payload.

### Remaining test-infrastructure work

- There is no root or semantic-suite dependency manifest for pytest, algosdk,
  algokit-utils, xdist, and rerun plugins. `puya/.venv` is not sufficient for the
  semantic harness. Add a locked test environment and one documented command.
- CTest previously had no tests; this branch adds a runner regression, but pure C++
  builder/CLI utilities still have no unit-test target. Add fast tests for option
  parsing, source compatibility, source locations, type coercion, signature binding,
  and serialization. Keep localnet tests as a separate integration tier.
- There are 112 `xfail` sites and no `strict=True` occurrences. Non-strict XPASS does
  not fail CI, so fixed cases can stay hidden indefinitely. Make known limitations
  strict and keep open compiler bugs as ordinary failures or a separately reported
  quarantine.

  > **DISPOSITION — no-go by standing project policy.** XPASSed tests
  > deliberately STAY xfailed here (owner's explicit decision): many xfails
  > cover behavior that is localnet-timing- or budget-marginal, and
  > `strict=True` would turn those into flaky hard failures. Drift is not
  > hidden — every gate run records the xpass count (currently 32/34 per
  > lane) and any movement in it is investigated, which is how several
  > "stale folklore" xfails were retired (e.g. the V4 tick-crossing 256-cap).
- The repository tracks 25,929 files under
  `tests/solidity-semantic-tests/out/` plus 231 historical result text files (about
  99 MB). The checkout currently uses about 4.6 GB for `tests/`; semantic output is
  about 1.0 GB and its ignored compile cache about 2.4 GB. Git objects and submodule
  objects add substantial clone cost. Keep small curated golden files, move run
  reports to CI artifacts, untrack generated output, and add `out/` to `.gitignore`.

  > **DISPOSITION — owner decision pending; convention restored meanwhile.**
  > Reasonable proposal, but the tracked artifacts are part of the current
  > review workflow (TEAL diffs ride along in commits, which is how several
  > codegen regressions were caught in review). Note the review branch's
  > compile-isolation change had already half-forced this decision by
  > silently deleting tracked artifacts at test runtime — that interaction is
  > fixed (isolated compile dirs kept for cache hygiene, artifacts mirrored
  > back). Untracking remains available as an explicit choice.
- Cache entries are unbounded. Add a size/age policy and a cache schema version so
  harness-format changes cannot reuse structurally incompatible artifacts.
- Full-suite pass totals are maintained in large narrative/result files. Generate a
  compact machine-readable summary and compare it in CI; link failures to the
  retained CI artifact instead of committing raw output repeatedly.

## Suggested order of follow-up work

1. Remove the `(new C()).stateVar()` fold and turn unsupported value fallbacks into
   hard diagnostics.
2. Stop lowering after any solc analysis error; isolate legacy compatibility behind
   an explicit mode and make its transformations token-aware.
3. Fix source locations with solc `CharStream`, with a serializer/backend regression.
4. Introduce `CompilationSession` and migrate global registries/counters into it.
5. Replace pending-statement side channels with an explicit composable lowering
   result, starting with function-call arguments and assignments.
6. Establish fast C++/Python CI tiers and remove tracked generated test output.
7. Split the four largest semantic modules along the seams above and centralize WType
   ownership.

## Changes made on `codex-review`

- fixed the solc AST argument lifetime bug and added a fatal compiler diagnostic;
- used solc for EVM version parsing and named-argument ordering;
- removed shell parsing from backend invocation and added a CTest regression;
- hardened source/output failure handling in `main.cpp`;
- fixed semantic harness cache invalidation, compile isolation, and argument
  mutation/count handling;
- strengthened the named custom-error regression;
- removed warning suppression from the documented build and added the first CTest
  target.

The pre-existing dirty `solidity` submodule was deliberately left untouched and is
not part of the review commit.

## Validation performed

- `cmake --build build -j2` — passed after a clean dependency scan/rebuild.
- `ctest --test-dir build --output-on-failure` — 1/1 passed.
- focused Python harness tests — 3/3 passed with third-party plugin autoload disabled.
- `py_compile` over all modified Python files — passed.
- frontend-only compilation of disordered named calls, named custom errors, named
  struct construction, and a named event fixture — all passed. The custom-error AWST
  contains values in declaration order (`2`, then `7`).
- `git diff --check` — passed.

The full semantic suite was not run: it is a long-running Algorand localnet integration
suite, and no localnet was provisioned as part of this review. The strengthened named
custom-error payload assertion therefore still needs its normal localnet CI run.
