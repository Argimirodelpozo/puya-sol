// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract InlineWireChild {
    uint16 public small;
    int24 public negative;
    uint128 public wide;
    bool public flag;
    bytes4 public tag;

    constructor(uint16 a, int24 b, uint128 c, bool d, bytes4 e) payable {
        small = a; negative = b; wide = c; flag = d; tag = e;
    }
}

contract DeferredWireChild {
    uint16 public small;
    int128 public negative;
    uint128 public wide;
    bool public flag;
    bytes4 public tag;
    bytes private payload;
    string private text;
    uint256[] private marker;

    constructor(uint16 a, int128 b, uint128 c, bool d, bytes4 e, bytes memory p, string memory t) payable {
        small = a; negative = b; wide = c; flag = d; tag = e;
        payload = p; text = t; marker.push(1);
    }
    function payloadMatches() external view returns (bool) {
        return keccak256(payload) == keccak256(hex"deadbeef") && keccak256(bytes(text)) == keccak256("text");
    }
}

contract ChildWireFactory {
    uint256 private trace;
    function markValue() internal returns (uint256) { trace = trace * 10 + 1; return 0; }
    function markArg() internal returns (uint16) { trace = trace * 10 + 2; return uint16(trace); }

    function inlineValues() external returns (uint256, uint256, bool, bytes4) {
        trace = 0;
        InlineWireChild child = new InlineWireChild{value: markValue()}(markArg(), -7, 2**100, true, hex"12345678");
        return (trace * 100 + child.small(), child.wide(), child.flag() && child.negative() == -7, child.tag());
    }
    function deferredValues() external returns (uint256, uint256, bool, bytes4) {
        trace = 0;
        DeferredWireChild child = new DeferredWireChild{value: markValue()}(
            markArg(), -7, 2**100, true, hex"12345678", hex"deadbeef", "text");
        return (trace * 100 + child.small(), child.wide(),
            child.flag() && child.negative() == -7 && child.payloadMatches(), child.tag());
    }
}
