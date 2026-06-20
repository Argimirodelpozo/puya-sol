// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// puyasolRegression — NOT an o.g. semantic test. Found by the STATEFUL differential fuzzer.
// Signed sub-word compound assignment (int128 x += / -= / *= d) must do real signed arithmetic:
// correct value, signed-overflow revert, sub-word truncation. It previously hit the UNSIGNED
// biguint path (`-1 += 1` false-reverted; real overflow → untruncated garbage).
contract SignedCompound {
    int128 acc;
    function add(int128 a, int128 d) external pure returns (int128) { int128 x = a; x += d; return x; }
    function sub(int128 a, int128 d) external pure returns (int128) { int128 x = a; x -= d; return x; }
    function mul(int128 a, int128 d) external pure returns (int128) { int128 x = a; x *= d; return x; }
    function stateAdd(int128 d)      external returns (int128)      { acc += d; return acc; }
    function uncheckedAdd(int128 a, int128 d) external pure returns (int128) { unchecked { int128 x = a; x += d; return x; } }
}
