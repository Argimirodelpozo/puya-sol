// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract G {
    function f0(int256[] calldata arr, int256[][] calldata mat, int256 a, int256 b, int256 c, int256 d) external pure returns (int256) {
        int256 acc = a;
        unchecked {
        for (uint i0 = 0; i0 < arr.length; i0++) { if ((((acc & b) - (a | d)) != (a ** 2))) break; acc ^= arr[i0]; }
        for (uint i1 = 0; i1 < arr.length; i1++) { acc += arr[i1]; }
        acc += ((-(c % b)) ^ (c - acc));
        }
        return acc;
    }
    function f1(uint8[] calldata arr, uint8[][] calldata mat, uint8 a, uint8 b, uint8 c, uint8 d) external pure returns (uint8) {
        uint8 acc = a;
        unchecked {
        for (uint i0 = 0; i0 < mat.length; i0++) { for (uint i1 = 0; i1 < mat[i0].length; i1++) { acc = mat[i0][i1]; } }
        for (uint i2 = 0; i2 < mat.length; i2++) { for (uint i3 = 0; i3 < mat[i2].length; i3++) { acc = mat[i2][i3]; } }
        }
        return acc;
    }
    function f2(uint16[] calldata arr, uint16[][] calldata mat, uint16 a, uint16 b, uint16 c, uint16 d) external pure returns (uint16) {
        uint16 acc = a;
        unchecked {
        for (uint i0 = 0; i0 < arr.length; i0++) { if ((((~type(uint16).max) / (b * b)) <= ((b * c) ** 0))) break; acc *= arr[i0]; }
        for (uint i1 = 0; i1 < mat.length; i1++) { for (uint i2 = 0; i2 < mat[i1].length; i2++) { acc += mat[i1][i2]; } }
        }
        return acc;
    }
    function f3(int16[] calldata arr, int16[][] calldata mat, int16 a, int16 b, int16 c, int16 d) external pure returns (int16) {
        int16 acc = a;
        unchecked {
        for (uint i0 = 0; i0 < 2; i0++) { if ((acc >= ((acc | d) - (type(int16).min + b)))) continue; for (uint i1 = 0; i1 < 2; i1++) { if (((~(d >> 8)) < (~(-d)))) continue; acc -= acc; } }
        for (uint i2 = 0; i2 < 4; i2++) { if ((type(int16).min <= (c ** 0))) break; for (uint i3 = 0; i3 < 3; i3++) { acc ^= ((b + c) + (a / d)); } }
        }
        return acc;
    }
    function f4(int128[] calldata arr, int128[][] calldata mat, int128 a, int128 b, int128 c, int128 d) external pure returns (int128) {
        int128 acc = a;
        unchecked {
        for (uint i0 = 0; i0 < arr.length; i0++) { acc *= arr[i0]; }
        for (uint i1 = 0; i1 < mat.length; i1++) { for (uint i2 = 0; i2 < mat[i1].length; i2++) { acc |= mat[i1][i2]; } }
        for (uint i3 = 0; i3 < arr.length; i3++) { acc = arr[i3]; }
        }
        return acc;
    }
    function f5(int64[] calldata arr, int64[][] calldata mat, int64 a, int64 b, int64 c, int64 d) external pure returns (int64) {
        int64 acc = a;
        unchecked {
        for (uint i0 = 0; i0 < arr.length; i0++) { acc = arr[i0]; }
        for (uint i1 = 0; i1 < mat.length; i1++) { for (uint i2 = 0; i2 < mat[i1].length; i2++) { acc *= mat[i1][i2]; } }
        }
        return acc;
    }
}
