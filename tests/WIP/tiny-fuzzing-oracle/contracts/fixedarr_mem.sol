// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    // fixed-size memory array build + index
    function fixSum(uint64 a, uint64 b, uint64 c) external pure returns (uint64) {
        uint64[3] memory m = [a, b, c];
        unchecked { return m[0] + m[1] * 2 + m[2]; }
    }
    // fixed array element write then read
    function fixWrite(uint128 a, uint256 i) external pure returns (uint128) {
        uint128[4] memory m;
        m[0] = a; m[1] = a + 1; m[2] = a + 2; m[3] = a + 3;
        return m[i % 4];
    }
    // signed fixed array
    function fixSigned(int32 a, int32 b, uint256 i) external pure returns (int32) {
        int32[2] memory m = [a, b];
        return m[i % 2];
    }
    // nested fixed array
    function fixNested(uint64 a, uint256 i, uint256 j) external pure returns (uint64) {
        uint64[2][2] memory m = [[a, a + 1], [a + 2, a + 3]];
        return m[i % 2][j % 2];
    }
    // memory struct array
    function memStructArr(uint64 a, uint64 b, uint256 i) external pure returns (uint64) {
        uint64[] memory m = new uint64[](3);
        m[0] = a; m[1] = b; m[2] = a ^ b;
        return m[i % 3];
    }
}
