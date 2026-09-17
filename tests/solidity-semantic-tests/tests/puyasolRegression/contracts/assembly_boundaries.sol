// SPDX-License-Identifier: MIT
pragma solidity ^0.8.28;

contract InternalCalldataFrame {
    function run(uint256 x, bytes calldata data) external pure returns (uint256, uint256, uint256, uint256) {
        x = 42;
        return inspect(data[1:]);
    }
    function inspect(bytes calldata data) private pure returns (uint256 original, uint256 offset, uint256 n, uint256 first) {
        assembly { original := calldataload(4) offset := data.offset n := data.length }
        first = uint8(data[0]);
    }
    function throughPublic(uint256 x, bytes calldata data) external pure returns (uint256, uint256) {
        x = 42;
        return publicRead(data);
    }
    function publicRead(bytes calldata data) public pure returns (uint256 original, uint256 offset) {
        assembly { original := calldataload(4) offset := data.offset }
    }
    function indirect(uint256 x, bytes calldata data, bool take) external pure returns (uint256, uint256, uint256, uint256) {
        function(bytes calldata) internal pure returns (uint256, uint256, uint256, uint256) ptr = inspect;
        if (take) ptr = inspectAgain;
        x = 42;
        return ptr(data[1:]);
    }
    function inspectAgain(bytes calldata data) private pure returns (uint256, uint256, uint256, uint256) {
        return inspect(data);
    }
    function mixedPointer(uint256 x, bytes calldata data, bool take) external pure returns (uint256, uint256, uint256, uint256) {
        function(bytes calldata) internal pure returns (uint256, uint256, uint256, uint256) ptr = inspect;
        if (take) ptr = plain;
        x = 42;
        return ptr(data[1:]);
    }
    function plain(bytes calldata data) private pure returns (uint256, uint256, uint256, uint256) {
        return (777, 888, data.length, uint8(data[0]));
    }
}

contract VirtualFrameBase {
    function run(uint256 x) external pure returns (uint256) { x = 42; return read(); }
    function read() internal pure virtual returns (uint256) { return 0; }
}
contract VirtualCalldataFrame is VirtualFrameBase {
    function read() internal pure override returns (uint256 original) {
        assembly { original := calldataload(4) }
    }
}

contract FallbackCalldataFrame {
    fallback(bytes calldata data) external returns (bytes memory) {
        uint256 size; uint256 offset; uint256 n;
        assembly { size := calldatasize() offset := data.offset n := data.length }
        return abi.encode(size, offset, n, uint8(data[0]));
    }
}

contract ModifierCalldataFrame {
    uint256 public observed;
    modifier change(bytes memory data) { data[0] = 0xff; _; }
    function run(bytes memory data) external change(data) returns (uint256 original, uint256 current) {
        assembly { original := byte(0, calldataload(68)) }
        current = uint8(data[0]);
    }
    function noReturn(bytes memory data) external change(data) {
        data[0] = 0xfe;
        uint256 original;
        assembly { original := byte(0, calldataload(68)) }
        observed = original * 256 + uint8(data[0]);
    }
}

contract RawScalars {
    function u8() external pure returns (uint256 r) {
        uint8 x;
        assembly { x := 257 }
        assembly { r := x }
    }
    function u64() external pure returns (uint256 r) {
        uint64 x;
        assembly { x := 18446744073709551617 }
        assembly { r := x }
    }
    function u128() external pure returns (uint256 r) {
        uint128 x;
        assembly { x := not(0) }
        assembly { r := x }
    }
    function boolean() external pure returns (uint256 r) {
        bool x;
        assembly { x := 2 }
        assembly { r := x }
    }
    function fixedByte() external pure returns (uint256 r) {
        bytes1 x;
        assembly { x := not(0) }
        assembly { r := x }
    }
    function signedByte() external pure returns (uint256 r) {
        int8 x;
        assembly { x := 257 }
        assembly { r := x }
    }
    function sameBlock() external pure returns (uint256 r) {
        uint8 x;
        assembly { x := 257 r := x }
    }
    function highLevelRead() external pure returns (uint256 clean, uint256 raw) {
        uint8 x;
        assembly { x := 257 }
        clean = x;
        assembly { raw := x }
    }
    function highLevelWrite() external pure returns (uint256 r) {
        uint8 x;
        assembly { x := 257 }
        x = 7;
        assembly { r := x }
    }
    function branch(bool take) external pure returns (uint256 r) {
        uint8 x = 7;
        if (take) { assembly { x := 257 } }
        assembly { r := x }
    }
    function loop(uint256 count) external pure returns (uint256 r) {
        uint8 x = 7;
        for (uint256 i; i < count; ++i) { assembly { x := 257 } }
        assembly { r := x }
    }
    function parameter(uint8 x) external pure returns (uint256 clean, uint256 raw) {
        assembly { x := add(x, 256) }
        clean = x;
        assembly { raw := x }
    }
    function tupleWrites() external pure returns (uint256 a, uint256 b) {
        uint8 x; uint8 y;
        assembly { x := 257 y := 514 }
        (x, y) = (y, x);
        assembly { a := x b := y }
    }
    function increment() external pure returns (uint256 r) {
        uint8 x;
        assembly { x := 257 }
        ++x;
        assembly { r := x }
    }
    function copies(bool choose) external pure returns (uint256 a, uint256 b, uint256 c) {
        uint8 x;
        assembly { x := 257 }
        uint8 middle = x;
        uint8 y = middle;
        uint8 z;
        z = y = x;
        uint8 selected = choose ? z : y;
        assembly { a := y b := z c := selected }
    }
    function clear() external pure returns (uint256 r) {
        uint8 x;
        assembly { x := 257 }
        delete x;
        assembly { r := x }
    }
    function namedReturn(uint256 n) external pure returns (uint8 x) {
        assembly { x := n }
    }
}

