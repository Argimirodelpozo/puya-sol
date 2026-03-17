// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

error InsufficientBalance(uint256 available, uint256 required);

contract Errors {
    function requirePass(uint256 x) external pure returns (uint256) {
        require(x > 0, "must be positive");
        return x;
    }

    function requireFail() external pure {
        require(false, "always fails");
    }

    function assertPass() external pure returns (bool) {
        assert(1 + 1 == 2);
        return true;
    }

    function revertAlways() external pure {
        revert("explicit revert");
    }

    function revertCustomError(uint256 balance, uint256 needed) external pure {
        if (balance < needed) {
            revert InsufficientBalance(balance, needed);
        }
    }
}
