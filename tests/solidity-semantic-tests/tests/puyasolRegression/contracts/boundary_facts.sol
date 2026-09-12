// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

import {Scratch as S, Crypto as NativeCrypto} from "libs/AVM.sol";
import * as Native from "libs/AVM.sol";

// These are ordinary user declarations, not the canonical native facade.
library Crypto {
    function sha3_256(bytes memory) internal pure returns (bytes32) { return bytes32(uint256(7)); }
}
library Group {
    function size() internal pure returns (uint64) { return 19; }
}

contract IntrinsicBoundaryFacts {
    using S for uint64;
    uint64 private counter;
    function next() internal returns (uint64) { return ++counter; }
    function hashes(bytes memory data) external pure returns (bytes32, bytes32, bytes32) {
        return (Crypto.sha3_256(data), NativeCrypto.sha3_256(data), Native.Crypto.sha3_256(data));
    }
    function ordinaryGroup() external pure returns (uint64) { return Group.size(); }
    function named() external returns (uint64, uint64, uint64) {
        S.store(3, 0); S.store(4, 0); counter = 0;
        S.store({value: next() + 10, slot: next() + 2});
        return (counter, S.loadSelf(3), S.loadSelf(4));
    }
    function mutateSlot() internal returns (uint64) { S.store(0, 9); return 1; }
    function capture() external returns (uint64) {
        S.store(0, 2); S.store(1, 0);
        S.store({value: S.loadSelf(0), slot: mutateSlot()});
        return S.loadSelf(1);
    }
    function bound() external returns (uint64, uint64, uint64) {
        S.store(1, 0); S.store(2, 0); counter = 0;
        next().store(next());
        return (counter, S.loadSelf(1), S.loadSelf(2));
    }
}

contract PointerBoundaryFacts {
    struct Pointer { function(uint64) external pure returns (uint64) value; }
    function value(uint64 n) external pure returns (uint64) { return n + 7; }
    function referenceWire(address target) external pure returns (bytes memory) {
        return abi.encode(PointerBoundaryFacts(target).value);
    }
    function roundTrip(bytes memory word) external pure returns (bytes memory) {
        Pointer memory pointer = abi.decode(word, (Pointer));
        return abi.encode(pointer);
    }
    function cross(address target, uint64 n) external pure returns (uint64) {
        function(uint64) external pure returns (uint64) pointer = PointerBoundaryFacts(target).value;
        return pointer(n);
    }
    function self(uint64 n) external view returns (uint64) {
        function(uint64) external pure returns (uint64) pointer = this.value;
        return pointer(n);
    }
    function zero(uint64 n) external pure returns (uint64) {
        function(uint64) external pure returns (uint64) pointer;
        return pointer(n);
    }
}
