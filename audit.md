# puya-sol project audit

**Audit date:** 2026-09-03  
**Audited revision:** `0018b06f9ad87d924ae5984f179cf0b59e42ece8` (`main`)  
**Working tree:** reviewed as found, including a pre-existing dirty `solidity` submodule  
**Auditor:** OpenAI Codex

> **Post-audit remediation (2026-09-03):** The ARC4-specific part of H-01 was
> subsequently removed from the vendored Solidity frontend. ARC4 is now exposed
> through `libs/AVM.sol` and lowered by puya-sol using ordinary Solidity call
> envelopes. A clean `libsolidity` archive, puya-sol, the focused regression
> test, and end-to-end Puya backend fixtures all built successfully. `Bits.bitlen`
> is also now a compiler-recognized AVM stdlib facade, so importing `AVM.sol` no
> longer requires Solidity's Osaka EVM target; the fixtures build with the
> compiler's default Cancun target. The nested `fmtlib` and `nlohmann-json`
> checkouts have since been synchronized to the gitlinks recorded by Solidity,
> leaving the entire Solidity submodule tree clean. CMake now owns an isolated,
> out-of-source Solidity build, rejects dirty or mismatched recursive submodules,
> uses one coherent Boost 1.83+ installation, and verifies the generated Solidity
> commit identity before linking. A fresh external Release configure/build and a
> relocated install both succeeded without consulting `solidity/build/`. The
> build now emits a manifest containing the exact root/submodule/toolchain
> identities and SHA-256 hashes for the compiler and staged AVM stdlib. A pinned
> GitHub Actions workflow starts from a recursive checkout, performs that fresh
> build, runs CTest, checks source-tree cleanliness, and uploads the manifest;
> its first hosted run is still pending. These changes were committed as
> `53a982bc97`. The concrete H-01 source/build provenance failures and the
> per-commit portion of M-01 are remediated; scheduled semantic/localnet gates,
> artifact reproducibility, SBOMs, and dependency scanning remain outstanding.
>
> H-04 now fails closed: policy-classified AVM adaptations and hard runtime
> divergences are compile errors regardless of log filtering unless each stable
> divergence name is acknowledged with a repeatable `--allow-divergence` option.
> There is deliberately no catch-all opt-in, denied builds do not emit AWST, and
> black-box coverage exercises block fields, balance units, native transfers,
> static/low-level calls, delegate calls, self-calls, and try/catch. The semantic
> research harness opts into its measured adaptations explicitly. H-05's stale
> scratch-layout test now asserts the contiguous layout and its 1/88-slot
> boundaries; CLI parsing/help share that bound and invalid values exit 2 rather
> than abort. H-06 now uses one strict, exception-safe hex decoder and black-box
> tests cover malformed nibble, length, prefix, and placeholder cases. M-02's
> removed splitter flags and claims have been deleted from the live CLI and docs,
> with regression tests requiring every former option to be rejected. A final
> empty-directory Release build passed all 11 CTest targets; the Solidity tree
> remained clean and `git diff --check` passed.
>
> H-02 is now remediated: entry, additional, and imported source units are
> passed byte-for-byte to the pinned Solidity frontend by default, so their
> original version constraints are enforced. The compatibility transforms are
> available only through the explicit research-only
> `--legacy-source-rewrite` option, whose warning bypasses log filtering and
> whose manifest records every source unit's exact original/transformed text
> and both Keccak-256 hashes, including on a failed analysis. The semantic and
> chainwide research harnesses opt in explicitly and no longer present changed
> sources as ordinary compilation. H-03 is also remediated:
> `--evm-memory-layout` and the `--evm-layout` umbrella now exit 2 before source
> processing, even under `--log-level error`; help and live research tooling
> direct storage-only use to the implemented `--evm-storage-layout` option.
> A final empty-directory Release build and all 13 CTest targets passed, the
> focused Python cache/harness suite passed 3/3, the recursive Solidity
> submodules remained clean, and `git diff --check` passed. All six High
> findings are therefore addressed in commit `0d73752ddc`; the audit's
> historical evidence and original verdict below remain unchanged.
>
> **Follow-up correction (2026-09-04):** H-04's native-value-transfer
> enforcement is not complete. The ordinary `transfer`, `send`, and
> `call{value: ...}` lowerings use the shared payment builder, which either maps
> the receiver through the configured xchain account model or requires the
> explicit `native-value-transfer` divergence. However, high-level
> `selfdestruct(beneficiary)` (`src/builder/sol-eb/BuiltinCallables.cpp`) and
> inline-assembly `call(gas, target, value, ...)`
> (`src/builder/assembly/PrecompileDispatch.cpp`) construct payment fields
> directly, bypassing both that policy check and xchain receiver mapping. Both
> paths were reproduced emitting EVM-profile output without an xchain template
> or divergence opt-in. The native-value portion of H-04 is therefore reopened;
> it needs one mandatory receiver/policy boundary shared by every `Amount` and
> `CloseRemainderTo` lowering. The earlier statement that all six High findings
> were addressed does not apply to this newly identified coverage gap.
>
> M-03 is remediated in commit `34e65cf0dd`. Frontend JSON and deployment
> templates are now validated, written through verified sibling temporary
> files, and atomically renamed. Each run invalidates its previous completion
> marker and current-target backend outputs; expected child and primary backend
> programs are mandatory and size-checked. A phase-tagged artifact manifest
> records byte lengths and SHA-256 digests, and success is reported only after
> complete backend validation. The persistent-server and backend-cache paths
> enforce the same contract. The full build, all 14 CTest targets, seven focused
> Python tests, and real frontend/backend failure-and-success smoke tests passed;
> recursive Solidity submodules remained clean.

