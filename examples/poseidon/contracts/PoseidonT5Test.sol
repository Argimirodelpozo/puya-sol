/// SPDX-License-Identifier: MIT
pragma solidity >=0.7.0;

import "./PoseidonT5.sol";

contract PoseidonT5Test {
    function hashFour(uint256 a, uint256 b, uint256 c, uint256 d) external pure returns (uint256) {
        return PoseidonT5.hash([a, b, c, d]);
    }
}
