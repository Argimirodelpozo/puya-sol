// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
type U16 is uint16;
type I32 is int32;
type U256 is uint256;
contract C {
    function wrapAddU16(uint16 a, uint16 b) external pure returns (uint16) {
        U16 x = U16.wrap(a); U16 y = U16.wrap(b);
        unchecked { return U16.unwrap(x) + U16.unwrap(y); }
    }
    function roundtripI32(int32 a) external pure returns (int32) {
        return I32.unwrap(I32.wrap(a));
    }
    function u256mul(uint256 a, uint256 b) external pure returns (uint256) {
        U256 x = U256.wrap(a);
        unchecked { return U256.unwrap(x) * b; }
    }
    // UDVT stored and read back
    U16 stored;
    function setStored(uint16 v) external { stored = U16.wrap(v); }
    function getStored() external view returns (uint16) { return U16.unwrap(stored); }
    // signed UDVT negative roundtrip
    function negI32(int32 a) external pure returns (int32) {
        I32 x = I32.wrap(a);
        unchecked { return -I32.unwrap(x); }
    }
}
