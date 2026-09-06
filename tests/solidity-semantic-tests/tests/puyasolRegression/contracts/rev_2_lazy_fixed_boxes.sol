// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

contract LazyFixedBoxes {
    mapping(uint256 => uint256[128]) public edge;
    mapping(uint256 => uint256[129]) public above;
    mapping(uint256 => uint256[1024]) public cap;
    mapping(uint256 => uint256[129][2]) public nested;
    struct Pair { int128 x; uint16 y; bool flag; bytes3 tag; }
    mapping(uint256 => Pair[257]) public pairs;
    uint256 public keyCalls;
    uint256 public indexCalls;

    function readAbove(uint256 k, uint256 i) external view returns (uint256) { return above[k][i]; }
    function readNested(uint256 k, uint256 i, uint256 j) external view returns (uint256) { return nested[k][i][j]; }
    function readPair(uint256 k, uint256 i) external view returns (int128, uint16, bool, bytes3) {
        Pair storage p = pairs[k][i];
        return (p.x, p.y, p.flag, p.tag);
    }
    function copyPair(uint256 k, uint256 i) external view returns (int128, uint16, bool, bytes3) {
        Pair memory p = pairs[k][i];
        return (p.x, p.y, p.flag, p.tag);
    }
    function returnPair(uint256 k, uint256 i) external view returns (Pair memory) { return pairs[k][i]; }
    function assignPair(uint256 k, uint256 i) external view returns (int128, uint16, bool, bytes3) {
        Pair memory p;
        p = pairs[k][i];
        return (p.x, p.y, p.flag, p.tag);
    }
    function argumentPair(uint256 k, uint256 i) external view returns (int128, uint16, bool, bytes3) {
        return memoryPair(pairs[k][i]);
    }
    function memoryPair(Pair memory p) internal pure returns (int128, uint16, bool, bytes3) {
        return (p.x, p.y, p.flag, p.tag);
    }
    function viaAlias(uint256 k, uint256 i) external view returns (uint256) {
        uint256[129] storage a = above[k];
        return readAlias(a, i);
    }
    function readAlias(uint256[129] storage a, uint256 i) internal view returns (uint256) { return a[i]; }
    function key(uint256 k) internal returns (uint256) { ++keyCalls; return k; }
    function index(uint256 i) internal returns (uint256) { ++indexCalls; return i; }
    function effectful(uint256 k, uint256 i) external returns (uint256, uint256, uint256) {
        uint256 v = above[key(k)][index(i)];
        return (v, keyCalls, indexCalls);
    }
    function setAbove(uint256 k, uint256 i, uint256 v) external { above[k][i] = v; }
    function addAbove(uint256 k, uint256 i, uint256 v) external { above[k][i] += v; }
    function setEdge(uint256 k, uint256 i, uint256 v) external { edge[k][i] = v; }
    function setCap(uint256 k, uint256 i, uint256 v) external { cap[k][i] = v; }
    function setNested(uint256 k, uint256 i, uint256 j, uint256 v) external { nested[k][i][j] = v; }
    function setPair(uint256 k, uint256 i) external {
        pairs[k][i].x = -32769;
        pairs[k][i].y = 65535;
        pairs[k][i].flag = true;
        pairs[k][i].tag = hex"123456";
    }
    function clear(uint256 k) external { delete above[k]; delete nested[k]; delete pairs[k]; }
}
