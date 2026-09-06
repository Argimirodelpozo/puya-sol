// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// Solidity memory parameters alias the argument's object: writes through the
// pointer inside a modifier are visible to the wrapped function, but REBINDING
// the parameter (`c = Cell(5)`) only moves the modifier's own pointer. The
// modifier chain aliases the AWST variable for member writes and binds by value
// when the body rebinds the parameter.
contract ModifierMemoryRebind {
    struct Cell { uint256 value; }
    uint256 public seen;

    modifier rebind(Cell memory c) { c = Cell(5); _; }
    modifier mutate(Cell memory c) { c.value += 10; _; seen = c.value; }
    // Writes through, THEN rebinds at top level: the write reaches the caller's
    // object, the rebind stays local (a fresh local from that statement on).
    modifier both(Cell memory c) { c.value += 1; c = Cell(5); require(c.value == 5); _; }
    // Rebind inside a branch: bound by value (documented residual).
    modifier nested(Cell memory c) { if (c.value > 0) { c = Cell(5); } _; }

    function g(Cell memory c) internal rebind(c) returns (uint256) { return c.value; }
    function h(Cell memory c) internal mutate(c) returns (uint256) { return c.value; }
    function k(Cell memory c) internal both(c) returns (uint256) { return c.value; }
    function n(Cell memory c) internal nested(c) returns (uint256) { return c.value; }

    function callG() external returns (uint256) { Cell memory x = Cell(1); return g(x); }
    function callK() external returns (uint256) {
        Cell memory x = Cell(2001);
        uint256 r = k(x);
        return r * 10000 + x.value;
    }
    function callN() external returns (uint256) { Cell memory x = Cell(1); return n(x); }
    function callH() external returns (uint256) {
        Cell memory x = Cell(1);
        uint256 r = h(x);
        return r * 1000 + x.value;
    }
}
