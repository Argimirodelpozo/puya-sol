pragma solidity ^0.8.28;

contract MemoryLayoutChecks {
    struct Child { uint64[2] values; bytes data; }
    struct Parent { Child[2] children; uint256 tag; }
    function nested() external pure returns (bytes memory) {
        uint64[2][5] memory a;
        a[0][0] = 11; a[4][1] = 99;
        assembly { mstore(mload(a), 17) }
        return abi.encode(a);
    }
    function distinct() external pure returns (bool ok) {
        uint64[5] memory a;
        uint64[5] memory b;
        a[4] = 11; b[0] = 22;
        assembly { ok := eq(sub(b, a), 160) }
        return ok && a[4] == 11 && b[0] == 22;
    }
    function structDefault() external pure returns (bool) {
        Parent memory a;
        a.children[0].values[0] = 11;
        a.children[1].values[1] = 99;
        uint256 tag;
        assembly { tag := mload(add(a, 32)) }
        return tag == 0 && a.children[0].values[1] == 0 && a.children[1].values[0] == 0
            && a.children[0].values[0] == 11 && a.children[1].values[1] == 99
            && a.children[0].data.length == 0 && a.children[1].data.length == 0;
    }
    function freshZero() external pure returns (bool) {
        assembly { mstore(mload(64), 99) }
        uint256[5] memory a;
        uint256 value;
        assembly { value := mload(a) }
        return value == 0 && a[0] == 0;
    }
    function byteAssignments() external pure returns (bytes memory) {
        bytes[5] memory a;
        a[0] = hex"1122";
        a[4] = hex"334455";
        assembly { mstore8(add(mload(a), 32), 0xaa) }
        return abi.encode(a);
    }
    function aliases() external pure returns (bool) {
        bytes[2] memory a;
        a[0] = hex"1122";
        a[1] = a[0];
        bytes memory old = a[0];
        a[0][0] = 0xaa;
        a[0] = hex"334455";
        assembly { mstore8(add(mload(add(a, 32)), 33), 0xbb) }
        return old[0] == 0xaa && old[1] == 0xbb && a[1][0] == 0xaa && a[0][0] == 0x33;
    }
    function tupleAliases() external pure returns (bool) {
        bytes[2] memory a;
        a[0] = hex"1122"; a[1] = hex"3344";
        bytes memory first = a[0];
        (a[0], a[1]) = (a[1], a[0]);
        a[1][0] = 0xaa;
        assembly { mstore8(add(mload(a), 33), 0xbb) }
        return first[0] == 0xaa && a[0][0] == 0x33 && a[0][1] == 0xbb;
    }
    function zeroPointer() external pure returns (uint256 a, uint256 b, uint256 c) {
        bytes memory data;
        bytes[2] memory items;
        assembly { a := data b := mload(items) c := mload(add(items, 32)) }
    }
    function deletedReferences() external pure returns (bool) {
        bytes[2] memory a;
        a[0] = hex"1122";
        bytes memory old = a[0];
        delete a[0];
        uint256 empty;
        assembly { empty := mload(a) }
        return old[0] == 0x11 && a[0].length == 0 && empty == 96;
    }
    function highLevelAliases() external pure returns (bool) {
        bytes[2] memory a;
        bytes memory b = hex"1122";
        Child memory c;
        a[0] = b;
        c.data = a[0];
        b[0] = 0xaa;
        a[1] = c.data;
        delete a[0];
        return b[0] == 0xaa && c.data[0] == 0xaa && a[1][0] == 0xaa && a[0].length == 0;
    }
    function word() internal pure returns (uint256) { return 17; }
    function freshFunctionArrays() external pure returns (bool) {
        function() internal pure returns (uint256)[][] memory a =
            new function() internal pure returns (uint256)[][](1);
        a[0] = new function() internal pure returns (uint256)[](1);
        a[0][0] = word;
        return a[0][0]() == 17;
    }
}

contract NarrowYulChecks {
    function uintWord(uint256 n) external pure returns (uint256 raw, uint8 clean) {
        uint8 value;
        assembly { value := n raw := value }
        clean = value;
    }
    function boolWord(uint256 n) external pure returns (uint256 raw, bool clean) {
        bool value;
        assembly { value := n raw := value }
        clean = value;
    }
    function bytesWord(uint256 n) external pure returns (uint256 raw, bytes1 clean) {
        bytes1 value;
        assembly { value := n raw := value }
        clean = value;
    }
}