## Executive summary

`puya-sol` is an ambitious, fast-moving proof-of-concept compiler with several good fail-closed controls, but it is not reproducible from its repository and is not safe to use for production contracts or real funds. That conclusion agrees with the warning already placed prominently in `README.md`.

The most serious newly verified issues are:

1. A clean checkout cannot reproduce the current compiler. First-party code depends on uncommitted changes inside the `solidity` submodule, and the build also depends on ignored, prebuilt Solidity archives.
2. Every input source has its Solidity version pragma silently rewritten. A contract explicitly requiring `<0.8.0` is accepted under a newer frontend, which can change language semantics such as overflow behavior.
3. `--evm-memory-layout` and the memory half of `--evm-layout` are advertised as implemented, full EVM behavior, but the implementation explicitly does nothing. Compilation still succeeds, and the warning disappears at `--log-level error`.
4. Other security-relevant EVM divergences, including loss of the `staticcall` read-only guarantee, also produce successful output and can be hidden by log filtering.
5. The native test gate is red, and the same scratch-layout mismatch lets a CLI value accepted by the parser abort the compiler with an uncaught exception.
6. Malformed xchain template hex can be partially accepted or can abort the compiler. This parser protects configuration used to derive accounts that may receive funds.

No direct remote-code-execution path, embedded credential, or bypass of the reviewed EVM entry/xchain caller-claim checks was found. This is not a statement that none exists: this audit did not formally verify the compiler or exhaustively validate generated TEAL.

### Verdict

**Do not use this compiler or its output for MainNet, custody, production authorization, or real assets.** Before even experimental release packaging, resolve H-01 through H-06 and establish a clean, continuously enforced build-and-test baseline.

## Severity summary

| Severity | Count | Meaning in this audit |
|---|---:|---|
| Critical | 0 | Direct, readily exploitable loss or compromise found during this review |
| High | 6 | Material miscompilation/security-property risk, funds-sensitive validation defect, or release blocker |
| Medium | 6 | Significant reliability, assurance, artifact-integrity, or maintainability weakness |
| Low | 1 | Limited operational/diagnostic weakness |

## Scope and method

The review covered the approximately 76,000 lines in 302 first-party C++ source/header files, the compiler pipeline, source preprocessing, CLI, target profiles, ABI entry routing and validation, EVM feature policy, xchain account handling, child deployment artifacts, backend process invocation, native tests, Python semantic-test harness, CMake configuration, documentation, repository state, and generated-artifact hygiene.

The `solidity` and `puya` submodules were reviewed as dependencies and at their integration boundaries. Their complete upstream implementations were not independently line-audited.

Methods used:

- Manual source and data-flow review of security- and correctness-sensitive paths.
- Repository/submodule provenance inspection and comparison of tracked versus working-tree Solidity sources.
- Existing build and native test execution.
- Focused Python unit tests for policy and test-harness/cache logic.
- Focused negative-input and semantic-divergence reproductions using temporary contracts under `/tmp`.
- Targeted `clang-tidy` sampling and pattern scans for unsafe process invocation, numeric parsing, unfinished paths, credentials, and ignored errors.
- Documentation, configuration, dependency-pinning, and repository-hygiene review.

