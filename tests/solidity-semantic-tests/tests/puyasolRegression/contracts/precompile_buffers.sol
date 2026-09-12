// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

contract PrecompileBuffers {
    function identity(uint256 size, uint256 window) external view returns (uint256 n, uint256 copied, uint256 complete) {
        assembly {
            mstore(128, not(0)) mstore(160, 0x1234)
            mstore(256, 0) mstore(288, 0)
            if iszero(staticcall(gas(), 4, 128, size, 256, window)) { revert(0, 0) }
            n := returndatasize() copied := mload(256)
            returndatacopy(384, 0, n) complete := mload(384)
        }
    }
    function sha(uint256 size, uint256 window) external view returns (uint256 n, uint256 copied, uint256 complete) {
        assembly {
            mstore(128, not(0)) mstore(256, not(0)) mstore(288, not(0))
            if iszero(staticcall(gas(), 2, 128, size, 256, window)) { revert(0, 0) }
            n := returndatasize() copied := mload(256)
            returndatacopy(384, 0, n) complete := mload(384)
        }
    }
    function invalidRecovery(uint256 off, uint256 size) external view returns (uint256 n, uint256 untouched) {
        assembly {
            mstore(off, 0) mstore(add(off, 32), 0) mstore(add(off, 64), 0) mstore(add(off, 96), 0)
            mstore(256, 123)
            if iszero(staticcall(gas(), 1, off, size, 256, 32)) { revert(0, 0) }
            n := returndatasize() untouched := mload(256)
        }
    }
    function pairing(uint256 size) external view returns (uint256 n, uint256 result) {
        assembly {
            if iszero(staticcall(gas(), 8, 128, size, 512, 32)) { revert(0, 0) }
            n := returndatasize() result := mload(512)
        }
    }
    function modexp(uint256 base, uint256 exponent, uint256 modulus) external view returns (uint256 n, uint256 result) {
        assembly {
            mstore(128, 32) mstore(160, 32) mstore(192, 32)
            mstore(224, base) mstore(256, exponent) mstore(288, modulus)
            if iszero(staticcall(gas(), 5, 128, 192, 512, 32)) { revert(0, 0) }
            n := returndatasize() result := mload(512)
        }
    }
    function callOrder() external view returns (uint256 trace, uint256 size, uint256 beforeCall) {
        assembly {
            function record(digit, value) -> result {
                mstore(512, add(mul(mload(512), 10), digit)) result := value
            }
            function inner() {
                pop(staticcall(record(1, gas()), 4, record(3, 128), record(4, 32), record(5, 160), record(6, 32)))
            }
            beforeCall := returndatasize()
            inner() trace := mload(512) size := returndatasize()
        }
    }
}
