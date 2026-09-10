// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract IncDecValues {
    function indexedValues(uint64 x) external pure returns (uint64[5] memory r, uint64, uint64) {
        uint64[] memory a = new uint64[](1);
        a[0] = x;
        uint64 i;
        r[0] = ++a[i++];
        r[1] = a[--i]++;
        r[2] = --a[i++];
        r[3] = a[--i]--;
        assert(++a[0] > x);
        r[4] = a[0];
        return (r, a[0], i);
    }

    function narrow(uint8 x, bool dec, bool post, bool wrap) external pure returns (uint8 r, uint8) {
        uint8[] memory a = new uint8[](1);
        a[0] = x;
        if (wrap) {
            unchecked {
                if (dec) r = post ? a[0]-- : --a[0];
                else r = post ? a[0]++ : ++a[0];
            }
        } else {
            if (dec) r = post ? a[0]-- : --a[0];
            else r = post ? a[0]++ : ++a[0];
        }
        return (r, a[0]);
    }

    function wide(uint128 x) external pure returns (uint128[6] memory r) {
        uint128[2] memory a = [x, x];
        r[0] = ++a[0];
        r[1] = a[0]++;
        r[2] = --a[1];
        r[3] = a[1]--;
        r[4] = a[0];
        r[5] = a[1];
    }

    function signedValue(int8 x, bool dec, bool post, bool wrap) external pure returns (int8 r, int8) {
        int8[1] memory a = [x];
        if (wrap) {
            unchecked {
                if (dec) r = post ? a[0]-- : --a[0];
                else r = post ? a[0]++ : ++a[0];
            }
        } else {
            if (dec) r = post ? a[0]-- : --a[0];
            else r = post ? a[0]++ : ++a[0];
        }
        return (r, a[0]);
    }
}
