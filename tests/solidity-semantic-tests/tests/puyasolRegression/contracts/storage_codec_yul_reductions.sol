pragma solidity ^0.8.28;

contract StorageCodecReductions {
    struct Packed {
        int8 a;
        int40 b;
        bool c;
        bytes4 d;
        int128 e;
    }
    Packed data;
    function packed(int8 a, int40 b, int128 e) external returns (bool) {
        data = Packed(a, b, true, hex"deadbeef", e);
        Packed memory copy = data;
        return copy.a == a && copy.b == b && copy.c && copy.d == hex"deadbeef" && copy.e == e;
    }
}

contract MemoryCodecReductions {
    function ints() external pure returns (bytes memory) {
        int8[9] memory a;
        assembly { mstore(a, not(0)) mstore(add(a, 256), 127) }
        return abi.encode(a);
    }
    function bools() external pure returns (bytes memory) {
        bool[9] memory a;
        assembly { mstore(a, 1) mstore(add(a, 256), 1) }
        return abi.encode(a);
    }
    function fixedBytes() external pure returns (bytes memory) {
        bytes4[5] memory a;
        assembly { mstore(a, shl(224, 0x11223344)) mstore(add(a, 128), shl(224, 0xaabbccdd)) }
        return abi.encode(a);
    }
    function nested() external pure returns (bytes memory) {
        uint64[2][5] memory a = [[uint64(11), 0], [uint64(0), 0],
            [uint64(0), 0], [uint64(0), 0], [uint64(0), 99]];
        assembly { mstore(mload(a), 17) }
        return abi.encode(a);
    }
    function dynamicElements() external pure returns (bytes memory) {
        bytes[5] memory a = [bytes(hex"1122"), bytes(""), bytes(""), bytes(""), bytes(hex"334455")];
        assembly { mstore8(add(mload(a), 32), 0xaa) }
        return abi.encode(a);
    }
    function largeFixed() external pure returns (bytes32) {
        uint256[65] memory a;
        assembly { mstore(a, 11) mstore(add(a, 2048), 99) }
        return keccak256(abi.encode(a));
    }
    function fullFixed() external pure returns (bytes32) {
        uint256[128] memory a;
        assembly { mstore(a, 11) mstore(add(a, 4064), 99) }
        return keccak256(abi.encode(a));
    }
    function makeBytes(uint256 n) internal pure returns (bytes memory b) {
        b = new bytes(n);
        if (n != 0) b[n - 1] = hex"61";
    }
    function bytesCopy(uint256 n) external pure returns (uint256 size, uint256 last, uint256 padding) {
        bytes memory b = makeBytes(n);
        assembly {
            size := mload(b)
            if n { last := byte(0, mload(add(add(b, 32), sub(n, 1)))) }
            padding := mload(add(add(b, 32), n))
        }
    }
}

contract YulReductions {
    struct S { uint256 a; uint256 b; }
    function boolSwitch(uint256 x) external pure returns (uint256 r) {
        assembly { switch lt(x, 3) case 2 { r := 99 } default { r := 7 } }
    }
    function stringSwitch(uint256 x) external pure returns (uint256 r) {
        assembly { switch shl(248, x) case "a" { r := 11 } default { r := 22 } }
    }
    function overlap() external pure returns (bytes32 h) {
        assembly { mstore(0, 1) mstore(1, 0) h := keccak256(0, 32) }
    }
    function calldataCoincidence(S calldata s) external pure returns (bytes32 h) {
        assembly { mstore(4, 0) mstore(36, 0) h := keccak256(4, 64) }
    }
    function dynamicCoincidence(bytes calldata s) external pure returns (bytes32 h) {
        assembly { mstore(36, 0) mstore(68, 0) h := keccak256(36, add(s.length, 32)) }
    }
    function hugeHash() external pure returns (bytes32 h) {
        assembly { h := keccak256(0, 0x100000020) }
    }
    function hashRange(uint256 offset, uint256 size) external pure returns (bytes32 h) {
        assembly { h := keccak256(offset, size) }
    }
    function poisonedAlignment() external pure returns (uint256 r) {
        assembly {
            mstore8(95, 255)
            let p := mload(0x40)
            mstore(add(p, 0xf00), 1)
            r := mload(add(p, 0xf00))
        }
    }
    function previousBlockAlignment() external pure returns (uint256 r) {
        assembly { mstore(64, 255) }
        assembly {
            let p := mload(0x40)
            mstore(add(p, 0xf00), 1)
            r := mload(add(p, 0xf00))
        }
    }
    function revertRange(uint256 size) external pure {
        assembly {
            mstore(4090, 0x1122334455667788)
            revert(4090, size)
        }
    }
    function results(uint256 x) external pure returns (uint256 r, uint256 s, uint256 count) {
        assembly {
            function pair(v) -> a, b {
                mstore(0, add(mload(0), 1))
                a := add(v, 1)
                b := add(v, 2)
            }
            mstore(0, 0)
            let a, b := pair(x)
            r, s := pair(add(a, b))
            count := mload(0)
        }
    }
}
