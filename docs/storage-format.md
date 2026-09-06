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
including a mapping-entry box. Passing or returning an interior struct/array
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

## Artifacts and tooling

ARC-56 `state.keys.box` records the exact root key and its stored representation,
with a format-2 description. A mapping root's value is only its existing internal
placeholder; actual entries have derived hash keys. They are **not** described
as ARC-56 `state.maps.box` prefix maps: a holder is not a literal prefix of its
entry hashes. Struct/array roots may also contain ordinary encoded data.

The test/tooling helper
[`framework/storage_keys.py`](../tests/solidity-semantic-tests/framework/storage_keys.py)
implements root/member/array/mapping derivation from supplied layout facts and
already-encoded mapping keys. Runtime tests read boxes by those derived keys,
in addition to checking Solidity reads, aliases, reference calls and getters.

The other approved placement change on `rev-2` moves default-layout dynamic
aggregates containing internal function pointers from globals to boxes.
Mapping-free roots retain their source-name keys, and small fixed callback
aggregates stay global. This placement fix is also fresh-deployment-only.