### Important limitations

- The approximately 45-minute localnet semantic suite was not rerun. It requires a running AlgoKit localnet. The latest checked-in consolidated result is reported below, but it predates substantial changes.
- Generated TEAL was not formally verified, fuzzed, symbolically executed, or deployed to a network during this audit.
- A live dependency/CVE database audit was not performed. No claim is made that the vendored or locked dependencies are free of known vulnerabilities.
- The whole-tree `clang-tidy` pass was stopped because processing all translation units through the large Solidity headers was not completing in a reasonable audit window. A targeted `src/main.cpp` pass completed without a diagnostic. Manual review and compiler/test execution were the primary methods.
- Findings are against the dirty workspace, not a clean release artifact. H-01 explains why those are presently different products.

## Validation results

| Check | Result |
|---|---|
| `cmake -S . -B <fresh-dir> -DCMAKE_BUILD_TYPE=RelWithDebInfo` | Pass in this machine-specific environment; it found Boost only at the hard-coded user-local path and reused the ignored Solidity build tree |
| `cmake --build build -j4` | Pass for the existing configured workspace |
| `ctest --test-dir build --output-on-failure` | **Fail:** 4/5 passed; `puya-sol-scratch-layout` aborted |
| Focused Python policy/harness/cache unit tests | 47 passed; 2 marker warnings, after disabling an unrelated globally installed pytest socket-using plugin |
| `git diff --check` | Pass |
| Minimal Solidity-to-AWST compile with `--no-puya` | Pass |
| `--evm-memory-slots 89` | **Abort, exit 134:** parser accepts it, `ScratchLayout` throws later |
| Malformed `--xchain-template gg` | **Abort, exit 134:** uncaught `std::invalid_argument` from `std::stoul` |
| `--pure-helper-split value:1` without the removed splitter | **Exit 0:** option is silently ignored |
| `--evm-memory-layout --log-level error` | **Exit 0 with no diagnostic:** the option has no effect |
| Contract using `address.staticcall`, normal logging | **Exit 0:** reports loss of read-only semantics and uncatchable failure only as warnings |
| Same `staticcall` contract with `--log-level error` | **Exit 0 with no diagnostic** |
| Source with `pragma solidity <0.8.0` | Vendored `solc` rejects it; `puya-sol` silently rewrites and accepts it, exit 0 |
| First-party credential-pattern scan | No credential material found; matches were descriptive `private_key` references in committed test-result transcripts |

The checked-in semantic status at `tests/solidity-semantic-tests/CURRENT.md:3` is **8 failed, 1,445 passed, 113 xfailed, and 32 xpassed** from 2026-08-19. There have been 115 commits since 2026-08-20. The README still claims an older **1,083/1,322 (82%)** result.

## Detailed findings

### H-01 — The current compiler cannot be reproduced from a clean checkout

**Severity:** High  
**Category:** Build provenance / supply chain / release integrity

#### Evidence

- The root repository pins `solidity` at `a99b6d8c0cbf9eddbac104e8e4e16545db7d3d8d`, but `git status` reports the submodule as dirty.
- The dirty submodule contains changes to seven Solidity frontend files adding the `arc4` magic namespace and `MagicType::Kind::ARC4`. It also has two nested submodules at commits different from their recorded gitlinks.
- First-party tracked code directly references that uncommitted enum in `src/builder/sol-ast/calls/SolAbiEncode.cpp:14` and `SolAbiDecode.cpp:13`.
- `git show HEAD:libsolidity/ast/Types.h` inside the pinned submodule contains no `ARC4` member. Resetting/updating the submodule to the recorded revision therefore makes first-party compilation fail.
- `CMakeLists.txt:237-247` directly links six archives under `solidity/build/`. That directory is ignored by the Solidity repository, absent from a clean clone, and the README gives no command that builds it.
- The current ignored Solidity build tree is internally inconsistent: its `solc` binary identifies `0.8.28`/commit `7893614a`, while generated `BuildInfo.h` identifies `0.8.35`/commit `a99b6d8c`; archive timestamps span several months. The `solc` executable is not linked into `puya-sol`, but the mismatch demonstrates that this directory is not a coherent release artifact.