contract Alias {
    function f(bytes calldata a) external pure returns (uint256 delta, uint256 n) {
        bytes calldata b = a;
        assembly { delta := sub(b.offset, a.offset) n := b.length }
    }
}

contract ReferenceVariants {
    function swap(bytes calldata a, bytes calldata b) external pure returns (uint256 x, uint256 y) {
        assembly { x := a.length }
        (a, b) = (b, a);
        assembly { x := a.length y := b.length }
    }
    function declare(bytes calldata a, bytes calldata b) external pure returns (uint256 x, uint256 y) {
        (bytes calldata c, bytes calldata d) = (b, a);
        assembly { x := c.length y := d.length }
    }
    function select(bytes calldata a, bytes calldata b, bool take) external pure returns (uint256 n, uint256 first) {
        bytes calldata c = take ? a : b;
        assembly { n := c.length }
        first = uint8(c[0]);
    }
    function typed(uint256[] calldata a, uint256[] calldata b) external pure returns (uint256 n, uint256 first) {
        assembly { a.offset := b.offset a.length := b.length }
        return (a.length, a[0]);
    }
}

contract SliceAlias {
    function f(bytes calldata a) external pure returns (uint256 delta, uint256 n) {
        bytes calldata b = a[1:];
        assembly { delta := sub(b.offset, a.offset) n := b.length }
    }
}

contract Rebind {
    function f(bytes calldata a, bytes calldata b) external pure returns (uint256 n, uint256 first) {
        assembly { n := a.length }
        a = b;
        assembly { n := a.length first := byte(0, calldataload(a.offset)) }
    }
}

contract ConditionalSeed {
    function f(bytes calldata a, bool take) external pure returns (uint256 n) {
        if (take) { assembly { n := a.length } }
        assembly { n := a.length }
    }
}

contract LoopSeed {
    function f(bytes calldata a, uint256 count) external pure returns (uint256 n) {
        for (uint256 i; i < count; ++i) { assembly { n := a.length } }
        assembly { n := a.length }
    }
}

contract PointerControl {
    function f(bytes calldata a, bytes calldata b) external pure returns (uint256 n, uint256 first) {
        assembly { a.offset := b.offset a.length := b.length }
        assembly { n := a.length first := byte(0, calldataload(a.offset)) }
    }
}

contract CalldataSnapshot {
    function messageAlias(uint256 x) external pure returns (uint256 original, uint256 n) {
        bytes calldata input = msg.data;
        x = 42;
        assembly { original := calldataload(add(input.offset, 4)) n := input.length }
    }
    function fixedOffset(uint256 x) external pure returns (uint256 r) {
        x = 42;
        assembly { r := calldataload(4) }
    }
    function dynamicOffset(uint256 x, uint256 off) external pure returns (uint256 r) {
        x = 42;
        assembly { r := calldataload(off) }
    }
    function acrossBlocks(uint256 x) external pure returns (uint256 r) {
        assembly { x := 42 }
        assembly { r := calldataload(4) }
    }
    function sameBlock(uint256 x) external pure returns (uint256 r) {
        assembly { x := 42 r := calldataload(4) }
    }
    function control(uint256 x) external pure returns (uint256 r) {
        assembly { r := calldataload(4) }
    }
}

contract StaticAlias {
    function f(uint256[2] calldata a) external pure returns (uint256 delta, uint256 first) {
        uint256[2] calldata b = a;
        assembly { delta := sub(b, a) first := calldataload(b) }
    }
}

contract ArrayAlias {
    function f(uint256[] calldata a) external pure returns (uint256 delta, uint256 n) {
        uint256[] calldata b = a;
        assembly { delta := sub(b.offset, a.offset) n := b.length }
    }
}

contract DynamicFixedPointer {
    function f(bytes[2] calldata a) external pure returns (uint256 p) {
        assembly { p := a }
    }
}

contract DynamicStructPointer {
    struct S { bytes data; }
    function f(S calldata a) external pure returns (uint256 p) {
        assembly { p := a }
    }
}

contract LiveArrayLength {
    function f(uint256[] calldata a) external pure returns (uint256 n, uint256 high) {
        assembly { n := a.length }
        high = a.length;
    }
}

contract LiveArrayElement {
    function f(uint256[] calldata a) external pure returns (uint256 n, uint256 first) {
        assembly { n := a.length }
        first = a[0];
    }
}
