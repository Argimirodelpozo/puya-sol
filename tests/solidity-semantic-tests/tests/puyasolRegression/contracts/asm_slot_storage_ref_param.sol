// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// CUSTOM regression fixture (NOT vendored). Guards asm `.slot` on a struct
// storage-ref PARAMETER — the solady storage-library idiom:
//   library L { function op(S storage s) { assembly { sload(s.slot) } } }
// Such a param travels as a box-key handle (bytes) so `s.slot` resolves to a
// BoxValueExpression over that key; sload/sstore read/write the box's slot-0
// word. Was a hard error ("unmodeled .slot reference (type 'S')"). Single
// uint256-field struct (solady Uint8Set shape). See asm-slot-storage-ref-param.
library BitsetLib {
    struct Bitset { uint256 data; }

    function setBit(Bitset storage b, uint256 i) internal {
        assembly { sstore(b.slot, or(sload(b.slot), shl(i, 1))) }
    }

    function has(Bitset storage b, uint256 i) internal view returns (bool r) {
        assembly { r := and(1, shr(i, sload(b.slot))) }
    }

    function raw(Bitset storage b) internal view returns (uint256 r) {
        assembly { r := sload(b.slot) }
    }
}

contract AsmSlotStorageRefParam {
    using BitsetLib for BitsetLib.Bitset;
    BitsetLib.Bitset private bits;

    function set(uint256 i) external { bits.setBit(i); }
    function get(uint256 i) external view returns (bool) { return bits.has(i); }
    function rawWord() external view returns (uint256) { return bits.raw(); }
}
