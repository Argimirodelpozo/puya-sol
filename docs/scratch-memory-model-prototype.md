# Scratch memory model prototype — `--memory-model scratch`

Branch `experiment/scratch-memory-model`, rebased onto completed `rev-2`
(`18804ec53f`). Experimental and off by default. No experimental memory-model
implementation is included in `rev-2`.

## What the prototype does

Shared memory arrays and structs use uint64 byte offsets into the scratch pages,
laid out the way solc lays out memory (free-memory pointer at 0x40, allocation
from 0x80 in word-rounded sizes, 32-byte words per scalar, pointer words for
reference-typed members and elements). A pointer is translated into a scratch
slot and an offset inside it: `slot = offset / 4096`, `sub = offset % 4096`, then
`extract3`/`replace3` on that slot value. The high-level word path specializes
for aligned pointers from solc-style allocations, without straddle detection.
`bytes`/`string` keep their native carriers.

The existing blob pointer path (previously reserved for aggregates over 4 KiB
and assembly-visible objects) is generalized rather than replaced:

- `TypeMapper::memoryDeclarationUsesBlob` combines the existing size rule with
  declaration-keyed sharing facts in scratch mode. Unique aggregates can retain
  their ARC4 value representation.
- Declarations spill fresh values (`new`, literals, storage reads, ABI copies)
  into memory; references (other memory variables, internal-call results,
  conditionals of references) bind the pointer.
- Internal functions take and return shared memory aggregates as offsets
  (`RefParamPassing::BlobOffset`, `FunctionReturnPlan::internalType`), including
  unnamed returns; `return <reference>` returns the pointer, `return <value>`
  spills first. Functions on the external interface keep the value protocol and
  copy at the ABI boundary.
- ARC-4 public entry spills pointer-backed arguments; public returns materialize.
- Reference-typed member/element assignment (`h.items = x`, `a[i] = x` for
  arrays of arrays, `s.name = "x"`) writes a pointer word: aliasing for an
  existing reference, a fresh spill otherwise.
- `.length` of a memory array reads the length word.
- Word reads/writes pass the alignment fact, so the inline `(slot, sub)` path
  is used instead of the shared straddle helpers.

## Uniqueness: value where identity is provably unobservable

The initial prototype made every aggregate pointer-backed. The current stage
uses a conservative sharing fixed point (`context/MemorySharing.cpp`,
`ProgramAnalysis::memorySharingFacts`) to promote declarations from value to
pointer representation. This analysis runs only with the scratch flag, after
target-specific reachability roots are added. Sharing is required when an
aggregate is

- bound to or assigned from another memory variable or a member/element path
  (`T memory b = a`, `b = a`, `c = s.items`), including through conditionals
  and tuples;
- stored into another object's reference-typed slot, including through a struct
  constructor, or is the root of a slot receiving an existing reference;
- returned when it is a parameter, or returned alongside anything shared,
  including an implicit named return;
- passed to a pointer-backed callee parameter, or through potentially aliased
  arguments where the existing parameter-mutation facts require shared identity;
- bound to the result of a callee whose return may alias a parameter or a
  shared object;
- captured by a modifier argument. Modifier argument expressions and bodies
  are scanned, including invocations without an argument list.

Solc resolves internal calls in each applicable host contract. All resulting
edges participate in one declaration-keyed fixed point: solving hosts separately
and only then unioning their declarations can leave callers using an incompatible
representation. A runtime internal target without exact resolution conservatively
promotes memory-aggregate parameter and return signatures across the invocation.
Read-only, non-escaping unique objects can still use values. A function whose
single memory return may alias returns an offset; one that returns a fresh,
unshared object returns a value the caller takes over.

Raw Yul can observe objects without naming their declarations. Solc's prepared
Yul memory effects, or an aggregate external reference, therefore force memory
aggregates into the arena invocation-wide. Scalar-only assembly does not.
The old free-memory-pointer-only reservation is removed: addresses without
resident object bytes are insufficient for `mload` and `mstore`.

