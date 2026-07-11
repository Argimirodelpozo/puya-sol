// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract SxSites {
    function viaAssign(int256 x) external pure returns (int256) {
        int8 a = int8(x); int16 b = 0; b = a; return b;     // plain assignment widen
    }
    function viaReturn(int256 x) external pure returns (int16) {
        int8 a = int8(x); return a;                          // return-type widen
    }
    function viaArg(int256 x) external pure returns (int16) {
        int8 a = int8(x); return _id16(a);                   // function-arg widen
    }
    function _id16(int16 v) internal pure returns (int16) { return v; }
    function viaTernary(int256 x, bool f) external pure returns (int16) {
        int8 a = int8(x); int16 w = 7; return f ? a : w;     // ternary common-type widen
    }
}
