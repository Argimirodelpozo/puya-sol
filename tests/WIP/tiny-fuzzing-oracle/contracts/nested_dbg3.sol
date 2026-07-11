// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract NestedDbg3 {
    function emptyOuter() external pure returns (uint256) {
        uint256[][] memory x = new uint256[][](0);
        uint256[][] memory d = abi.decode(abi.encode(x), (uint256[][]));
        return d.length;                              // 0
    }
    function loopOuter(uint256 n) external pure returns (uint256) {
        uint256[][] memory x = new uint256[][](n % 3 + 1);  // 1..3 outer
        for (uint256 i=0;i<x.length;i++){ x[i]=new uint256[](1); x[i][0]=i; }
        uint256[][] memory d = abi.decode(abi.encode(x), (uint256[][]));
        uint256 s; for(uint256 i=0;i<d.length;i++) s+=d[i][0]; return s;
    }
    function loopVarInner(uint256 n, uint256 m) external pure returns (uint256) {
        uint256[][] memory x = new uint256[][](n % 3 + 1);
        for (uint256 i=0;i<x.length;i++){ x[i]=new uint256[](m % 3); for(uint256 j=0;j<x[i].length;j++) x[i][j]=i*10+j; }
        uint256[][] memory d = abi.decode(abi.encode(x), (uint256[][]));
        uint256 s; for(uint256 i=0;i<d.length;i++) for(uint256 j=0;j<d[i].length;j++) s+=d[i][j]; return s;
    }
}