#### Impact

Developers and CI cannot recreate the reviewed binary from source. An ordinary `git submodule update --init --recursive` can remove required behavior. Builds can silently combine headers and static libraries from different source states, undermining every test result and making incident response or binary attestation impractical.

#### Recommendation

1. Commit the ARC4 frontend work to a maintained Solidity fork and update the root gitlink to that exact commit.
2. Update and commit the intended nested `fmt` and `nlohmann-json` gitlinks.
3. Add a documented bootstrap target/script that configures and builds Solidity from the pinned source before configuring `puya-sol`; preferably use CMake targets rather than raw archive paths.
4. Make configuration fail early if expected libraries or their generated build identity do not match the pinned revision.
5. Add CI that starts from a fresh clone with no pre-existing build/cache directories and verifies a reproducible version manifest.

### H-02 — Solidity version constraints are silently discarded or inverted

**Severity:** High  
**Category:** Semantic correctness

#### Evidence

`src/cli/SourceCompat.cpp:169-197` finds the first major/minor number in every Solidity pragma and replaces the entire constraint with `pragma solidity >=<major>.<minor>.0;`.

Examples of the transformation include:

- `pragma solidity =0.5.16;` → `pragma solidity >=0.5.0;`
- `pragma solidity >=0.7.0 <0.8.0;` → `pragma solidity >=0.7.0;`
- `pragma solidity <0.8.0;` → `pragma solidity >=0.8.0;`

`src/main.cpp:101-118` applies the transform to the entry source and every imported source; lines 124-138 apply it to additional CLI sources. There is no opt-out, warning, or emitted transformed-source record. `tests/cpp/SourceCompatTests.cpp:21-34` explicitly enshrines pragma relaxation.

The audit reproduction used a contract containing `pragma solidity <0.8.0`. The vendored `solc` rejected it as incompatible, while `puya-sol` silently accepted and lowered it with exit 0.

#### Impact

Solidity language semantics are version-dependent. Most notably, pre-0.8 arithmetic wraps by default while 0.8+ arithmetic checks overflow, but parsing, ABI, fallback/receive, constructor, and code-generation behavior also changed across releases. A source can therefore compile successfully into behavior that neither its author nor its declared compiler range permits. This is a compiler-level integrity failure, not only a compatibility convenience.

#### Recommendation

- Do not alter version pragmas by default. Reject incompatible sources or select a genuinely compatible frontend.
- If legacy rewriting remains useful for corpus research, require an explicit `--legacy-source-rewrite`-style opt-in, print a high-visibility diagnostic, preserve the original constraint, and write the exact transformed source plus hashes into the output manifest.
- Test behavior across each claimed Solidity language version, especially checked arithmetic and ABI transitions. Do not describe rewritten test-corpus coverage as source-level Solidity equivalence.

### H-03 — “Full EVM memory layout” options are accepted but do nothing

**Severity:** High  
**Category:** Unsafe feature contract / miscompilation risk

#### Evidence

- CLI help at `src/cli/CliOptions.cpp:126-132` describes `--evm-layout` as “FULL EVM data-location semantics,” calls it recommended for assembly-heavy contracts, and describes `--evm-memory-layout` as a universal flat-blob model.
- `src/main.cpp:185-194` explicitly states that `--evm-memory-layout` and the memory half of `--evm-layout` “currently changes NOTHING”; no target-profile field carries the setting.
- Compilation nevertheless continues and returns success. With `--log-level error`, the warning is suppressed. The audit reproduced a successful, completely silent compile with the ineffective option.

#### Impact

Users can reasonably treat the successful command and help text as assurance that EVM pointer/alias/layout behavior is enabled. Assembly-heavy code may then be lowered under call-site-specific AVM memory models instead. The result can compile and deploy while memory aliases, offsets, or serialization behavior differ from the source contract.

#### Recommendation

Immediately make both options hard errors on `main` until their advertised behavior is implemented. When implementation resumes, carry the mode in `TargetProfile`, test every lowering consumer, and add EVM differential tests for aliasing, cross-page operations, assembly pointers, nested aggregates, internal/library calls, and return-data copies. Do not use “full” or “recommended” in help until those tests are enforced.

### H-04 — Security-relevant EVM adaptations can compile successfully and silently

**Severity:** High  
**Category:** Semantic safety policy

