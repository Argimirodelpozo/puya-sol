// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract LoopPrefixes {
    uint64 private calls;

    function next() internal returns (uint64) { return ++calls; }
    function step(uint64 i) internal returns (uint64) { ++calls; return i + 1; }
    function condition(uint64[] memory state, uint64 limit) internal returns (bool) {
        ++calls;
        return ++state[0] < limit;
    }

    function forPrefixes(uint64 mask, uint64 stop) external returns (uint64, uint64, uint64[4] memory) {
        calls = 0;
        uint64[4] memory values;
        uint64 i;
        uint64 sum;
        // Double inversion forces SingleEvaluation without changing the value.
        for (; i < 4; values[i - 1] += ~~next()) {
            if (i == stop) break;
            ++i;
            if (i == 1 && (mask & 1) != 0) continue;
            if (i == 3 && (mask & 2) != 0) continue;
            sum += i;
        }
        return (sum, calls, values);
    }

    function doPrefixes(uint64 mask) external returns (uint64, uint64, uint64, uint64) {
        calls = 0;
        uint64[] memory state = new uint64[](1);
        uint64 n;
        uint64 sum;
        do {
            ++n;
            if (n == 1 && (mask & 1) != 0) continue;
            if (n == 2 && (mask & 2) != 0) continue;
            sum += n;
        } while (!!condition(state, 4));
        return (n, state[0], calls, sum);
    }

    function nested() external returns (uint64 sum, uint64 count) {
        calls = 0;
        for (uint64 i; i < 3; i = step(i)) {
            uint64[] memory state = new uint64[](1);
            do {
                if (state[0] == 0) continue;
                if (state[0] == 2) continue;
                sum += i + state[0];
            } while (condition(state, 4));
            if (i == 1) continue;
            sum += 10;
        }
        return (sum, calls);
    }

    function forChecked() external pure returns (uint8 i) {
        i = 255;
        uint8 n;
        for (; n < 2; ++i) { unchecked { ++n; continue; } }
    }
    function doChecked() external pure returns (uint8 i) {
        i = 255;
        uint8 n;
        do { unchecked { ++n; if (n < 2) continue; break; } } while (++i > 0);
    }
    function forUnchecked() external pure returns (uint8 i) {
        unchecked {
            i = 255;
            uint8 n;
            for (; n < 2; ++i) { ++n; continue; }
        }
    }
    function doUnchecked() external pure returns (uint8 i) {
        unchecked {
            i = 255;
            do { continue; } while (++i < 1);
        }
    }
}
