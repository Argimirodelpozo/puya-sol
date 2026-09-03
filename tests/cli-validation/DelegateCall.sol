// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract DelegateCall {
    function probe(address target) external returns (bool) {
        (bool ok,) = target.delegatecall("");
        return ok;
    }
}
