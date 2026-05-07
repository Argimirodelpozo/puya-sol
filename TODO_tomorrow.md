# TODO tomorrow

## Solidity `memory` reference-type params: by-reference semantics broken in puya-sol

When Solidity declares a function param as a `memory` reference type
(e.g. `Fr[40] memory evals`, `MyStruct memory s`), Solidity passes
that param **by reference**: mutations inside the function are visible
to the caller.

puya-sol currently translates these as algopy value types
(`arc4.static_array<...>`, `arc4.struct`), which are pass-by-value at
the algopy level. Mutations inside the function happen on a local
copy; the caller never sees them.

**Concrete failure**: rust-honk's `RelationsLib.accumulateAuxillaryRelation`,
`accumulateArithmeticRelation`, etc. are declared `internal pure` in
Solidity and mutate their `evals` array param to output
sub-relation evaluations. Solidity's `pure` allows this (no state
read/write). After puya-sol translation, the mutations don't
propagate, and puya's DCE correctly eliminates the call as dead code
in any caller that doesn't read the (lost) output. Result: lifted
helpers compile to ~23 B no-ops; main contract silently misses
relation-accumulation logic.

## Proposed fix: extend the blob memory model

puya-sol already simulates EVM linear memory via scratch slots 0-4
(see `blob-memory-model.md` memory file). That blob model is wired
only into the assembly-builder path. Solidity-level `memory` variables
bypass it and go through algopy types.

The fix: extend the blob model to Solidity-level memory references.

  - Type translation: `Fr[40] memory x` → `uint64` (offset into the
    blob), not `arc4.static_array<uint256, 40>`.
  - Body translation: `x[i] = v` → `replace3(blob, x_offset + i*32,
    pad32(v))`. `x[i]` reads → `extract3(blob, x_offset + i*32, 32)`
    cast to the right type.
  - Memory struct field accesses get the same treatment via fixed
    offsets within the struct's blob region.
  - Call sites: caller allocates a free blob region, materializes the
    array data, passes the offset, reads back from the offset after
    return.

This matches Solidity's actual memory model (memory pointers) and
preserves by-reference semantics naturally.

## Why this matters beyond honk

This bug isn't honk-specific. Any Solidity contract whose internal
helpers output values via `memory` param mutation is silently broken
in puya-sol today. The pattern is common in zk verifiers, math
libraries, and protocols built around accumulator structs.

## Risk

The translation rewrite affects every Solidity contract using memory
reference types. All existing tests would re-translate and could
break in subtle ways. Recommended approach: gate behind a flag
(`--memory-via-blob`) so existing contracts keep current behavior;
new ones (honk) opt in and we measure regression surface.

## Owner / status

- Discovered: 2026-05-07 while testing if rust-honk Verifier compiles
  with current splitter strategies (it compiles bytecode but is
  runtime-broken because of this gap).
- Status: documented; not implemented.