#### Evidence

`src/builder/EvmFeaturePolicy.cpp:101-125` emits an error only for `HardCompileError`; `AvmAdaptation`, `ConfiguredEnvironment`, and even `HardRuntimeFailure` are warnings. `--log-level error` hides those warnings without changing the exit status.

The audit compiled a contract using `address.staticcall`:

- Normal logging returned exit 0 while warning that the AVM call is not read-only and that a failed inner call aborts instead of returning `false`.
- `--log-level error` returned exit 0 with no diagnostic.

Other material divergences are accurately documented in `EVM_DIVERGENCE.md`, including:

- `staticcall` becoming an ordinary potentially state-mutating inner application call.
- `this.f()` becoming a subroutine call that retains the original `msg.sender` and `msg.value`.
- Failed/codeless low-level calls behaving differently and not exposing a catchable `false` result.
- Reentrancy being unavailable.
- Native value transfer in EVM profile without xchain accounts sending funds to a keyless projected address.

#### Impact

These differences can invalidate authorization, read-only-call, error-handling, accounting, and payment assumptions inherited from audited Solidity. A successful compiler exit is easy to consume in automation as “safe to deploy,” especially when warning output is filtered.

#### Recommendation

- Add a strict-equivalence mode and make it the default for any release-oriented command. In that mode, every semantics-changing adaptation must be a compile error.
- Require granular, explicit opt-ins such as `--allow-divergence staticcall` rather than one broad unsafe switch.
- Emit a machine-readable divergence manifest into every artifact set and make downstream deployment tooling reject unapproved entries.
- Keep the existing documentation, but do not rely on documentation or suppressible logs as the enforcement boundary.

### H-05 — Native tests are red and the scratch-slot contract aborts on parser-approved input

**Severity:** High  
**Category:** Correctness / test gate / availability

#### Evidence

- `ctest` passes four tests and aborts `puya-sol-scratch-layout`.
- `ScratchLayout::maxMemorySlots` is 88 in `src/builder/ScratchLayout.h:38-45`.
- CLI parsing still accepts 1 through 240 at `src/cli/CliOptions.cpp:257-259`.
- The stale test expects the old relocated layout and constructs `ScratchLayout{240}` at `tests/cpp/ScratchLayoutTests.cpp:31-49`.
- `src/main.cpp:213-216` constructs `ScratchLayout` outside an exception boundary.
- The audit passed `--evm-memory-slots 89`. Parsing and type-checking succeeded, then the process terminated on an uncaught `std::invalid_argument` with exit 134.

#### Impact

The repository has no green native baseline, and values accepted by the CLI can crash the compiler instead of producing a controlled validation error. More importantly, a stale test in one of only five native CTest targets indicates that current layout changes are not being continuously integrated.

#### Recommendation

Use `ScratchLayout::maxMemorySlots` as the parser bound and source of help text; remove all duplicate numeric limits. Update the test to the contiguous model and assert boundary behavior at 1, 88, 0, and 89. Add a top-level exception boundary as defense in depth, plus black-box CLI tests that require deterministic exit code 2 and no abort/core dump for malformed input.

### H-06 — Xchain template hex validation is not strict or exception-safe

**Severity:** High  
**Category:** Funds-sensitive configuration validation

#### Evidence

The xchain decode lambda in `src/main.cpp:223-235`:

- Removes only a lowercase `0x` prefix.
- Checks only even length.
- Calls `std::stoul(twoCharacters, nullptr, 16)` without checking how many characters were consumed.
- Does not catch `std::invalid_argument` or `std::out_of_range`.

Consequently, a pair such as `0g` is partially parsed as zero, while `gg` aborts the process. The audit reproduced the latter with exit 134. This contradicts the adjacent invariant at lines 218-220 that any template decode/placement error must fail the compile rather than risk funds.

#### Impact

The parsed template is used to derive the LogicSig account to which EVM-profile payments are routed. Partial parsing can derive an address from bytes other than the operator intended; a mismatch between the compiled derivation and deployed LogicSig template can make routed funds inaccessible. Fully invalid input also causes a denial-of-service-style compiler abort.

#### Recommendation

