// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

struct BoundaryItem { uint64 value; }

function boundaryFree(BoundaryItem storage item, uint64 amount) { item.value += amount; }

function boundaryPair() pure returns (uint64, uint64) { return (4, 5); }
function boundaryMemory(uint64[] memory values) pure returns (uint64, uint64) {
    values[0] += 7;
    return boundaryPair();
}

library BoundaryLibrary {
    function add(BoundaryItem storage item, uint64 amount) internal { boundaryFree(item, amount); }
}

contract CallBoundaryFacts {
    // Literals are not nameable solc types, including when a calldata
    // declaration is reached through a memory-typed external pointer.
    function literal(bytes calldata value) external pure returns (uint256) {
        return value.length;
    }
    function literalCall() external view returns (uint256) {
        return this.literal("abc");
    }
    function literalPointer() external view returns (uint256) {
        function(bytes memory) external pure returns (uint256) pointer = this.literal;
        return pointer("abcd");
    }
    using BoundaryLibrary for BoundaryItem;
    BoundaryItem[] items;
    function memoryReturn() external pure returns (uint64, uint64, uint64) {
        uint64[] memory values = new uint64[](1);
        values[0] = 3;
        (uint64 first, uint64 second) = boundaryMemory(values);
        return (first, second, values[0]);
    }
    function run(bool bound) external returns (uint64, uint64) {
        if (items.length == 0) { items.push(BoundaryItem(0)); items.push(BoundaryItem(0)); }
        if (bound) items[1].add(7);
        else BoundaryLibrary.add(items[1], 7);
        return (items[0].value, items[1].value);
    }
    function sequenced() external returns (uint64, uint64) {
        if (items.length == 0) { items.push(BoundaryItem(0)); items.push(BoundaryItem(0)); }
        uint64 index = 0;
        items[index].add(++index);
        return (items[0].value, items[1].value);
    }
}
