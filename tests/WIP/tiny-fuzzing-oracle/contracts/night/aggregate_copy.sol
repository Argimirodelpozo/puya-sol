// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
contract AggregateCopy {
    struct S { uint256 x; uint256 y; uint32 z; }
    S sstruct;
    uint256[] sarr;
    uint256[][] grid;
    S[] sstructs;

    // memory struct -> storage struct (whole copy)
    function memToStorStruct(uint256 x, uint256 y, uint32 z) external returns (uint256,uint256,uint32) {
        S memory m = S(x, y, z);
        sstruct = m;
        return (sstruct.x, sstruct.y, sstruct.z);
    }
    // memory array -> storage array (whole copy, replaces)
    function memToStorArr(uint256 a, uint256 b, uint256 c) external returns (uint256) {
        uint256[] memory m = new uint256[](3); m[0]=a; m[1]=b; m[2]=c;
        sarr = m;
        return sarr.length + sarr[0] + sarr[2];
    }
    // storage array -> memory (read, sum)
    function storToMem() external view returns (uint256 sum) {
        uint256[] memory m = sarr;
        for (uint256 i=0;i<m.length;i++) sum += m[i];
    }
    // 2D array push + mutate
    function pushRow(uint256 a, uint256 b) external returns (uint256) {
        uint256[] memory row = new uint256[](2); row[0]=a; row[1]=b;
        grid.push(row);
        return grid.length;
    }
    function gridGet(uint256 i, uint256 j) external view returns (uint256) { return grid[i][j]; }
    function gridSet(uint256 i, uint256 j, uint256 v) external { grid[i][j] = v; }
    // delete array element (leaves zero-gap, not shift)
    function delElem(uint256 i) external returns (uint256) { delete sarr[i]; return sarr[i]; }
    // struct array push + field mutate
    function pushS(uint256 x, uint256 y, uint32 z) external returns (uint256) { sstructs.push(S(x,y,z)); return sstructs.length; }
    function bumpS(uint256 i, uint256 d) external { sstructs[i].x += d; }
    function getS(uint256 i) external view returns (uint256,uint256,uint32) { return (sstructs[i].x, sstructs[i].y, sstructs[i].z); }
    function seedArr(uint256 n, uint256 v) external { delete sarr; for (uint256 i=0;i<n;i++) sarr.push(v+i); }
}
