// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// Guard: `(a, b) = (b, a)` on VALUE-TYPE state vars is a real swap.
//
// Solidity copies value types into the RHS tuple, so the swap works. Only an
// AGGREGATE storage var is a reference, and only that keeps the sequential
// overwrite documented by various/swap_in_storage_overwrite (which swaps two
// STRUCTS, and is left alone here).
//
// The RHS-snapshot gate used to key on the LHS component's EXPRESSION SHAPE —
// array/mapping index or member access — so plain identifiers fell through and
// every value-type swap collapsed to (b, b). Found by behavioural audit, never
// by the suite.
contract C {
    uint256 a;
    uint256 b;
    address addrA;
    address addrB;
    bool flagA;
    bool flagB;
    uint8 packedA;
    uint8 packedB;

    struct S { uint256 p; uint256 q; }
    S sx;
    S sy;

    function init() public {
        a = 1; b = 2;
        addrA = address(0x1111); addrB = address(0x2222);
        flagA = true; flagB = false;
        packedA = 7; packedB = 9;
        sx.p = 1; sx.q = 2;
        sy.p = 3; sy.q = 4;
    }

    function swapUint() public { (a, b) = (b, a); }
    function swapAddress() public { (addrA, addrB) = (addrB, addrA); }
    function swapBool() public { (flagA, flagB) = (flagB, flagA); }
    function swapSubword() public { (packedA, packedB) = (packedB, packedA); }

    // Three-way rotation: every target must read the PRE-assignment value.
    function rotate() public { (a, b, packedA) = (b, uint256(packedA), uint8(a)); }

    // Aggregates keep the EVM sequential-overwrite quirk: `(sx, sy) = (sy, sx)`
    // runs as `sy = sx; sx = sy`, so BOTH end up holding sx's old value. Matches
    // solc's swap_in_storage_overwrite exactly.
    function swapStruct() public { (sx, sy) = (sy, sx); }

    function getUint() public view returns (uint256, uint256) { return (a, b); }
    function getAddress() public view returns (address, address) { return (addrA, addrB); }
    function getBool() public view returns (bool, bool) { return (flagA, flagB); }
    function getSubword() public view returns (uint8, uint8) { return (packedA, packedB); }
    function getStructs() public view returns (uint256, uint256, uint256, uint256) {
        return (sx.p, sx.q, sy.p, sy.q);
    }
}
