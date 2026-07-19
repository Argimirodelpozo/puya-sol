// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract G {
    function f0(int16 a, int16 b, int16 c, int16 d) external pure returns (int16) {
        return (((int16(uint16((d % ((c > ((b < d) ? a : c)) ? (b ^ d) : ((a == c) ? b : c)))))) > ((((((d - type(int16).min) >= ((b > type(int16).min) ? c : a)) ? (-a) : (a + d)) + (((a / a) >= c) ? type(int16).min : type(int16).min)) >= (((b != d) ? ((b != c) ? type(int16).min : c) : ((a == c) ? type(int16).min : c)) * ((((d <= b) ? b : b) > type(int16).min) ? ((b < a) ? a : c) : b))) ? ((b >= (((c / c) == ((c == d) ? a : a)) ? (type(int16).min | b) : (-d))) ? (((d + c) < b) ? c : a) : c) : ((c != (d % a)) ? (-(-a)) : type(int16).min))) ? (((((b | (-d)) > type(int16).min) ? (int16(int8(a))) : (((b >= type(int16).min) ? d : type(int16).min) & ((b <= a) ? b : c))) <= (int16(int64((((c < a) ? d : a) / (b + type(int16).min)))))) ? type(int16).min : (((d > ((d > b) ? a : d)) ? ((d == type(int16).min) ? b : type(int16).min) : ((type(int16).min == type(int16).min) ? d : b)) / (((-a) > (type(int16).min + b)) ? type(int16).min : (type(int16).min % c)))) : (((((((d < a) ? c : b) >= ((c != c) ? d : b)) ? ((c < type(int16).min) ? a : type(int16).min) : c) / (((c <= type(int16).min) ? type(int16).min : c) - c)) >= (((((c <= a) ? d : c) / (c * type(int16).min)) > (((type(int16).min >= d) ? c : a) ^ (c % type(int16).min))) ? ((((type(int16).min < a) ? type(int16).min : c) == ((c >= a) ? type(int16).min : d)) ? (a * c) : ((d <= type(int16).min) ? c : a)) : type(int16).min)) ? (b / (-b)) : ((c < b) ? ((((c == type(int16).min) ? a : type(int16).min) < (type(int16).min % a)) ? a : c) : (a + ((a >= d) ? a : type(int16).min)))));
    }
    function f1(int16 a, int16 b, int16 c, int16 d) external pure returns (int16) {
        return d;
    }
}
