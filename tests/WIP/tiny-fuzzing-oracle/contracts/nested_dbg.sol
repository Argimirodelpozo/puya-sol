// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract NestedDbg {
    function construct(uint256 a) external pure returns (uint256) {
        uint256[][] memory x = new uint256[][](2);
        x[0] = new uint256[](2); x[0][0] = a; x[0][1] = a + 1;
        x[1] = new uint256[](1); x[1][0] = a + 2;
        return x[0][0] + x[0][1] + x[1][0];          // construct + access, no abi
    }
    function encOnly(uint256 a) external pure returns (uint256) {
        uint256[][] memory x = new uint256[][](1);
        x[0] = new uint256[](1); x[0][0] = a;
        return abi.encode(x).length;                 // encode only
    }
    function decRT(uint256 a) external pure returns (uint256) {
        uint256[][] memory x = new uint256[][](1);
        x[0] = new uint256[](1); x[0][0] = a;
        uint256[][] memory d = abi.decode(abi.encode(x), (uint256[][]));
        return d[0][0];                              // full round-trip
    }
}
