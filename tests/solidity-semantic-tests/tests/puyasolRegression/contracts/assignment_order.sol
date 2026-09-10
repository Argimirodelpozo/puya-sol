// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract AssignmentOrder {
    uint64 private trace;
    uint64 private cursor;
    uint64[2] private stored;

    function left(uint64[] memory a) internal returns (uint64) {
        trace = trace * 10 + 1;
        a[0] = 9;
        return 1;
    }

    function right(uint64[] memory a) internal returns (uint64) {
        trace = trace * 10 + 2;
        a[1] = 20;
        return 4;
    }

    function memoryOrder(bool compound, bool rhsCall) external returns (uint64, uint64, uint64) {
        trace = 0;
        uint64[] memory a = new uint64[](2);
        a[0] = 3;
        a[1] = 7;
        if (rhsCall) {
            if (compound) a[left(a)] += right(a);
            else a[left(a)] = right(a);
        } else {
            if (compound) a[left(a)] += a[0];
            else a[left(a)] = a[0];
        }
        return (a[0], a[1], trace);
    }

    function changeCursor() internal returns (uint64) { cursor = 1; return 5; }

    function storageOrder(bool compound) external returns (uint64, uint64, uint64) {
        cursor = 0;
        stored[0] = 3;
        stored[1] = 7;
        if (compound) stored[cursor] += changeCursor();
        else stored[cursor] = changeCursor();
        return (stored[0], stored[1], cursor);
    }

    function localIndex(bool compound) external pure returns (uint64, uint64) {
        uint64[2] memory a;
        a[1] = 10;
        uint64 i = 1;
        if (compound) a[i++] += i;
        else a[i++] = i;
        return (a[1], i);
    }
}
