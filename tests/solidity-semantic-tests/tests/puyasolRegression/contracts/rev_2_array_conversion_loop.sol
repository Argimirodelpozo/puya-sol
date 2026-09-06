// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract ArrayConversionLoop {
    uint256 internal calls;
    int16[258] internal values;

    function next() internal returns (int8) { ++calls; return -3; }
    function source() internal returns (int8[257] memory result) {
        result[0] = next();
        result[256] = next();
    }
    function run() external returns (uint256, int16, int16, int16, int16) {
        calls = 0;
        values = source();
        return (calls, values[0], values[128], values[256], values[257]);
    }
}
