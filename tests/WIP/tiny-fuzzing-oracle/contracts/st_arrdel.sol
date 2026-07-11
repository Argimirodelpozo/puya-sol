// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// Storage dynamic array push/pop/delete + mapping delete across a sequence.
contract DR {
    uint256[] public arr;
    mapping(uint256 => uint256) public m;
    function push(uint256 v)        external { arr.push(v); }
    function pop()                  external { arr.pop(); }              // reverts on empty
    function setI(uint256 i, uint256 v) external { arr[i] = v; }        // reverts OOB
    function len()                  external view returns (uint256) { return arr.length; }
    function clearArr()             external { delete arr; }
    function setM(uint256 k, uint256 v) external { m[k] = v; }
    function addM(uint256 k, uint256 v) external { m[k] += v; }
    function delM(uint256 k)        external { delete m[k]; }
}
