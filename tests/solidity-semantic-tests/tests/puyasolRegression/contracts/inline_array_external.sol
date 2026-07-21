// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// CUSTOM regression fixture (NOT vendored / not an original Solidity semantic test).
// Guards inline array literals as TYPED external-call args (fable-review-3 M22):
// the old hand-rolled 32-byte-word concat zero-extended narrow elements and
// emitted no ARC4 length header — the callee decoded garbage. Now routed
// through the shared ARC4 encoder (arc4.uint8 elements at the right width).
interface ICallee {
    function sumU8(uint8[2] calldata a) external returns (uint256);
    function sumU256(uint256[2] calldata a) external returns (uint256);
}

contract Callee is ICallee {
    function sumU8(uint8[2] calldata a) external pure returns (uint256) {
        return uint256(a[0]) + uint256(a[1]);
    }
    function sumU256(uint256[2] calldata a) external pure returns (uint256) {
        return a[0] + a[1];
    }
}

contract Caller {
    function callU8(address t) external returns (uint256) {
        return ICallee(t).sumU8([uint8(3), 4]);
    }
    function callU256(address t) external returns (uint256) {
        return ICallee(t).sumU256([uint256(10), 20]);
    }
}
