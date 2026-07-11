// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// Storage layouts: nested mappings, mapping=>struct, array of structs, signed sub-word in storage.
contract StorageLayout {
    mapping(uint256 => mapping(uint256 => int128)) nested;
    struct P { int64 a; uint64 b; }
    mapping(uint256 => P) ps;
    P[] arr;
    function nestedRW(uint256 k1, uint256 k2, int256 v) external returns (int256) {
        nested[k1][k2] = int128(v); return nested[k1][k2];
    }
    function mapStruct(uint256 k, int256 a) external returns (int256) {
        ps[k].a = int64(a); ps[k].b = 7; return int256(ps[k].a) + int256(uint256(ps[k].b));
    }
    function arrStruct(int256 a) external returns (int256) {
        delete arr; arr.push(P(0,0)); arr[0].a = int64(a); arr[0].b = 9;
        return int256(arr[0].a) + int256(uint256(arr[0].b));
    }
}
