// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// NOT an o.g. semantic test. Guards box lifecycle for a struct-embedded fixed-size array.
// Writing `st.inner[i] = v` is a PARTIAL write (box_replace at an offset) into the struct's box.
// A plain state-var box is only created by a FULL write (st = S(...), or st.x = v which COW-rebuilds
// and box_puts). So a FIRST partial write (setStInner before any full write) used to box_replace a
// non-existent box -> "no such box" revert (EVM auto-zero-inits storage). Fixed by an idempotent
// box_put(default) prologue in maybePrePopulateBox for state-var boxes reached by a partial write.
contract G {
    struct S { int128[2] inner; uint64 x; }
    S st;
    function setStInner(uint256 i, int128 v) external { if (i < 2) st.inner[i] = v; }
    function getStInner(uint256 i) external view returns (int128) { return i < 2 ? st.inner[i] : int128(0); }
    function setStX(uint64 v) external { st.x = v; }
    function getStX() external view returns (uint64) { return st.x; }
}
