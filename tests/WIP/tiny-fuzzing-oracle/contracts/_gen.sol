// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract G {
    function f0(int16[] calldata arr, int16[][] calldata mat, int16 a, int16 b, int16 c, int16 d) external pure returns (int16) {
        int16 acc = a;
        unchecked {
        for (uint i0 = 0; i0 < mat.length; i0++) { for (uint i1 = 0; i1 < mat[i0].length; i1++) { acc = mat[i0][i1]; } }
        for (uint i2 = 0; i2 < arr.length; i2++) { if ((((c * acc) & c) == ((int16(int64(acc))) | acc))) continue; acc -= arr[i2]; }
        }
        return acc;
    }
    function f1(uint128[] calldata arr, uint128[][] calldata mat, uint128 a, uint128 b, uint128 c, uint128 d) external pure returns (uint128) {
        uint128 acc = a;
        unchecked {
        for (uint i0 = 0; i0 < arr.length; i0++) { acc *= arr[i0]; }
        if ((acc != (~(type(uint128).max * type(uint128).max)))) { acc = ((acc ** 1) ** 3); } else { acc &= (~a); }
        }
        return acc;
    }
    function f2(uint8[] calldata arr, uint8[][] calldata mat, uint8 a, uint8 b, uint8 c, uint8 d) external pure returns (uint8) {
        uint8 acc = a;
        unchecked {
        for (uint i0 = 0; i0 < mat.length; i0++) { for (uint i1 = 0; i1 < mat[i0].length; i1++) { acc *= mat[i0][i1]; } }
        for (uint i2 = 0; i2 < mat.length; i2++) { for (uint i3 = 0; i3 < mat[i2].length; i3++) { acc |= mat[i2][i3]; } }
        }
        return acc;
    }
    function f3(uint64[] calldata arr, uint64[][] calldata mat, uint64 a, uint64 b, uint64 c, uint64 d) external pure returns (uint64) {
        uint64 acc = a;
        unchecked {
        acc *= type(uint64).max;
        for (uint i0 = 0; i0 < arr.length; i0++) { if ((((b & b) - (b ** 1)) == (uint64(uint128((d % a)))))) continue; acc = arr[i0]; }
        for (uint i1 = 0; i1 < arr.length; i1++) { if ((c == (b ^ d))) continue; acc |= arr[i1]; }
        }
        return acc;
    }
    function f4(uint16[] calldata arr, uint16[][] calldata mat, uint16 a, uint16 b, uint16 c, uint16 d) external pure returns (uint16) {
        uint16 acc = a;
        unchecked {
        for (uint i0 = 0; i0 < mat.length; i0++) { for (uint i1 = 0; i1 < mat[i0].length; i1++) { acc += mat[i0][i1]; } }
        for (uint i2 = 0; i2 < arr.length; i2++) { if ((((b + c) | (type(uint16).max >> 255)) <= type(uint16).max)) continue; acc &= arr[i2]; }
        for (uint i3 = 0; i3 < 2; i3++) { if ((((a / c) & d) < c)) continue; for (uint i4 = 0; i4 < 4; i4++) { acc += (~acc); } }
        }
        return acc;
    }
    function f5(int16[] calldata arr, int16[][] calldata mat, int16 a, int16 b, int16 c, int16 d) external pure returns (int16) {
        int16 acc = a;
        unchecked {
        for (uint i0 = 0; i0 < arr.length; i0++) { if ((((b / c) % (type(int16).min % b)) != ((type(int16).min - type(int16).min) ^ type(int16).min))) break; acc += arr[i0]; }
        if ((acc > (d ** 0))) { for (uint i1 = 0; i1 < 4; i1++) { acc |= type(int16).min; } } else { acc += (((a & type(int16).min) >= (acc % c)) ? (a + b) : (d / c)); }
        }
        return acc;
    }
}
