// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// Found by the differential fuzzer: compound `x /= b` skipped the intN.min / -1
// signed-division overflow check that EVM reverts on (the plain `a / b` path has it).
contract C {
    function compMix(int128 a, int16 b) external pure returns (int128) { int128 x = a; x /= int128(b); return x; }
    function compSame(int128 a, int128 b) external pure returns (int128) { int128 x = a; x /= b; return x; }
    function comp256(int256 a, int256 b) external pure returns (int256) { int256 x = a; x /= b; return x; }
    function compMod(int128 a, int128 b) external pure returns (int128) { int128 x = a; x %= b; return x; }
}
