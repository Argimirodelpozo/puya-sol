pragma solidity ^0.8.28;

contract StorageLengthChecks {
    bool[] flags;
    uint256[] words;
    bool[][] nested;
    function copyFlags(uint256 n) external returns (uint256) {
        assembly { sstore(flags.slot, n) }
        bool[] memory copy = flags;
        return copy.length;
    }
    function copyWords(uint256 n) external returns (uint256) {
        assembly { sstore(words.slot, n) }
        uint256[] memory copy = words;
        return copy.length;
    }
    function copyNested(uint256 n) external returns (uint256) {
        assembly { sstore(nested.slot, n) }
        bool[][] memory copy = nested;
        return copy.length;
    }
    function replaceFlags(uint256 n) external returns (uint256) {
        assembly { sstore(flags.slot, n) }
        flags = new bool[](1);
        return flags.length;
    }
    function replaceWords(uint256 n) external returns (uint256) {
        assembly { sstore(words.slot, n) }
        words = new uint256[](1);
        return words.length;
    }
}

contract StorageTraversalChecks {
    struct Big { uint256[65] values; uint256 tail; }
    uint256[65] values;
    Big big;
    mapping(uint256 => uint16[129]) packedArrays;
    function roundTrip() external returns (bool) {
        values[0] = 11; values[64] = 99;
        uint256[65] memory copy = values;
        bool ok = copy[0] == 11 && copy[32] == 0 && copy[64] == 99;
        delete values;
        ok = ok && values[0] == 0 && values[64] == 0;
        values = copy;
        return ok && values[0] == 11 && values[64] == 99;
    }
    function clearStruct() external returns (bool) {
        big.values[0] = 11; big.values[64] = 99; big.tail = 7;
        delete big;
        return big.values[0] == 0 && big.values[64] == 0 && big.tail == 0;
    }
    function clearPacked() external returns (bool) {
        packedArrays[7][0] = 11; packedArrays[7][64] = 22; packedArrays[7][128] = 33;
        packedArrays[9][128] = 44;
        delete packedArrays[7];
        return packedArrays[7][0] == 0 && packedArrays[7][64] == 0 && packedArrays[7][128] == 0
            && packedArrays[9][128] == 44;
    }
}

contract PackedAddressChecks {
    address value;
    uint96 neighbor;
    struct Reverse { uint96 neighbor; address value; }
    mapping(uint256 => Reverse) entries;
    function clearRaw() external returns (bool) {
        value = msg.sender;
        assembly { sstore(value.slot, 0) }
        return value == address(0);
    }
    function keepNeighbor() external returns (bool) {
        value = msg.sender; neighbor = 3;
        assembly { sstore(value.slot, or(and(sload(value.slot), sub(shl(160, 1), 1)), shl(160, 17))) }
        return value == msg.sender && neighbor == 17;
    }
    function changeRestore() external returns (bool) {
        value = msg.sender;
        uint256 old;
        assembly { old := sload(value.slot) sstore(value.slot, 0) sstore(value.slot, old) }
        return value == address(uint160(old));
    }
    function mapped(bool clear, bool typed) external returns (bool) {
        Reverse storage entry = entries[17];
        entry.neighbor = 3; entry.value = msg.sender;
        if (typed) delete entries[17];
        else if (clear) { assembly { sstore(entry.slot, 0) } }
        else { assembly { sstore(entry.slot, or(and(sload(entry.slot), not(sub(shl(96, 1), 1))), 17)) } }
        return (clear || typed) ? entry.value == address(0) : entry.value == msg.sender && entry.neighbor == 17;
    }
}

contract EnumBoundaryChecks {
    enum E { A, B }
    E stored;
    function encodeEnum(uint256 n, bool packed) external pure returns (bytes memory) {
        E e;
        assembly { e := n }
        return packed ? abi.encodePacked(e) : abi.encode(e);
    }
    function storedEnum(uint256 n) external returns (uint256) {
        assembly { sstore(stored.slot, n) }
        return uint256(stored);
    }
    function explicitEnum(uint256 n) external pure returns (uint256) {
        E e;
        assembly { e := n }
        return uint256(e);
    }
    function rawEnum(uint256 n) external pure returns (uint256 r) {
        E e;
        assembly { e := n r := e }
    }
    function echo(E e) external pure returns (E) { return e; }
    function namedEnum(uint256 n) external pure returns (E e) {
        assembly { e := n }
    }
    function callEnum(uint256 n, bool pointer) external view returns (E) {
        E e;
        assembly { e := n }
        if (pointer) {
            function(E) external view returns (E) target = this.echo;
            return target(e);
        }
        return this.echo(e);
    }
}