Reuse a single strict hex decoder for all CLI address/template inputs. Require every character to be hexadecimal, accept prefixes consistently, validate full consumption and expected size constraints, catch conversion errors, and return a stable nonzero CLI status. Add negative tests for empty input, odd length, invalid first/second nibble, mixed-case prefix, huge input, duplicated/missing placeholder, and boundary template sizes. Where possible, accept a binary template file and record its hash instead of a long hex argument.

### M-01 — There is no continuously enforced, current compiler baseline

**Severity:** Medium  
**Category:** Release assurance

#### Evidence

- There is no root `.github` workflow or equivalent checked-in CI configuration.
- The latest consolidated semantic result in `CURRENT.md` is from 2026-08-19 and still includes 8 failures, 113 xfails, and 32 non-strict xpasses.
- There have been 115 commits since 2026-08-20, many changing memory, ABI, calls, storage, proxies, and authorization-sensitive behavior.
- `README.md:29-31` reports a much older 1,083/1,322 result.
- The only five native tests are declared at `CMakeLists.txt:255-295`, and one currently aborts.
- The full semantic suite requires mutable localnet infrastructure and approximately 45 minutes, making it unsuitable as the only meaningful gate.

#### Impact

There is no trustworthy answer to “what does current HEAD pass?” and no automatic barrier against committing a compiler miscompilation or broken clean build. Non-strict xpasses and hundreds of historical result files further obscure what is expected versus accidental.

#### Recommendation

Build tiered CI:

1. Per commit: clean submodule build, native tests, CLI negative tests, formatting/warnings, Python harness units, and deterministic compile-only corpus checks.
2. Scheduled/merge gate: localnet semantic and differential suites in a pinned container/network image.
3. Release: sanitizer builds, fuzz targets, artifact reproducibility, dependency scan/SBOM, and signed results tied to the exact root and submodule commits.

Generate one authoritative status file from CI. Make unexpected failures and xpasses fail the gate unless explicitly reviewed.

### M-02 — Removed splitter functionality remains in docs and the live CLI

**Severity:** Medium  
**Category:** Interface correctness

#### Evidence

- `README.md:19` says the compiler has a contract splitter and line 115 instructs users to pass `--split-contracts --allow-mid-function-split`; current CLI rejects the first option as unknown.
- CLI help still advertises `--split-config`, `--force-delegate`, `--fn-split`, `--pin-to-main`, `--deploy-pure-helpers`, and `--pure-helper-split` in `src/cli/CliOptions.cpp:154-199`.
- `src/main.cpp:275-285` says the splitter was removed to another branch and rejects only some of those parsed fields.
- `pureHelperSplits` and `pinnedToMain` are parsed but never included in the rejection condition or otherwise consumed on `main`.
- The audit reproduced `--pure-helper-split value:1` returning exit 0 and generating ordinary, unsplit AWST.

#### Impact

Scripts can believe required size or placement transformations occurred when they did not. This can waste deployment effort, conceal configuration mistakes, and make reported command lines impossible to reproduce.

#### Recommendation

On `main`, remove branch-only options from help and parsing or reject every one with the same explicit message before compiling. Update the README and repository-layout claims. Keep branch-specific documentation with the experimental branch rather than describing it as current behavior.

### M-03 — Options and child deployment artifacts are not written or validated transactionally

**Severity:** Medium  
**Category:** Artifact integrity

#### Evidence

- `src/json/OptionsWriter.cpp:69-76` and `111-118` check only whether `options.json` opened. They do not close and verify the stream after writing.
- `src/main.cpp:327-357` logs “Wrote” regardless and may invoke the backend with that path. In contrast, `awst.json` is explicitly closed and checked at lines 297-310.
- `src/cli/AwstPostPasses.cpp:114-153` silently omits expected child `.bin` files when absent, does not verify reads or the final JSON write, and logs success unconditionally.
- The approval program is split at 4,096 bytes, but page 1 receives the entire remainder without an upper-bound assertion even though the comment promises two pages of at most 4,096 bytes each.
- Writes go directly to reusable final paths, so failure can leave stale or truncated files.

#### Impact

Disk-full, permission, interrupted-write, stale-output, or unexpected backend behavior can produce an artifact set that appears successful but is incomplete or inconsistent. For child deployments, that can result in missing template variables or programs that do not match what the parent was compiled to deploy.

#### Recommendation

Write every artifact to a fresh temporary file in the destination directory, close and verify it, validate JSON/schema and expected size, then atomically rename it. Clear or isolate each build output directory. Require every expected child binary, enforce both page limits, hash all inputs/outputs, and fail the command before logging success if any artifact is absent or invalid.

