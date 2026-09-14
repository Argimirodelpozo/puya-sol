// SPDX-License-Identifier: UNLICENSED
pragma solidity ^0.8.26;

contract AuditCallTarget {
    function sum(uint256[2] calldata values) external pure returns (uint256) {
        return values[0] + values[1];
    }
}

contract AuditCalls {
    function sum(uint256[2] calldata values) external pure returns (uint256) {
        return values[0] + values[1];
    }
    function encodedArray() external pure returns (bytes memory) {
        return abi.encodeCall(AuditCallTarget.sum, [uint256(3), uint256(4)]);
    }
    function selfArray() external returns (uint256) {
        (bool ok, bytes memory data) = address(this).call(
            abi.encodeCall(AuditCallTarget.sum, [uint256(3), uint256(4)]));
        require(ok);
        return abi.decode(data, (uint256));
    }
    function forwardedArray(address target) external returns (uint256) {
        (bool ok, bytes memory data) = target.call(
            abi.encodeCall(AuditCallTarget.sum, [uint256(3), uint256(4)]));
        require(ok);
        return abi.decode(data, (uint256));
    }
}

contract AuditSignedKeys {
    mapping(int72 => uint256) public small;
    mapping(int128 => uint256) public medium;
    mapping(int248 => uint256) public large;
    mapping(int256 => uint256) public word;
    mapping(int128 => mapping(int72 => uint256)) public nested;
    function put(int72 a, int128 b, int248 c, int256 d, uint256 value) external {
        small[a] = value;
        medium[b] = value;
        large[c] = value;
        word[d] = value;
        nested[b][a] = value;
    }
    function explicitGet(int128 key) external view returns (uint256) { return medium[key]; }
}

contract AuditHashRanges {
    function dynamicHash(uint256 offset, uint256 length) external pure returns (bytes32 result) {
        assembly { mstore(4080, 0x1234) result := keccak256(offset, length) }
    }
    function fixedHash(uint256 offset) external pure returns (bytes32 result) {
        assembly { mstore(4080, 0x1234) result := keccak256(offset, 32) }
    }
    function emptyHash(uint256 offset) external pure returns (bytes32 result) {
        assembly { result := keccak256(offset, 0) }
    }
    function tailHash(uint256 length) external pure returns (bytes32 result) {
        assembly { mstore8(20479, 0xab) result := keccak256(20479, length) }
    }
    function fixedTailHash() external pure returns (bytes32 result) {
        assembly { mstore8(20479, 0xab) result := keccak256(20479, 1) }
    }
}

library AuditCallerLibrary {
    function low() internal view returns (address value) { assembly { value := caller() } }
}

contract AuditCaller {
    function high() external view returns (address) { return msg.sender; }
    function low() external view returns (address value) { assembly { value := caller() } }
    function libraryLow() external view returns (address) { return AuditCallerLibrary.low(); }
}
