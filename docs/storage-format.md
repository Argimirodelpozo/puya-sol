# Default storage holder format 2

This is a **fresh-deployment-only** change on `rev-2`. Existing applications
must retain their original compiler and artifacts. There is no legacy-key
fallback, automatic reinterpretation or state migration. `--evm-storage-layout`
keeps its existing slot-based keys and does not use this format.

## Roots and descendants

Default-layout persistent roots containing mappings use the following printable
box key/holder identity. Ordinary mapping-free named cells retain their keys.
All integers below are unsigned, big-endian and fixed-width.

```text
coordinate(slot, offset) = slot[32 bytes] || offset[1 byte]
root = ASCII("@puya-sol/2:") || RFC1924-base85(coordinate)
child = SHA256(ASCII("puya-sol/2/") || tag[1 byte]
               || byte_length(parent)[8 bytes] || parent || payload)
```

A root is 54 bytes; every descendant is 32 bytes. The reserved root prefix
cannot be a Solidity identifier, and both fit AVM's 64-byte box-name limit.
Source names remain readable artifact labels, but neither names nor
compilation-local AST IDs enter the encoded identity. Full solc logical slots
are retained, including custom storage bases above 64 bits.

| Step | Tag | Payload | Authoritative input |
|---|---|---|---|
| Struct member | ASCII `s` | `coordinate(relative slot, byte offset)` | Solc `storageOffsetsOfMember()` |
| Array element leading to a mapping | ASCII `a` | Index, 32 bytes | Checked index and solc array shape/bounds |
| Mapping entry | ASCII `m` | Declared-key encoding below | Solc mapping key type |

Parent length framing and separate tags distinguish every segment boundary.
Member offsets are relative to their enclosing struct; they are not ARC4 byte
offsets. A nested array's holder is an identity for descendant mapping boxes,
not a claim that the array itself lives in a separate box. Its data/length
remain at the appropriate projection of its enclosing serialized value.

A nonrecursive mapping-containing struct whose sole member is another struct
at solc coordinate `(0, 0)`, with the same storage extent, is a transparent
wrapper. It adds neither a holder step nor an ARC4 wrapper header. Its nominal
type identity remains distinct, while its encoded fields and holder are the
inner struct's. This also changes the persisted representation of such wrappers
and is covered by the same fresh-deployment-only boundary. Tooling must omit
these transparent steps when constructing paths from solc types/layout.

Key-only aggregate reference parameters/returns must address a whole box,
including a mapping-entry box. In practice this rejects, in the default layout
only, OpenZeppelin idioms that pass an interior aggregate by reference:
`Checkpoints.push(Trace storage)` (ERC20Votes/Votes on OZ 5.x: `Trace208`
lives inside the contract's storage and `_insert(self._checkpoints, …)` is an
interior dynamic-array reference), `EnumerableMap.set` (`map._keys.add(key)`),
and `f(pools[i])` where the element struct contains a mapping. Compile those
contracts with `--evm-storage-layout`; the old default lowering silently wrote
phantom boxes for these shapes. Passing or returning an interior struct/array
containing mappings would lose either its data location or its holder identity;
the compiler diagnoses that unsupported handle shape. Direct nested updates,
local aliases, references to the mapping field itself, and references to the
whole enclosing aggregate remain available. Transparent wrappers do not create
an interior data slice and therefore support key-only references to their sole
member. EVM slot mode retains its existing representation and interior
references through canonical logical slots.

Declared mapping-key encoding retains the native rules: uint64-carried integer
and bool values use eight bytes; biguint-carried integers use 32 bytes; accounts
use their profile-selected address representation; fixed bytes use their declared
byte width. Dynamic string/bytes keys first become a SHA-256 digest. Values must
be converted to the declared key type before encoding. This is not EVM's
`keccak256(key || slot)` storage format.