### M-04 — Repository history is dominated by generated outputs and result snapshots

**Severity:** Medium  
**Category:** Repository hygiene / auditability

#### Evidence

- The root tracks 52,434 paths.
- 47,698 tracked paths are under directories named like `out*`; 244 more are semantic `RESULTS*.txt`/`results_v*.txt` snapshots.
- Extension counts include 14,913 JSON, 13,996 TEAL, 13,490 binary, 3,639 log, 1,024 MIR, and 789 IR files.
- Git reports 581,139 packed objects occupying about 1.05 GiB; the on-disk `.git` directory measured about 2.4 GiB in this workspace.
- There are many competing historical baselines, while current `CURRENT.md` and README are stale.

#### Impact

Clone/fetch/storage costs are high, reviews are noisy, generated diffs can hide source changes, and it is difficult to identify the authoritative evidence for a revision. Large history also raises the cost of clean CI—the exact control H-01 and M-01 require.

#### Recommendation

Keep a deliberately small set of reviewed golden artifacts in Git. Upload full corpus outputs, logs, and historical reports as CI artifacts or release assets keyed by commit. Add generated-output rules to `.gitignore`. If repository owners choose to rewrite history, plan and communicate that separately because it disrupts all clones and forks.

### M-05 — The build and test toolchains are machine-specific and incompletely declared

**Severity:** Medium  
**Category:** Portability / dependency management

#### Evidence

- `CMakeLists.txt:28-31` forces Boost to `$HOME/.local/boost-1.83`, disables system search, and provides no portable fallback.
- The build links absolute archive paths rather than dependency targets and does not own the Solidity configure/build step.
- The README does not list required CMake/compiler/Boost/Z3 versions or commands for building the Solidity archives.
- The existing build cache has `CMAKE_CXX_FLAGS=-w`; the repository itself enables only `-Werror=return-local-addr` for the main target and no consistent warning baseline for all targets.
- The root Python test harness has no root `pyproject.toml`, lock file, or requirements file. Its behavior therefore depends on globally installed plugins and packages; in this audit, a global pytest plugin had to be disabled to run focused units in the sandbox.
- No dependency-update policy, root SBOM, or automated vulnerability scan configuration is present.

#### Impact

Different machines can build or test materially different products, onboarding is brittle, warnings are inconsistently visible, and dependency provenance/security status is difficult to assess.

#### Recommendation

Provide a pinned dev container or Nix/uv-equivalent environment, declare every root build/test dependency, use CMake imported targets, and give one bootstrap command. Enable a reviewed warning set on all first-party targets, plus separate ASan/UBSan builds. Generate an SBOM and scan the exact root/submodule/lock revisions in CI.

### M-06 — Root licensing and security-governance metadata are absent

**Severity:** Medium  
**Category:** Legal / vulnerability handling

#### Evidence

The root project has no `LICENSE`, `SECURITY.md`, `CONTRIBUTING.md`, or code-owner/release policy. The submodules contain their own licenses and security documents, but those do not license or govern the first-party `puya-sol` code. The README invites contributions and describes third-party integration without specifying redistribution terms or a private vulnerability-reporting channel.

#### Impact

Users and contributors cannot reliably determine permission to use, modify, or distribute first-party code. Security researchers lack a documented reporting path, supported-version policy, disclosure expectation, or response contact.

#### Recommendation

Choose and add an explicit license with SPDX headers/notices appropriate to the linked dependencies. Add `SECURITY.md` with a private contact and supported-version statement, plus contribution, ownership, release, and provenance policies. Preserve required notices for Solidity, Puya, Boost, and other dependencies.

### L-01 — Some operational failures are downgraded or hidden

**Severity:** Low  
**Category:** Diagnostics / fail-fast behavior

#### Evidence

- `Logger::setOutputLogFile` in `src/Logger.cpp:80-83` does not report an open failure.
- `readSourceFile` in `src/cli/CompilerSetup.cpp:79-85` checks opening but not a read error after streaming.
- Invalid import remappings only warn and compilation continues at `src/cli/CompilerSetup.cpp:118-136`.
- Several filesystem operations in `main` have no surrounding exception boundary.

#### Impact

Compilation can continue without an expected log or with confusing secondary errors, and filesystem faults can produce inconsistent exit behavior. Most malformed imports will later fail type-checking, limiting the direct impact.

