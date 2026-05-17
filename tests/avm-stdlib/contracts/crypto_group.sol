==== Source: AVM.sol ====
// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

library Crypto {
    function sha512_256(bytes memory data) internal pure returns (bytes32) { data; revert(); }
    function sha3_256(bytes memory data) internal pure returns (bytes32) { data; revert(); }
    function ed25519Verify(bytes memory d, bytes memory s, bytes memory p) internal pure returns (bool) { d;s;p; revert(); }
    function falconVerify(bytes memory d, bytes memory s, bytes memory p) internal pure returns (bool) { d;s;p; revert(); }
    function vrfVerify(bytes memory m, bytes memory p, bytes memory k) internal pure returns (bytes memory, bool) { m;p;k; revert(); }
}

library Group {
    function txnSender(uint64 idx) internal view returns (address) { idx; revert(); }
    function txnAmount(uint64 idx) internal view returns (uint64) { idx; revert(); }
    function txnReceiver(uint64 idx) internal view returns (address) { idx; revert(); }
    function txnType(uint64 idx) internal view returns (uint64) { idx; revert(); }
    function txnFee(uint64 idx) internal view returns (uint64) { idx; revert(); }
}

==== Source: contract.sol ====
import {Crypto, Group} from "AVM.sol";

contract C {
    function sha512(bytes memory data) public pure returns (bytes32) {
        return Crypto.sha512_256(data);
    }

    function sha3(bytes memory data) public pure returns (bytes32) {
        return Crypto.sha3_256(data);
    }

    function ed25519(bytes memory d, bytes memory s, bytes memory p) public pure returns (bool) {
        return Crypto.ed25519Verify(d, s, p);
    }

    function falcon(bytes memory d, bytes memory s, bytes memory p) public pure returns (bool) {
        return Crypto.falconVerify(d, s, p);
    }

    function gtxnSender(uint64 i) public view returns (address) {
        return Group.txnSender(i);
    }

    function gtxnAmount(uint64 i) public view returns (uint64) {
        return Group.txnAmount(i);
    }

    function gtxnReceiver(uint64 i) public view returns (address) {
        return Group.txnReceiver(i);
    }

    function gtxnType(uint64 i) public view returns (uint64) {
        return Group.txnType(i);
    }

    function gtxnFee(uint64 i) public view returns (uint64) {
        return Group.txnFee(i);
    }
}
