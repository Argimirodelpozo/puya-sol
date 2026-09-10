// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

function freeMixed(uint64 x) pure returns (uint64, uint64 value, uint64) {
    value = x + 1;
}

library MixedLibrary {
    function update(uint64[] memory state) internal pure returns (uint64 value, uint64, uint64) {
        state[0] += 2;
        value = state[0];
    }
}

contract MixedReturns {
    // Original reproducer: two empty names must not become duplicate fields.
    function f() external pure returns (uint64[1] memory r, uint64, uint64) {
        return (r, 1, 2);
    }

    function middle(uint64 x) external pure returns (uint64, int16 value, uint64) {
        value = -3;
        return (x, value, x + 1);
    }

    function last(uint64 x) external pure returns (uint64, uint64, uint64 value) {
        value = x + 2;
        return (x, x + 1, value);
    }

    function singleGap(uint64 x) external pure returns (uint64 value, uint64) {
        value = x + 1;
        return (value, x);
    }

    function implicitValues(bool early) external pure returns (uint64[1] memory r, uint64, bool, uint16 code) {
        r[0] = 7;
        code = 11;
        if (early) return (r, 2, true, 13);
    }

    function implicitHead() external pure returns (uint64, uint64 value, uint64) { value = 9; }
    function implicitTail() external pure returns (uint64, uint64, uint64 value) { value = 9; }
    function implicitDynamic() external pure returns (bytes memory data, uint64, uint64 marker) { marker = 9; }
    function unnamedAggregates() external pure returns (uint64 value, bytes memory, uint64[1] memory) { value = 9; }

    function inner(uint64 x) internal pure returns (uint64[1] memory r, uint64, uint64) {
        r[0] = x;
        return (r, x + 1, x + 2);
    }

    function forward(uint64 x) external pure returns (uint64[1] memory r, uint64, uint64) {
        return inner(x);
    }

    function freeCaller(uint64 x) external pure returns (uint64, uint64 value, uint64) {
        return freeMixed(x);
    }

    function libraryCaller(uint64 x) external pure returns (uint64, uint64 value, uint64, uint64) {
        uint64[] memory state = new uint64[](1);
        state[0] = x;
        (uint64 a, uint64 b, uint64 c) = MixedLibrary.update(state);
        return (a, b, c, state[0]);
    }

    modifier twice() { _; _; }
    modifier when(bool run) { if (run) _; }

    function modified(uint64 x) external pure twice returns (uint64, uint64 value, uint64) {
        value += x;
    }

    function skipped(bool run) external pure when(run) returns (uint64 value, uint64, uint64) {
        value = 9;
    }

    function fullyNamed() public pure returns (uint64 first, uint64 second) { first = 3; second = 4; }
    function forwardNamed() external pure returns (uint64, uint64) { return fullyNamed(); }
    function allUnnamed() external pure returns (uint64, uint64, uint64) { return (3, 4, 5); }
}
