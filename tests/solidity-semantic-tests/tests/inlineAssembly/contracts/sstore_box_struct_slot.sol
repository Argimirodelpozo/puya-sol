// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/// Minimal reproduction of Uniswap V4 Pool.updateTick's storage idiom:
/// a struct-valued mapping whose first two fields pack into EVM slot 0, written
/// via inline-assembly `sstore(ref.slot, packedWord)`. Exercises the puya-sol
/// box-keyed-struct sstore lowering (handleBoxKeyedStructSlotStore).
contract SstorePackTest {
    struct Packed {
        uint128 a; // EVM slot 0, low 128 bits  (ARC4 bytes[0:16])
        int128 b;  // EVM slot 0, high 128 bits (ARC4 bytes[16:32]) — signed, like liquidityNet
        uint256 c; // EVM slot 1                 (ARC4 bytes[32:64])
    }

    mapping(uint256 => Packed) data;

    // Solidity allocates each bool a byte in storage, while ARC-4 packs this
    // consecutive run into the high two bits of one byte. `rest` fills the
    // remainder of EVM slot 0, and `untouched` proves the conversion does not
    // overwrite later slots.
    struct BoolPacked {
        bool first;
        bool second;
        uint240 rest;
        uint256 untouched;
    }

    mapping(uint256 => BoolPacked) boolData;

    /// Pack `a` (low) and `b` (high) into slot 0 with one sstore, leaving `c`
    /// untouched — the exact shape of Pool.updateTick's final write.
    function setPacked(uint256 key, uint128 a, int128 b) public {
        Packed storage p = data[key];
        assembly ("memory-safe") {
            sstore(
                p.slot,
                or(and(a, 0xffffffffffffffffffffffffffffffff), shl(128, b))
            )
        }
    }

    function setC(uint256 key, uint256 c) public {
        data[key].c = c;
    }

    function getA(uint256 key) public view returns (uint128) {
        return data[key].a;
    }

    function getB(uint256 key) public view returns (int128) {
        return data[key].b;
    }

    function getC(uint256 key) public view returns (uint256) {
        return data[key].c;
    }

    function setBoolPacked(
        uint256 key,
        bool first,
        bool second,
        uint240 rest
    ) public {
        BoolPacked storage p = boolData[key];
        assembly ("memory-safe") {
            sstore(p.slot, or(or(first, shl(8, second)), shl(16, rest)))
        }
    }

    function setBoolUntouched(uint256 key, uint256 value) public {
        boolData[key].untouched = value;
    }

    function getBoolPacked(uint256 key)
        public
        view
        returns (bool first, bool second, uint240 rest, uint256 untouched)
    {
        BoolPacked storage p = boolData[key];
        return (p.first, p.second, p.rest, p.untouched);
    }

    function loadBoolPackedWord(uint256 key) public view returns (uint256 word) {
        BoolPacked storage p = boolData[key];
        assembly ("memory-safe") {
            word := sload(p.slot)
        }
    }
}
