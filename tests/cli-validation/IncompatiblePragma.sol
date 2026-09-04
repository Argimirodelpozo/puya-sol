// SPDX-License-Identifier: MIT
pragma solidity <0.8.0;

contract IncompatiblePragma {
    function value() external pure returns (uint256) {
        return 1;
    }
}
