// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
// CUSTOM regression fixture (NOT vendored). Guards two tuple-assignment fixes:
//
// 1. arc4.bool tuple target: `(m[0], m[1]) = (m[1], m[0])` over a bool[] left the
//    RHS native bool because handleTupleAssignment's kind-based targetIsArc4
//    switch missed arc4.bool (kind `Basic`) → puya "assignment target type
//    differs". Twin of the scalar bool[] write fix (bool_array_write). Exercised
//    here by memBoolSwap (memory bool[] — the storage bool[] element store is
//    blocked by a separate puya backend bug, see puyabug.md #10).
//
// 2. Array-element parallel swap: `(arr[i], arr[j]) = (arr[j], arr[i])` collapsed
//    to sequential `arr[j]=arr[i]; arr[i]=arr[j]` (both elements took one source
//    value) because the lazy RHS was not snapshotted when the LHS is an array
//    element. A memory element write reassigns the whole backing blob local; a
//    storage element write is read back in-place by the next element's lazy RHS —
//    either way the swap collapses. Fixed by snapshotting the RHS into temps when
//    the LHS writes an array element (storage AND memory). Whole storage
//    state-var/struct tuples keep the EVM sequential-overwrite quirk
//    (swap_in_storage_overwrite) — their LHS is an Identifier, not an index.
contract BoolArrTuple {
    uint256[] su;
    int128[] si;

    // --- storage value-type element swaps (bug 2) ---
    function suSwap(uint256 a, uint256 b) external returns (uint256, uint256) {
        delete su; su.push(a); su.push(b);
        (su[0], su[1]) = (su[1], su[0]);
        return (su[0], su[1]);
    }
    function suRot3(uint256 a, uint256 b, uint256 c) external returns (uint256, uint256, uint256) {
        delete su; su.push(a); su.push(b); su.push(c);
        (su[0], su[1], su[2]) = (su[2], su[0], su[1]);
        return (su[0], su[1], su[2]);
    }
    function siSwap(int128 a, int128 b) external returns (int128, int128) {
        delete si; si.push(a); si.push(b);
        (si[0], si[1]) = (si[1], si[0]);
        return (si[0], si[1]);
    }

    // --- memory element swaps (bug 2), incl. bool[] (also covers bug 1) ---
    function memUSwap(uint256 a, uint256 b) external pure returns (uint256, uint256) {
        uint256[] memory m = new uint256[](2); m[0] = a; m[1] = b;
        (m[0], m[1]) = (m[1], m[0]);
        return (m[0], m[1]);
    }
    function memBoolSwap(bool a, bool b) external pure returns (bool, bool) {
        bool[] memory m = new bool[](2); m[0] = a; m[1] = b;
        (m[0], m[1]) = (m[1], m[0]);
        return (m[0], m[1]);
    }
    function memBoolRot3(bool a, bool b, bool c) external pure returns (bool, bool, bool) {
        bool[] memory m = new bool[](3); m[0] = a; m[1] = b; m[2] = c;
        (m[0], m[1], m[2]) = (m[2], m[0], m[1]);
        return (m[0], m[1], m[2]);
    }
    // mixed element + local
    function mixLocal(uint256 a, uint256 x) external returns (uint256, uint256) {
        delete su; su.push(a);
        (su[0], x) = (x, su[0]);
        return (su[0], x);
    }
}
