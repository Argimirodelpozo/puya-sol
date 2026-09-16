pragma solidity ^0.8.20;

contract EbArithmetic {
    function mul32(uint32 a, uint32 b) external pure returns (uint32) { unchecked { return a * b; } }
    function mul40(uint40 a, uint40 b) external pure returns (uint40) { unchecked { return a * b; } }
    function mul48(uint48 a, uint48 b) external pure returns (uint48) { unchecked { return a * b; } }
    function mul56(uint56 a, uint56 b) external pure returns (uint56) { unchecked { return a * b; } }
    function mul64(uint64 a, uint64 b) external pure returns (uint64) { unchecked { return a * b; } }
    function compound40(uint40 a, uint40 b) external pure returns (uint40) { unchecked { a *= b; return a; } }
    function checked40(uint40 a, uint40 b) external pure returns (uint40) { return a * b; }
    function shiftNeg128(int128 a) external pure returns (int128) { return -(a << 1); }
    function notNeg128(int128 a) external pure returns (int128) { return -(~a); }
    function shiftNeg8(int8 a) external pure returns (int8) { return -(a << 1); }
    function notNeg8(int8 a) external pure returns (int8) { return -(~a); }
    function complement(int128 a) external pure returns (bool) { return ~a < 0; }
    function shifted(int128 a) external pure returns (bool) { return (a << 1) < 0; }
}
