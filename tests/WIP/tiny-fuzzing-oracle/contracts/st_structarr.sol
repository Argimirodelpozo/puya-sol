// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract SA {
    struct P { uint128 a; int128 b; }
    P[] public arr;
    function push(uint128 a, int128 b) external { arr.push(P(a, b)); }
    function setA(uint256 i, uint128 v) external { arr[i].a = v; }      // OOB reverts
    function addB(uint256 i, int128 d)  external { arr[i].b += d; }     // signed compound on struct array element
    function pop()                      external { arr.pop(); }
    function len()                      external view returns (uint256) { return arr.length; }
    function clear()                    external { delete arr; }
}
