// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

library WideReceiver {
    function widen(int128 self) internal pure returns (int256) { return self; }
}

contract SolTypesConversions {
    using WideReceiver for int8;
    struct Wide { int128 number; bytes5 data; bool flag; string text; }
    struct Empty { bool a; bool b; string text; uint16[] values; }
    struct Node { uint16 value; Node[] children; }
    int128[] private signedValues;
    Wide[] private structures;
    uint16[5] private fixedValues;
    uint64 private calls;

    function construct(int8 value, bytes3 data) external pure returns (int128, bytes5, bool, string memory) {
        Wide memory item = Wide(value, data, true, "hello");
        return (item.number, item.data, item.flag, item.text);
    }

    function pushValue(int8 value) external returns (int256, int256, uint256) {
        delete signedValues;
        signedValues.push(value);
        signedValues.push() = value;
        return (signedValues[0], signedValues[1], signedValues.length);
    }

    function pushStruct(int8 value) external returns (int256, bytes5, string memory) {
        delete structures;
        structures.push(Wide(value, hex"010203", true, "push"));
        return (structures[0].number, structures[0].data, structures[0].text);
    }

    function receiver(int8 value) external pure returns (int256) { return value.widen(); }

    function literalTuple() external returns (bytes5, bytes4, bytes3, bytes4, int128, uint64) {
        calls = 0;
        return ("hi", hex"00ff", "", "a\x00\xff", -128, ++calls);
    }

    function projection(uint16 value, uint8 count) external pure returns (uint16, uint256, uint16) {
        Node[] memory children = new Node[](count);
        children[0].value = value;
        Node memory parent = Node(9, children);
        return (parent.value, parent.children.length, parent.children[0].value);
    }

    function fixedCopy() external returns (uint256, uint256, uint256, uint256) {
        uint8[3] memory source = [uint8(1), 7, 255];
        fixedValues[4] = 99;
        fixedValues = source;
        return (fixedValues[0], fixedValues[1], fixedValues[2], fixedValues[4]);
    }

    function defaults() external pure returns (bool, bool, uint256, uint256, uint256, bool) {
        Empty memory item;
        string[2] memory texts;
        bool[9] memory flags;
        return (item.a, item.b, bytes(item.text).length, item.values.length, bytes(texts[1]).length, flags[8]);
    }

    function pair(int8 value) private returns (int8, int8) { ++calls; return (value, -2); }
    function forward(int8 value) private returns (int128, int256) { return ((pair(value))); }
    function forwarded(int8 value, bool choose) external returns (int128, int256, uint64) {
        calls = 0;
        (int128 a, int256 b) = choose ? forward(value) : forward(7);
        return (a, b, calls);
    }
}
