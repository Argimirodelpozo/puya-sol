// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// Guard: pushing to a dynamic array that is a FIELD of a struct stored in a
// mapping entry (`m[k].arr.push(v)`). The lazy per-entry struct box must be
// materialised with a valid default struct encoding before ArrayExtend's
// box_extract, else the AVM reverts "no such box". See
// SolArrayMethod chained-storage path + StorageMapper::makeEnsureRootBoxForWrite.
contract G {
    struct S { uint64[] arr; uint64 x; }
    mapping(uint256 => S) m;

    function pushM(uint256 k, uint64 v) external { m[k].arr.push(v); }
    function lenM(uint256 k) external view returns (uint256) { return m[k].arr.length; }
    function getM(uint256 k, uint256 i) external view returns (uint64) { return m[k].arr[i]; }
}
