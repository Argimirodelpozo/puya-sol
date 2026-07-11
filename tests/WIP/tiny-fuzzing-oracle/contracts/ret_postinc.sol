// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract G {
    uint64 x;
    int64 y;
    function setX(uint64 v) external { x = v; }
    function retPostX() external returns (uint64) { return x++; }   // postfix return on scalar state var
    function stmtX() external { x++; }
    function getX() external view returns (uint64) { return x; }
    function setY(int64 v) external { y = v; }
    function stmtY() external { y++; }
    function getY() external view returns (int64) { return y; }
}
