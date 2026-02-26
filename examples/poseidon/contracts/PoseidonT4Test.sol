/// SPDX-License-Identifier: MIT
pragma solidity >=0.7.0;

import "./PoseidonT4.sol";

contract PoseidonT4Test {
    function hashThree(uint256 a, uint256 b, uint256 c) external pure returns (uint256) {
        return PoseidonT4.hash([a, b, c]);
    }
}
