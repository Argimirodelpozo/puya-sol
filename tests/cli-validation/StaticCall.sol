// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract StaticCall {
    function probe(address target) external view returns (bool) {
        (bool ok,) = target.staticcall("");
        return ok;
    }
}
