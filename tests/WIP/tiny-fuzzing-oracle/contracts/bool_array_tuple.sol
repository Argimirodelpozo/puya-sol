// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
// CUSTOM regression fixture (NOT vendored). Guards TWO tuple-assignment bugs:
//
// 1. arc4.bool tuple target: `(sflags[0], sflags[1]) = (b, a)` over a storage
//    bool[] left the RHS native bool because handleTupleAssignment's kind-based
//    targetIsArc4 switch missed arc4.bool (kind `Basic`) → puya "assignment
//    target type differs". Twin of the scalar bool[] write fix (bool_array_write).
//
// 2. Memory-array-element parallel swap: `(m[i], m[j]) = (m[j], m[i])` over a
//    MEMORY array collapsed to sequential `m[j]=m[i]; m[i]=m[j]` (both elements
//    took one source value) — a memory element write reassigns the whole backing
//    blob local, so the lazy RHS did not snapshot. Storage element writes were
//    already correct (in-place box mutation). Fixed by snapshotting the RHS into
//    temps when the LHS writes a non-storage array element.
contract BoolArrTuple {
    bool[] sflags;

    // --- bug 1: storage bool[] tuple swap (arc4.bool target encode) ---
    function setS(bool a, bool b) external { delete sflags; sflags.push(a); sflags.push(b); }
    function swapS() external { (sflags[0], sflags[1]) = (sflags[1], sflags[0]); }
    function pairSetS(bool a, bool b) external { (sflags[0], sflags[1]) = (a, b); }
    function getS(uint256 i) external view returns (bool) { return sflags[i]; }

    // --- bug 2: memory-array parallel swap ---
    function memUSwap(uint256 a, uint256 b) external pure returns (uint256, uint256) {
        uint256[] memory m = new uint256[](2); m[0] = a; m[1] = b;
        (m[0], m[1]) = (m[1], m[0]);
        return (m[0], m[1]);
    }
    function memBSwap(bool a, bool b) external pure returns (bool, bool) {
        bool[] memory m = new bool[](2); m[0] = a; m[1] = b;
        (m[0], m[1]) = (m[1], m[0]);
        return (m[0], m[1]);
    }
    function rot3(uint256 a, uint256 b, uint256 c) external pure returns (uint256, uint256, uint256) {
        uint256[] memory m = new uint256[](3); m[0] = a; m[1] = b; m[2] = c;
        (m[0], m[1], m[2]) = (m[2], m[0], m[1]);
        return (m[0], m[1], m[2]);
    }
}