Pointer-backed struct constructors use solc's memory size and member offsets,
and preserve existing child pointers. Legacy solc allocates the head before
evaluating arguments and stores fields as it goes; via-IR evaluates arguments
before allocating the head. Regression expectations for this observable order
come from executing both solc compilation modes, not from assuming they agree.

## What the category sweep found and what was fixed

The first scratch-mode run of the `array`, `structs` and `memoryManagement`
categories (134 cases) had 11 regressions against the mixed model on the same
binary. Triage:

- **Function-pointer members and elements (6 cases).** Internal function
  pointers are uint64 dispatch IDs but the memory codec did not treat them as
  word scalars, so `function(uint)[] memory` and structs with pointer fields
  could not be spilled or read. Fixed: internal pointers are words. External
  function pointers in memory aggregates still need `--evm-selectors` for
  their canonical word, as the existing error says (2 of the 6).
- **Internal calls to public functions with memory parameters (2 cases).**
  The caller passed an offset into the callee's wire signature. Fixed: a
  direct call to an external-interface function with memory parameters now
  targets the private implementation carrier (the mechanism write-backs
  already use), which takes offsets, so the caller's objects are shared as
  solc shares them; only the ABI entry spills its wire arguments.
- **Static array of strings read back wrong (1 case).** A latent bug in the
  existing blob codec: the aggregate allocation and the first child's bytes
  allocation named their offset variable from two different counters and
  collided on `__evmmem_off_0`, so every pointer slot was written 128 bytes
  too high. Only >4 KiB aggregates with string children could have hit this
  before. Fixed by giving bytes allocations their own prefix.
- **Recursive struct types (2 cases, open).** `struct S { S[] x; }`: the
  spill of the default value indexes `arc4.dynamic_array<S__rec>` with result
  type `S`, which puya rejects. Needs the recursive carrier name threaded
  through the codec.

## Known gaps (prototype scope)

- Pointer-backed recursive struct types in memory (the carrier mismatch above;
  unique value-backed cases can pass).
- External function pointers stored as memory words need `--evm-selectors`;
  retaining a unique object's value carrier can avoid that word conversion.
- Functions with modifiers and memory-aggregate parameters: each chain member
  re-decodes the wire argument, so members would spill separate copies.
- `delete` of a reference-typed member/element, tuple returns of memory
  aggregates (value protocol, identity lost across the call), memory `bytes`
  and `string` variables (still native values, so `b = a` copies).
- The EVM calldata entry router (`--contract-abi evm`) has no spill step.
- Raw-memory equivalence is not complete: temporary materialization and
  storage/calldata-to-memory conversion allocation order need further work.
- Arbitrary unaligned aggregate pointers rebound by raw Yul need a separate
  audit of the high-level word path's alignment assumption.
- `bytes` payload copies still use the shared straddle write helper.
- Arena capacity is the existing `--evm-memory-slots` bound; solc layout
  needs more of it than ARC4 (a `uint8[1000]` is 32,000 bytes).

## Current validation after the context-audit rebase (2026-09-15)

The sharing fix passes all 21 new regression checks; the pre-fix rebased binary
failed 16 of them. The combined sharing, original prototype and context/reference
run passed 34 cases with one existing mixed-model xfail. Independent solc/PyEVM
execution confirmed 28 expectations across legacy and via-IR compilation.
Native CTest passed 24/24.

The full default-profile semantic and framework suite completed with **2,539
passed, 1 failed, 101 xfailed and 39 xpassed** in 522.16 seconds. The sole failure
is the existing pinned-Puya DCE/divide-by-zero bug, not an accepted divergence.
All 2,657 cases shared with `rev-2` retained their outcomes; the experiment adds
22 passing cases and its original mixed-model xfail. Both memory-category runs
(default and scratch) passed 128 cases with five xfails and one xpass each.
This is not a claim that every semantic test passes with scratch forced globally.

