// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
// CUSTOM regression fixture (NOT vendored). Guards a STORAGE-ALIASING bug.
//
// Mapping box keys derive as `sha256(keyBytes ++ prefix)`. A string/bytes key
// encoded to RAW bytes is variable-length, and the prefix (the mapping name) is
// variable-length too, so the preimage had two possible splits:
//
//     a["xb"]   ->  sha256("xb" ++ "a")   == sha256("xba")
//     ba["x"]   ->  sha256("x"  ++ "ba")  == sha256("xba")
//
// Two DIFFERENT mappings therefore shared ONE box: writing ba["x"] silently
// changed a["xb"]. On EVM these are unrelated slots (keccak over distinct slot
// numbers), so it was a silent storage-corruption / fund-loss divergence, and
// the colliding key is chosen by the caller (attacker-controlled).
//
// Fixed in awst::makeKeyBytes by hashing DYNAMIC keys to a fixed 32 bytes, so
// every key encoding is fixed-width (uint64→8B, biguint→32B, string/bytes→32B)
// and the field boundary can no longer be shifted. Same property that makes
// Solidity's `keccak256(h(k) . p)` safe: a fixed-width trailing slot number.
//
// The names here are deliberately suffix-related ("a" is a suffix of "ba"),
// which is what made the two splits collide.
contract MappingKeyCollision {
    mapping(string => uint256) public a;
    mapping(string => uint256) public ba;
    mapping(bytes => uint256) public c;
    mapping(bytes => uint256) public bc;

    function setA(string calldata k, uint256 v) external { a[k] = v; }
    function setBa(string calldata k, uint256 v) external { ba[k] = v; }
    function getA(string calldata k) external view returns (uint256) { return a[k]; }
    function getBa(string calldata k) external view returns (uint256) { return ba[k]; }

    function setC(bytes calldata k, uint256 v) external { c[k] = v; }
    function setBc(bytes calldata k, uint256 v) external { bc[k] = v; }
    function getC(bytes calldata k) external view returns (uint256) { return c[k]; }
    function getBc(bytes calldata k) external view returns (uint256) { return bc[k]; }
}
