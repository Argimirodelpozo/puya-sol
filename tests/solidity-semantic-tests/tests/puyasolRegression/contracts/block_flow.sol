// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract BlockFlow {
    function scopes(uint8 x) external pure returns (uint64 result) {
        result = x;
        unchecked {
            { uint8 x = 255; ++x; result += x; }
            { uint8 x = 2; result += x; }
        }
        { uint8 x = 3; result += x; }
        result += x;
    }
    function uncheckedNested(uint8 x) external pure returns (uint8) {
        unchecked { { ++x; } }
        return x;
    }
    function checkedAfter(uint8 x) external pure returns (uint8) {
        unchecked { uint8 temporary = x + 1; temporary; }
        return x + 1;
    }
    function nestedReturn(uint64 x) external pure returns (uint64) {
        { { { return x + 1; } } }
        return 999;
    }
    function branchHalt(bool halt) external pure returns (uint64) {
        if (halt) { assembly { mstore(0, 11) return(0, 32) } }
        return 22;
    }
    function bothHalt(bool first) external pure returns (uint64) {
        if (first) { { assembly { mstore(0, 31) return(0, 32) } } }
        else { { return 32; } }
        return 999;
    }
    function forHalt(bool enter) external pure returns (uint64) {
        for (uint64 i = 0; enter && i < 1; ++i) {
            { { assembly { mstore(0, 41) return(0, 32) } } }
            return 999;
        }
        return 42;
    }
    function doHalt(bool first) external pure returns (uint64) {
        do {
            if (first) { { assembly { mstore(0, 51) return(0, 32) } } }
            else { { return 52; } }
            return 999;
        } while (false);
        return 998;
    }
    function doTransfers(bool stop) external pure returns (uint64 n) {
        do {
            if (stop) { { break; } }
            else { { continue; } }
            n = 999;
        } while (++n < 3);
    }
    function forTransfers() external pure returns (uint64 i) {
        for (; i < 5; ++i) {
            if (i < 2) { { continue; } }
            else { { break; } }
            i = 999;
        }
    }
    function nestedLoops() external pure returns (uint64 result) {
        for (uint64 i = 0; i < 3; ++i) {
            for (uint64 j = 0; j < 3; ++j) {
                if (j == 1) continue;
                ++result;
            }
            if (i == 1) continue;
            result += 10;
        }
    }
    function braceless(bool choose) external pure returns (uint64 n) {
        if (choose) n = 1; else n = 2;
        while (n < 3) ++n;
        do ++n; while (n < 5);
        for (; n < 7; ++n) {}
    }
    function nestedRevert(bool fail) external pure returns (uint64) {
        if (fail) { { { assembly { revert(0, 0) } } } }
        return 61;
    }
}