#### Recommendation

Return status from logger/source setup, verify reads and writes, make explicitly supplied invalid remappings fatal, and add one top-level exception handler that reports context and a stable exit code without masking programmer invariants.

## Known semantic hazards that remain release blockers

The following are primarily documented design/platform limitations rather than newly discovered implementation defects. They still matter to any risk decision:

- The EVM address projection is lossy. Without a pinned xchain account model, native transfers to projected 160-bit identities can be unrecoverable (`EVM_DIVERGENCE.md:24-40`). The shared high-level payment builder fails closed, but `selfdestruct` and inline-assembly value-bearing `call` currently bypass it.
- The xchain mapping deliberately collides with the `bzero24 ++ appId` convention for 20-byte identities whose leading 12 bytes are zero (`EVM_DIVERGENCE.md:42-60`). Random-key probability is small, but special or deliberately chosen low addresses are not random.
- AVM forbids the EVM reentrant/self-call execution model; `this.f()`, A→B→A, low-level failure, receive/fallback, and return-data behavior can differ.
- `staticcall` cannot enforce read-only state.
- Some features fail at compile time—which is safer—while others fail only at runtime or compile with adaptations. The manifest records enabled divergence names, but does not pin a canonical xchain template/profile or prove that every payment lowering passed through the policy boundary.
- The latest recorded semantic suite is not 100% passing and contains substantial expected-failure policy. Passing the supported subset is not evidence of equivalence for arbitrary Solidity.

## Positive controls observed

- `README.md` clearly and prominently says this is an unaudited proof of concept and not production-money-safe.
- `main.cpp:159-168` refuses to lower an AST unless Solidity parsing and semantic analysis complete successfully.
- Several impossible-to-model EVM features—such as `tx.origin`, arbitrary code introspection, creation/runtime EVM bytecode, and unresolved low-level calls—are hard compile errors instead of fabricated values.
- EVM entry routing checks selector/argument shapes, and the reviewed xchain owner-claim path verifies both the 20-byte claim length and `sha512_256("Program" || template-with-owner)` against `Txn.Sender` before adopting the identity.
- `awst.json` is explicitly checked after writing, and child templates are generated only after a successful backend exit.
- The Puya backend is launched with `fork`/`execlp` and discrete arguments (`src/runner/PuyaRunner.cpp:30-50`), avoiding shell injection through paths.
- Numeric CLI parsing outside the separate xchain lambda validates full decimal strings and catches range errors.
- Recent commit history and `CURRENT.md` show active use of differential tests to find real miscompilations; many comments document why a behavior is exact, adapted, or rejected.

## Prioritized remediation plan

### Before the next shareable experimental build

1. Fix H-01 and prove a clean-clone build from committed inputs.
2. Make pragma rewriting opt-in and make unimplemented EVM layout modes hard errors.
3. Fix scratch-slot validation/tests and strict xchain hex parsing.
4. Reject every removed/no-op splitter option on `main` and correct the README/help.
5. Establish green per-commit CI and publish one HEAD-specific status result.

### Before any testnet security claim

1. Add strict divergence enforcement and machine-readable per-contract fidelity manifests.
2. Make artifacts isolated, atomic, hashed, schema-validated, and size-checked.
3. Run pinned ASan/UBSan, fuzzing, full semantic, differential, proxy/auth, ABI, storage, memory-seam, and adversarial CLI suites in CI.
4. Produce a reproducible toolchain manifest and SBOM; perform dependency/vulnerability review.
5. Add license, security policy, release signing, supported-version policy, and an incident process.

### Before production or real funds

Commission independent compiler and generated-code audits, define and formally specify the supported Solidity subset, verify that every non-exact semantic requires explicit acceptance, and obtain sustained differential/fuzz coverage with zero unexplained failures. Current architecture and test results are not sufficient for this milestone.

## Final assessment

The project has unusually candid documentation and several thoughtful fail-closed decisions, but the trustworthy computing base is presently the developer's dirty workspace rather than the repository. Silent source rewriting, accepted no-op fidelity flags, suppressible semantic divergences, and a stale/red validation baseline are incompatible with compiler assurance. The immediate goal should be a small, reproducible, fail-closed experimental product—not expanding the supported surface—followed by automated semantic evidence tied to every revision.
