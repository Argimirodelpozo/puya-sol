// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

contract StoragePlacementFacts {
    uint8[64] public values;
    bool[64] public bits;
    struct Small { uint8 a; uint8 b; uint8 c; }
    Small public small;
    uint256[8] public large;
    // Identical values straddle the global key+value limit: 128 vs 129 bytes.
    uint248[4] public fits;
    uint248[4] public boxed;
    function update(uint256 i, uint8 value) external returns (uint256) {
        values[i] = value;
        bits[i] = true;
        small = Small(1, 2, 3);
        large[7] = 9;
        return values[i] + small.a + small.b + small.c + large[7];
    }
    function boundary(uint248 value) external returns (uint256) {
        fits[3] = value;
        boxed[3] = value;
        return uint256(fits[3]) + boxed[3];
    }
    function clear() external returns (uint256) {
        delete values;
        delete bits;
        delete small;
        delete fits;
        delete boxed;
        return values[63] + small.c + (bits[63] ? 1 : 0);
    }
}