Frontend-only validation covers all 1,770 Solidity sources in both storage
modes. Default output is byte-identical to `rev-2` for all 3,526 common cases.
The scratch before/after comparison preserves all 3,540 exit codes, with 3,285
successes and 255 frontend failures per binary and no timeouts. Scratch AWST
changes in 88 cases; options hashes do not change. Raw corpus files were removed
after hashing rather than retained as another full output tree.

The [sharing-audit report](../tests/solidity-semantic-tests/out/sharing-audit/REPORT.md)
contains compiler identities, full manifests, JUnit results and the exact outcome
comparison. The prototype limitations above remain; no new xfail markers were
added to hide them.

## Historical results (before the context-audit rebase and sharing fix)

### Category sweep after the fixes (LocalNet, same binary)

`array`, `structs`, `memoryManagement`: 134 cases.

| Mode | Passed | Failed | xfail | xpass |
|---|---:|---:|---:|---:|
| mixed | 128 | 0 | 5 | 1 |
| scratch | 123 | 5 | 5 | 1 |

The five scratch-mode failures are the three external-function-pointer cases
that need `--evm-selectors` for the canonical word and the two recursive
struct cases.

### Default path unchanged

Full suite in the default mixed mode on the prototype binary: 2,547 passed,
1 failed (the known pinned-Puya DCE case), 101 xfailed, 39 xpassed. Byte
identity was checked through the compile cache: for the 1,638 suite sources
compiled by both the pre-change binary and this run, 1,634 produced identical
approval TEAL; the 4 others are the per-run generated `address_metadata`
contracts, whose TEAL differs between any two runs.

### Correctness probes (`test_scratch_memory_model.py`, LocalNet)

The initial `puyasolRegression/contracts/scratch_memory_model.sol` held 13 identity
probes: alias then rebind, `f(a, a)`, mutation through an internal call,
struct alias through a call, returned reference, storage round trips are
copies, shared struct child, ABI boundary copies, fill loop, fixed array of
narrow ints aliasing, named memory return, reference-slot reassignment,
string member reassignment. Scratch model: all pass. Mixed model: the contract
does not compile (puya rejects `f(a, a)`, "mutable values cannot be passed more
than once to a subroutine"); recorded as xfail.

The selective-sharing checkpoint added three probes: a unique object beside a
shared one, a fresh return binding, and aliasing through a returned parameter.
The current fixture executes all sixteen; the new sharing-audit fixtures are
additional coverage, not replacements for those probes.

### Size and op counts

Static counts on the same binary; `main` is the router. The probe is a poor
cost benchmark because its inputs are literals and puya folds most mixed-model
bodies, so a second contract with runtime inputs was measured
(`scratch_memory_bench.sol`).

| Contract | Mixed bytecode | Scratch bytecode | Ratio |
|---|---:|---:|---:|
| probe (without `repeatedArgument`) | 3,382 | 7,085 | 2.09 |
| benchmark | 2,540 | 4,048 | 1.59 |

Benchmark, TEAL ops per function (mixed → scratch):

| Function | Mixed | Scratch | Ratio |
|---|---:|---:|---:|
| `sumParam(uint256[])` | 61 | 105 | 1.7 |
| `fillAndSum(n)` | 151 | 238 | 1.6 |
| `structUpdate(x, y, rounds)` | 130 | 234 | 1.8 |
| `sort(uint256[])` | 143 | 349 | 2.4 |
| `nestedRows(n)` (array of arrays) | 232 | 386 | 1.7 |
| `storageRoundTrip(n)` | 199 | 259 | 1.3 |
| shared memory encode/decode helpers | 0 | 394 | — |

The shared straddle read helper (`__puyasol_memory_read_word`, 37 ops) is gone
from scratch-model output: every high-level read is the inline
`(slot, sub)` sequence. The write helper (48 ops) is still emitted for `bytes`
payload copies, which do not carry the alignment fact yet.
