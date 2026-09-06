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

    function g(Cell memory c) internal rebind(c) returns (uint256) { return c.value; }
    function h(Cell memory c) internal mutate(c) returns (uint256) { return c.value; }

    function callG() external returns (uint256) { Cell memory x = Cell(1); return g(x); }
    function callH() external returns (uint256) {
        Cell memory x = Cell(1);
        uint256 r = h(x);
        return r * 1000 + x.value;
    }
}
