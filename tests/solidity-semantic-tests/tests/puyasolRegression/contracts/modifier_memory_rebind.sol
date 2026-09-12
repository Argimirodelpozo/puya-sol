// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// Solidity memory parameters alias the argument's object: writes through the
// pointer inside a modifier are visible to the wrapped function, but REBINDING
// the parameter (`c = Cell(5)`) only moves the modifier's own pointer. The
// modifier chain carries the shared object as a scratch-memory pointer and
// gives each modifier parameter its own pointer local, matching solc's model.
contract ModifierMemoryRebind {
    struct Cell { uint256 value; }
    uint256 public seen;
    uint256 public arraySeen;

    modifier rebind(Cell memory c) { c = Cell(5); _; }
    modifier mutate(Cell memory c) { c.value += 10; _; seen = c.value; }
    modifier mutateArray(uint256[] memory a) { a[0] += 10; _; arraySeen = a[0]; }
    // Writes through, THEN rebinds at top level: the write reaches the caller's
    // object, the rebind stays local (a fresh local from that statement on).
    modifier both(Cell memory c) { c.value += 1; c = Cell(5); require(c.value == 5); _; }
    // Every former scanner edge is runtime: branch, loop, tuple, and Yul.
    // The write through c reaches the original object; each rebind below only
    // changes the modifier's pointer.
    modifier nested(Cell memory c, uint256 mode) {
        c.value += 1;
        if (mode == 0) {
            c = Cell(5);
        } else if (mode == 1) {
            for (uint256 i = 0; i < 1; ++i) c = Cell(6);
        } else if (mode == 2) {
            uint256 marker;
            (c, marker) = (Cell(7), uint256(1));
            require(marker == 1);
        } else {
            assembly {
                let fresh := mload(0x40)
                mstore(fresh, 8)
                mstore(0x40, add(fresh, 0x20))
                c := fresh
            }
        }
        _;
    }

    function g(Cell memory c) internal rebind(c) returns (uint256) { return c.value; }
    function bump(Cell memory c) internal pure { c.value += 1; }
    function bumpArray(uint256[] memory a) internal pure { a[0] += 1; }
    function h(Cell memory c) internal mutate(c) returns (uint256) {
        bump(c);
        return c.value;
    }
    function k(Cell memory c) internal both(c) returns (uint256) { return c.value; }
    function arrayBody(uint256[] memory a) internal mutateArray(a) returns (uint256) {
        bumpArray(a);
        return a[0];
    }

    uint256 private freshCount;
    function makeFreshCell() internal returns (Cell memory) { ++freshCount; return Cell(9); }
    function mixedReference(Cell memory a, bool existing)
        internal mutate(existing ? a : makeFreshCell()) returns (uint256)
    { return a.value; }
    function callMixed(bool existing) external returns (uint256, uint256, uint256) {
        freshCount = 0;
        Cell memory a = Cell(1);
        uint256 result = mixedReference(a, existing);
        return (result, seen, freshCount);
    }
    function n(Cell memory a, Cell memory b, uint256 mode)
        internal
        nested(mode < 4 ? a : b, mode % 4)
        returns (uint256, uint256)
    {
        return (a.value, b.value);
    }

    function callG() external returns (uint256) { Cell memory x = Cell(1); return g(x); }
    function callK() external returns (uint256) {
        Cell memory x = Cell(2001);
        uint256 r = k(x);
        return r * 10000 + x.value;
    }
    function callN(uint256 mode) external returns (uint256, uint256, uint256, uint256) {
        Cell memory x = Cell(1);
        Cell memory y = Cell(10);
        (uint256 a, uint256 b) = n(x, y, mode);
        return (a, b, x.value, y.value);
    }
    function callH() external returns (uint256) {
        Cell memory x = Cell(1);
        uint256 r = h(x);
        return r * 1000 + x.value;
    }
    function callArray() external returns (uint256) {
        uint256[] memory x = new uint256[](1);
        x[0] = 1;
        uint256 r = arrayBody(x);
        return r * 1000 + x[0];
    }
}
