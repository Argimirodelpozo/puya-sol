// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract MetadataTarget {
    function ping() external pure returns (uint256) { return 7; }
}

contract MetadataHash {
    bool public constructorHash;

    constructor() {
        constructorHash = address(this).codehash == keccak256("");
    }

    function hashes() external view returns (bytes32, bytes32, bytes32) {
        return (address(7 - 7).codehash, address(1 + 1).codehash, address(this).codehash);
    }
}

contract AddressMetadata {
    uint256 public touches;
    bool public constructorChecks;
    uint256[] private initialised;

    constructor(address other) {
        // Defer to __postInit so the harness can discover foreign app resources.
        initialised.push(1);
        bytes memory otherCode = touch(other).code;
        bytes memory selfCode = touch(address(this)).code;
        constructorChecks = otherCode.length > 0 && selfCode.length == 0
            && touch(other).code.length > 0 && touch(address(this)).code.length == 0
            && address(this).code.length == 0;
    }

    function touch(address target) internal returns (address) {
        ++touches;
        return target;
    }

    function read(address target) external returns (bytes memory, uint256, uint256) {
        touches = 0;
        bytes memory code = touch(target).code;
        uint256 size = touch(target).code.length;
        return (code, size, touches);
    }

    function selfSize() external view returns (uint256, uint256) {
        address self = address(this);
        return (self.code.length, address(this).code.length);
    }

    function gated(bool enabled, address target) external returns (uint256, uint256) {
        touches = 0;
        uint256 size = enabled ? touch(target).code.length : 9;
        return (size, touches);
    }

    struct Payload { bytes code; }
    function fieldLength(bytes memory data) external pure returns (uint256) {
        Payload memory payload = Payload(data);
        return payload.code.length;
    }
}
