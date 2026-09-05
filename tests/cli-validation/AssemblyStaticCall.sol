// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract AssemblyStaticCall {
    function probe(address target) external view returns (bool ok) {
        assembly { ok := staticcall(100000, target, 0, 0, 0, 0) }
    }
}
