/// SPDX-License-Identifier: MIT
pragma solidity >=0.7.0;

import "./PoseidonT6.sol";

contract PoseidonT6Test {
    function hashFive(uint256 a, uint256 b, uint256 c, uint256 d, uint256 e) external pure returns (uint256) {
        return PoseidonT6.hash([a, b, c, d, e]);
    }
}
