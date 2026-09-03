// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract SelfCall {
    function outer() external returns (bool) {
        (bool ok,) = address(this).call(
            abi.encodeWithSignature("inner()")
        );
        return ok;
    }

    function inner() external pure returns (uint256) {
        return 1;
    }
}
