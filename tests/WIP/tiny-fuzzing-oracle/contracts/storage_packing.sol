// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// Differential battery: storage PACKING + the high-level vs assembly storage-model split.
// On the EVM, pa/pb share a slot and `x.slot` + sload sees the same storage as `x`. puya-sol
// stores high-level state name-keyed and assembly sstore/sload via a separate blob — these
// probe whether the two views agree (only the full-width uint256 `.slot` case was unified).
contract StoragePacking {
    uint128 pa;
    uint128 pb;            // pa, pb pack into one EVM slot
    uint64 c1;
    uint64 c2;
    uint64 c3;
    uint64 c4;             // c1..c4 pack into one EVM slot
    uint256 full;

    // packed neighbours stay independent: rewriting pa must not disturb pb. correct: pb=0xBBBB, pa=0x1111
    function packedIndependent() external returns (uint256) {
        pa = 0xAAAA;
        pb = 0xBBBB;
        pa = 0x1111;
        return uint256(pb) * 0x100000000 + uint256(pa);
    }

    // four-way packing: update one of four packed fields, others intact. correct=28.
    function quadPack() external returns (uint256) {
        c1 = 1; c2 = 2; c3 = 3; c4 = 4;
        c2 = 20;
        return uint256(c1) + uint256(c2) + uint256(c3) + uint256(c4);
    }

    // assembly sload of a full-width uint256 slot must see the high-level write. correct=0xDEADBEEF.
    function asmReadsHighLevel() external returns (uint256) {
        full = 0xDEADBEEF;
        uint256 viaAsm;
        assembly { viaAsm := sload(full.slot) }
        return viaAsm;
    }

    // high-level read must see an assembly sstore to the var's slot. correct=12345.
    function highLevelReadsAsm() external returns (uint256) {
        assembly { sstore(full.slot, 12345) }
        return full;
    }

    // round-trip a packed sub-word field through assembly: write hi-level, read whole slot via asm,
    // mask out the field. On the EVM pa occupies the low 128 bits of its slot. correct=0x1234.
    function asmReadsPackedField() external returns (uint256) {
        pa = 0x1234;
        uint256 word;
        assembly { word := sload(pa.slot) }
        return word & 0xffffffffffffffffffffffffffffffff;   // low 128 bits = pa
    }
}
