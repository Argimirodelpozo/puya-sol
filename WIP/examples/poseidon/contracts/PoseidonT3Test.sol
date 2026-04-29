/// SPDX-License-Identifier: MIT
pragma solidity >=0.7.0;

import "./PoseidonT3.sol";

contract PoseidonT3Test {
    function hashTwo(uint256 a, uint256 b) external pure returns (uint256) {
        return PoseidonT3.hash([a, b]);
    }
}
