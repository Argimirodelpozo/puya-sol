// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// Differential fixture: bare integer-literal mapping key (records[0x0]) set in
// the constructor, read back via BOTH a bare literal and a bytes32 param — the
// two key-encoding paths must byte-match (else the ctor write and param read hit
// different boxes). Regression for the uint64-literal-key reinterpret bug.
contract MapKeyLit {
    struct R { address owner; uint64 ttl; }
    mapping(bytes32 => R) records;
    constructor() { records[0x0].owner = msg.sender; }
    function ownerLit() external view returns (address) { return records[0x0].owner; }
    function ownerVia(bytes32 node) external view returns (address) { return records[node].owner; }
    function setTtlLit(uint64 t) external { records[0x0].ttl = t; }
    function ttlVia(bytes32 node) external view returns (uint64) { return records[node].ttl; }
    function setOwnerVia(bytes32 node, address o) external { records[node].owner = o; }
}
