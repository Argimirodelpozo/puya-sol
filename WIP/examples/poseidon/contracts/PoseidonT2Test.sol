/// SPDX-License-Identifier: MIT
pragma solidity >=0.7.0;

import "./PoseidonT2.sol";

contract PoseidonT2Test {
    function hashOne(uint256 input0) external pure returns (uint256) {
        return PoseidonT2.hash([input0]);
    }
}
