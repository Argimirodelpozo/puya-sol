// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// NOT an o.g. semantic test. Guards deploy-time box SIZING for a multi-dimensional fixed array.
// `int256[2][2] grid` is a single box of 4*32=128 bytes. The deploy-time (__postInit) box_create
// sized it from a manual elementType() switch that handled only ARC4UIntN/Bytes — a NESTED static
// array element (int256[2]) fell to the default elemSize=32, so the box was created at 32*2=64 bytes
// instead of 128. Writing grid[1][j] (offset >= 64) then hit "replacement end beyond original length".
// Fixed by sizing from StorageMapper::arc4StaticArrayTotalBytes (recursive element size => 128).
contract G {
    int256[2][2] grid;
    function setGrid(uint256 i, uint256 j, int256 v) external { if (i < 2 && j < 2) grid[i][j] = v; }
    function getGrid(uint256 i, uint256 j) external view returns (int256) {
        return (i < 2 && j < 2) ? grid[i][j] : int256(0);
    }
}
