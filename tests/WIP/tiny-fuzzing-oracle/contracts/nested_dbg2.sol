// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract NestedDbg2 {
    function twoOuter(uint256 a) external pure returns (uint256) {
        uint256[][] memory x = new uint256[][](2);
        x[0] = new uint256[](1); x[0][0] = a;
        x[1] = new uint256[](1); x[1][0] = a + 1;
        uint256[][] memory d = abi.decode(abi.encode(x), (uint256[][]));
        return d[0][0] + d[1][0];
    }
    function twoInner(uint256 a) external pure returns (uint256) {
        uint256[][] memory x = new uint256[][](1);
        x[0] = new uint256[](2); x[0][0] = a; x[0][1] = a + 1;
        uint256[][] memory d = abi.decode(abi.encode(x), (uint256[][]));
        return d[0][0] + d[0][1];
    }
    function emptyInner(uint256 a) external pure returns (uint256) {
        uint256[][] memory x = new uint256[][](1);
        x[0] = new uint256[](0);
        uint256[][] memory d = abi.decode(abi.encode(x), (uint256[][]));
        return d.length + d[0].length;
    }
    function varInner(uint256 n) external pure returns (uint256) {
        uint256[][] memory x = new uint256[][](1);
        x[0] = new uint256[](n % 3); for (uint256 k=0;k<x[0].length;k++) x[0][k]=k;
        uint256[][] memory d = abi.decode(abi.encode(x), (uint256[][]));
        return d[0].length;
    }
}
