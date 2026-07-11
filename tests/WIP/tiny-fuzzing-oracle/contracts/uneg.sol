// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    function neg16(int16 a)  external pure returns (int16)  { unchecked { return -a; } }   // -INT16_MIN wraps to INT16_MIN
    function neg8(int8 a)    external pure returns (int8)   { unchecked { return -a; } }
    function neg128(int128 a)external pure returns (int128) { unchecked { return -a; } }
    function neg256(int256 a)external pure returns (int256) { unchecked { return -a; } }
    function cmp16(int16 a)  external pure returns (bool)    { unchecked { return (-a) > a; } } // MIN: false (both MIN)
}
