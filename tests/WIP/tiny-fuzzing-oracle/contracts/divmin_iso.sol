// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    function plain_mix(int128 a, int16 b) external pure returns (int128) { return a / int128(b); }
    function comp_mix(int128 a, int16 b) external pure returns (int128) { int128 x = a; x /= int128(b); return x; }
    function plain_same(int128 a, int128 b) external pure returns (int128) { return a / b; }
    function comp_same(int128 a, int128 b) external pure returns (int128) { int128 x = a; x /= b; return x; }
    function plain256(int256 a, int256 b) external pure returns (int256) { return a / b; }
    function comp256(int256 a, int256 b) external pure returns (int256) { int256 x = a; x /= b; return x; }
    function mod_comp_mix(int128 a, int16 b) external pure returns (int128) { int128 x = a; x %= int128(b); return x; }
}
