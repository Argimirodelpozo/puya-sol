// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract G {
    function f0(int24 a, int24 b, int24 c, int24 d) external pure returns (int24) {
        unchecked { return ((((((((a == b) ? d : d) != b) ? ((d < b) ? b : c) : ((d < d) ? b : b)) < a) ? (-((b != a) ? a : c)) : a) == (((((c < type(int24).min) ? type(int24).min : d) | (type(int24).min + c)) > (a / c)) ? ((((type(int24).min >= a) ? b : b) != (c * b)) ? (c + b) : a) : ((d > ((b == type(int24).min) ? a : type(int24).min)) ? ((a >= c) ? a : type(int24).min) : ((a >= type(int24).min) ? c : b)))) ? (((-a) >= (d * d)) ? c : ((a == ((b < d) ? b : d)) ? (c | b) : (b / b))) : (((int24(int16(d))) == b) ? a : (((b / type(int24).min) < (d / c)) ? (int24(int256(c))) : a))); }
    }
    function f1(int128 a, int128 b, int128 c, int128 d) external pure returns (int128) {
        unchecked { return ((c != ((b > (((type(int128).min == c) ? c : d) | ((a < a) ? c : d))) ? c : a)) ? ((c > type(int128).min) ? a : ((c & d) ^ (-c))) : (((a * (type(int128).min | d)) != (d - ((d != a) ? d : a))) ? ((int128(int16(a))) ^ (int128(int256(a)))) : d)); }
    }
    function f2(int40 a, int40 b, int40 c, int40 d) external pure returns (int40) {
        unchecked { return (((int40(int16(b))) != ((a < a) ? type(int40).min : a)) ? b : a); }
    }
    function f3(int8 a, int8 b, int8 c, int8 d) external pure returns (int8) {
        return ((d >= ((type(int8).min > d) ? d : b)) ? (int8(int64(b))) : ((b > d) ? a : a));
    }
    function f4(int64 a, int64 b, int64 c, int64 d) external pure returns (int64) {
        return ((((a > (-(type(int64).min / a))) ? ((type(int64).min == (c - type(int64).min)) ? (int64(int16(d))) : ((c < b) ? a : c)) : (int64(int16(((b <= b) ? b : b))))) <= d) ? (((-b) != (a & (type(int64).min & type(int64).min))) ? (int64(int128(a))) : ((a <= (b | c)) ? (int64(uint64(a))) : ((c > d) ? a : b))) : (((-((type(int64).min <= b) ? c : d)) <= (((-a) <= (a * a)) ? (int64(int8(c))) : (b / type(int64).min))) ? d : ((((a != type(int64).min) ? a : a) != ((b != b) ? a : b)) ? a : (d & d))));
    }
    function f5(int8 a, int8 b, int8 c, int8 d) external pure returns (int8) {
        unchecked { return (int8(int128(((c <= c) ? b : b)))); }
    }
}
