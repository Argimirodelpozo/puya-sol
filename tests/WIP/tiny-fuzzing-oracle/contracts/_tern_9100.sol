// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract G {
    function f0(int8 a, int8 b, int8 c, int8 d) external pure returns (int8) {
        unchecked { return ((a > (-((d > a) ? (d | d) : a))) ? ((((((c != d) ? d : c) < (-a)) ? (type(int8).min / b) : (int8(uint8(b)))) == a) ? (((d + b) <= (d - type(int8).min)) ? ((d != type(int8).min) ? d : c) : (-d)) : (-a)) : ((((((a == type(int8).min) ? c : d) != ((type(int8).min == a) ? b : type(int8).min)) ? (-c) : ((d >= c) ? type(int8).min : b)) > ((((c != c) ? type(int8).min : b) > ((c < a) ? c : a)) ? type(int8).min : d)) ? type(int8).min : b)); }
    }
    function f1(int40 a, int40 b, int40 c, int40 d) external pure returns (int40) {
        unchecked { return ((int40(int16(a))) - ((c > type(int40).min) ? b : type(int40).min)); }
    }
    function f2(int64 a, int64 b, int64 c, int64 d) external pure returns (int64) {
        unchecked { return (d * (((type(int64).min == d) ? a : c) / ((d <= b) ? d : c))); }
    }
    function f3(int16 a, int16 b, int16 c, int16 d) external pure returns (int16) {
        unchecked { return ((((((-d) * (a / type(int16).min)) < (((c >= d) ? c : c) / (type(int16).min ^ d))) ? b : type(int16).min) >= a) ? (((((d * b) > ((a > c) ? d : type(int16).min)) ? (c & c) : (-d)) == ((d >= b) ? type(int16).min : (int16(int128(d))))) ? (((c - b) > ((d >= a) ? c : d)) ? ((type(int16).min != a) ? b : type(int16).min) : c) : type(int16).min) : (((((a / type(int16).min) >= type(int16).min) ? ((type(int16).min < type(int16).min) ? d : d) : ((type(int16).min != a) ? d : a)) <= c) ? type(int16).min : ((((c > a) ? d : c) <= c) ? a : ((c >= b) ? c : d)))); }
    }
    function f4(int128 a, int128 b, int128 c, int128 d) external pure returns (int128) {
        return (((-(b - (int128(int256(type(int128).min))))) < (((-a) - (d & d)) * (((d > c) ? d : c) ^ c))) ? (((((b >= d) ? d : d) / ((c < b) ? d : type(int128).min)) >= ((((b == type(int128).min) ? c : type(int128).min) > (c & type(int128).min)) ? (int128(int16(type(int128).min))) : a)) ? ((a != (-c)) ? (type(int128).min % type(int128).min) : ((a < c) ? type(int128).min : type(int128).min)) : (-a)) : (((((c - b) > ((c == type(int128).min) ? c : type(int128).min)) ? b : (int128(int256(b)))) >= d) ? ((((a < type(int128).min) ? d : d) < ((c > d) ? a : b)) ? ((b > c) ? b : type(int128).min) : (type(int128).min ^ c)) : b));
    }
    function f5(int64 a, int64 b, int64 c, int64 d) external pure returns (int64) {
        return ((-d) % c);
    }
    function f6(int24 a, int24 b, int24 c, int24 d) external pure returns (int24) {
        return (((int24(int16(c))) != (a / c)) ? ((type(int24).min != type(int24).min) ? type(int24).min : c) : c);
    }
    function f7(int32 a, int32 b, int32 c, int32 d) external pure returns (int32) {
        return a;
    }
    function f8(int32 a, int32 b, int32 c, int32 d) external pure returns (int32) {
        unchecked { return (((((((d * type(int32).min) > ((d >= type(int32).min) ? d : c)) ? a : (a % a)) <= c) ? (c / ((a < a) ? a : type(int32).min)) : (((a < d) ? type(int32).min : a) % (a - c))) < b) ? ((d == d) ? d : ((type(int32).min / a) ^ (int32(int64(b))))) : (((int32(uint32((a | c)))) == ((((b <= c) ? type(int32).min : b) > (type(int32).min + b)) ? ((b >= a) ? b : d) : c)) ? ((((b == type(int32).min) ? a : c) >= c) ? (int32(int16(b))) : (type(int32).min % d)) : (((c - a) < (a ^ b)) ? b : ((a >= d) ? a : a)))); }
    }
    function f9(int128 a, int128 b, int128 c, int128 d) external pure returns (int128) {
        unchecked { return a; }
    }
    function f10(int128 a, int128 b, int128 c, int128 d) external pure returns (int128) {
        unchecked { return ((a >= (c * b)) ? (-c) : ((a == c) ? a : type(int128).min)); }
    }
    function f11(int16 a, int16 b, int16 c, int16 d) external pure returns (int16) {
        unchecked { return a; }
    }
}
