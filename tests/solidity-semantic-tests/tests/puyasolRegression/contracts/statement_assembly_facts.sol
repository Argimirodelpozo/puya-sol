// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract StatementAssemblyFacts {
    uint256 constant WORD = 7 << 80;
    uint256[] private left;
    uint256[] private right;
    struct Container { uint256 head; uint256[] values; }
    Container private container;

    function shadowed() external returns (uint256 a, uint256 b, uint256 c) {
        delete left;
        delete right;
        left.push(3);
        right.push(5);
        right.push(7);
        { uint256[] storage p = left; assembly { a := sload(p.slot) } }
        { uint256[] storage p = right; assembly { b := sload(p.slot) c := WORD } }
    }

    function memberAlias() external returns (uint256 a, uint256 b) {
        delete container.values;
        container.values.push(11);
        uint256[] storage p = container.values;
        assembly { a := sload(p.slot) sstore(p.slot, 0) b := p.offset }
        return (a, container.values.length + b);
    }

    function reassignedSlot() external returns (uint256 a, uint256 b) {
        delete left;
        delete right;
        left.push(3);
        right.push(5);
        right.push(7);
        uint256[] storage p = left;
        assembly { p.slot := right.slot }
        assembly { a := sload(p.slot) b := p.offset }
    }
}