Enums now retain a full numeric word until a solc validation boundary, including
words dirtied by inline assembly. Standalone named enum cells therefore use
byte-valued storage instead of uint64 cells, and enum mapping keys use 32-byte
payloads instead of eight. This is another fresh-deployment-only representation
change; existing enum cells and mapping entries are not migrated automatically.
Enum ABI widths, enum fields inside ARC4 aggregates, and EVM-slot field widths
remain unchanged.

## Artifacts and tooling

ARC-56 `state.keys.box` records each struct/array root key and its stored
representation, with a format-2 description; such roots may also contain
ordinary encoded data. A pure mapping root is published as a `state.maps.box`
entry so clients see its key and value types (a nested mapping's `keyType` is
the tuple of its key types; the value struct appears in `structs`): its
`prefix` is the same coordinate root key, and its description states that entry
names are tagged SHA-256 derivations of the key under that root — **not**
`prefix ++ encode(key)`. Generic ARC-56 clients must derive entry names with
the rules above (or `framework/storage_keys.py`) rather than by concatenation.

The test/tooling helper
[`framework/storage_keys.py`](../tests/solidity-semantic-tests/framework/storage_keys.py)
implements root/member/array/mapping derivation from supplied layout facts and
already-encoded mapping keys. Runtime tests read boxes by those derived keys,
in addition to checking Solidity reads, aliases, reference calls and getters.

The other approved placement change on `rev-2` moves default-layout dynamic
aggregates containing internal function pointers from globals to boxes.
Mapping-free roots retain their source-name keys, and small fixed callback
aggregates stay global. This placement fix is also fresh-deployment-only.

## Named-cell placement policy

Named-cell storage uses actual AVM encoded sizes for placement by default;
there is no separate placement flag. Solc storage spans describe EVM words,
not ARC4 encoded byte sizes. For example, `uint8[64]` needs 64 value bytes,
and `bool[64]` needs 8, rather than the legacy conservative EVM-slot upper
bound. A mapping-free
fixed value can use global state when its actual encoding and key fit the
128-byte limit. Mapping holders, dynamic aggregates and structs whose reference
transport requires a box key retain their box representation (including root
values of struct types also used as mapping values). Strings retain their
existing global-state policy and capacity limitations.

This policy changes physical box/global placement relative to the earlier
conservative policy, not solc's logical slots, mapping-holder derivations, or
value encoding. ARC-56 publishes the chosen physical cells. Existing named-
layout applications may need explicit state migration before recompilation
and update: there is no automatic migration or fallback read from old boxes.
EVM-slot storage (`--evm-storage-layout`) retains its existing behavior.

Immutables are named cells even in EVM-slot mode. Same-named inherited
immutable declarations receive distinct physical keys; noncolliding keys are
unchanged. Their cells must be included in ARC-56/schema allocation. Previously
colliding immutable deployments require recreation or explicit migration;
the compiler cannot recover values already overwritten in the shared cell.

## Copying and addressing limits

Fixed-array storage copies use solc's declared element types and layout.
Equivalent scalar arrays can copy words, clearing unused bytes and the partial
last word. Aggregate/converting copies use the typed readers and writers, so
nested dynamic contents are copied and a shorter source clears the destination
tail. Self-copy is a no-op; struct copies retain unrelated padding bits.
Named-cell copies rebuild ARC4 offsets when the fixed array contains dynamic
elements. Native addresses retain their full AVM representation.

Logical lengths and EVM element strides remain full-width solc facts. Sparse
element access does not require materializing the entire declared array.
Whole-value operations still have target limits: the fixed scalar-copy fast
path unrolls at most 256 slots, converting/aggregate slot copies at most 64
outer elements, and materialized byte values must fit AVM's 4 KiB stack limit.
These are implementation capacities, not Solidity layout limits; this is not
an unbounded streaming-copy implementation.

The separate slot-to-value reader and value-to-slot writer no longer impose a
64-element fixed-array limit: they check actual encoded size and loop over
larger bounded arrays. Fixed-array and struct deletion likewise use counted
loops over the full solc storage extent, subject to AVM runtime resources.
Dynamic materialization validates the complete storage length before narrowing
it and checks the encoded head against the 4 KiB value limit.
