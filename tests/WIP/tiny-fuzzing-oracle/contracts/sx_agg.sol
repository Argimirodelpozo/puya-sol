// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract SxAgg {
    int16[] arr;
    struct S { int16 f; }
    S s;
    mapping(uint256 => int16) m;
    int16 stateVar;
    function arrElem(int256 x) external returns (int256) {
        int8 a = int8(x); delete arr; arr.push(0); arr[0] = a; return arr[0];
    }
    function structField(int256 x) external returns (int256) {
        int8 a = int8(x); s.f = a; return s.f;
    }
    function mapVal(int256 x) external returns (int256) {
        int8 a = int8(x); m[0] = a; return m[0];
    }
    function stateAssign(int256 x) external returns (int256) {
        int8 a = int8(x); stateVar = a; return stateVar;
    }
    function memArrElem(int256 x) external pure returns (int256) {
        int8 a = int8(x); int16[] memory ma = new int16[](1); ma[0] = a; return ma[0];
    }
}
