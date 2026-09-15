# Scratch memory model prototype — `--memory-model scratch`

Branch `experiment/scratch-memory-model`, based on `builder-layered-layout`
(`f824b2e243`). Experimental and off by default; the mixed model is unchanged
when the flag is absent.

## What the prototype does

Every memory array and struct is a uint64 byte offset into the scratch pages,
laid out the way solc lays out memory (free-memory pointer at 0x40, allocation
from 0x80 in word-rounded sizes, 32-byte words per scalar, pointer words for
reference-typed members and elements). A pointer is translated into a scratch
slot and an offset inside it: `slot = offset / 4096`, `sub = offset % 4096`, then
`extract3`/`replace3` on that slot value. There is no straddle detection: every
high-level access is 32-byte aligned by construction, so a word never crosses a
page. `bytes`/`string` keep their native carriers.

The existing blob pointer path (previously reserved for aggregates over 4 KiB
and assembly-visible objects) is generalized rather than replaced:

- `memoryUsesBlob(profile, wtype)` is true for every ARC4 array/struct/tuple
  carrier in scratch mode.
- Declarations spill fresh values (`new`, literals, storage reads, ABI copies)
  into memory; references (other memory variables, internal-call results,
  conditionals of references) bind the pointer.
- Internal functions take and return memory aggregates as offsets
  (`RefParamPassing::BlobOffset`, `FunctionReturnPlan::internalType`), including
  unnamed returns; `return <reference>` returns the pointer, `return <value>`
  spills first. Functions on the external interface keep the value protocol and
  copy at the ABI boundary.
- Public entry spills ABI arguments into memory; public returns materialize.
- Reference-typed member/element assignment (`h.items = x`, `a[i] = x` for
  arrays of arrays, `s.name = "x"`) writes a pointer word: aliasing for an
  existing reference, a fresh spill otherwise.
- `.length` of a memory array reads the length word.
- Word reads/writes pass the alignment fact, so the inline `(slot, sub)` path
  is used instead of the shared straddle helpers.

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

- Recursive struct types in memory (above).
- Functions with modifiers and memory-aggregate parameters: each chain member
  re-decodes the wire argument, so members would spill separate copies.
- `delete` of a reference-typed member/element, tuple returns of memory
  aggregates (value protocol, identity lost across the call), memory `bytes`
  and `string` variables (still native values, so `b = a` copies).
- The EVM calldata entry router (`--contract-abi evm`) has no spill step.
- `bytes` payload copies still use the shared straddle write helper.
- Arena capacity is the existing `--evm-memory-slots` bound; solc layout
  needs more of it than ARC4 (a `uint8[1000]` is 32,000 bytes).

## Results

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

`puyasolRegression/contracts/scratch_memory_model.sol` holds 13 identity
probes: alias then rebind, `f(a, a)`, mutation through an internal call,
struct alias through a call, returned reference, storage round trips are
copies, shared struct child, ABI boundary copies, fill loop, fixed array of
narrow ints aliasing, named memory return, reference-slot reassignment,
string member reassignment. Scratch model: all pass. Mixed model: the contract
does not compile (puya rejects `f(a, a)`, "mutable values cannot be passed more
than once to a subroutine"); recorded as xfail.

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
