// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract SolTypesArrayLoop {
    uint16[259] private values;

    function loopCopy() external returns (uint256, uint256, uint256) {
        uint8[257] memory source;
        source[0] = 17;
        source[256] = 255;
        values[258] = 99;
        values = source;
        return (values[0], values[256], values[258]);
    }
}
