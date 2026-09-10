// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract StatementDeclarations {
    struct Pair { uint256 x; uint256 y; }
    Pair private first;
    Pair private second;
    uint256[2] private data;
    uint64 private calls;

    function signedValues(int8 x) external pure returns (int128, int128, int128) {
        int128 a = x;
        (int128 b, , int128 c) = (x, uint64(99), x);
        return (a, b, c);
    }

    function pairs() internal returns (Pair storage, int8, Pair storage) {
        ++calls;
        return (first, -7, second);
    }

    function scalarCopy() external returns (uint256, uint256) {
        first.x = 5;
        Pair memory a = first;
        a.x = 13;
        return (a.x, first.x);
    }

    function tupleCopy() external returns (uint256, int128, uint256, uint256, uint256, uint64) {
        first.x = 5;
        second.y = 7;
        calls = 0;
        (Pair memory a, int128 n, Pair memory b) = pairs();
        a.x = 13;
        b.y = 19;
        return (a.x, n, b.y, first.x, second.y, calls);
    }

    function tupleReferences() external returns (uint256, uint256, uint64) {
        calls = 0;
        (Pair storage a, , Pair storage b) = pairs();
        a.x = 23;
        b.y = 29;
        return (first.x, second.y, calls);
    }

    function arrayCopies() external returns (uint256, uint256, uint256, uint256) {
        data[0] = 3;
        data[1] = 5;
        uint256[2] memory a = data;
        (uint256[2] memory b, uint256 n) = (data, uint256(11));
        a[0] = 7;
        b[1] = 9;
        return (a[0], b[1], data[0] + data[1], n);
    }

    function memoryTupleAlias() external pure returns (uint256, uint256, uint256) {
        Pair memory a = Pair(3, 5);
        (Pair memory b, uint256 n) = (a, uint256(11));
        b.x = 9;
        return (a.x, b.x, n);
    }
}
