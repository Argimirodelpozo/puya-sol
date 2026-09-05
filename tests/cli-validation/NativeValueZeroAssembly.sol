// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract NativeValueZeroAssembly {
    function probe(address target) external returns (uint256 ok) {
        assembly { ok := call(gas(), target, 0, 0, 0, 0, 0) }
    }
}
