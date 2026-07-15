// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// Guard: EVM ecrecover returns address(0) for invalid inputs (v not in {27,28},
// r/s zero or >= secp256k1 group order N). AVM ecdsa_pk_recover PANICS on such
// inputs; the recover opcode must be gated behind the checkable validity
// conditions and yield zero without running.
contract C {
    bytes32 constant H = 0x0000000000000000000000000000000000000000000000000000000000000001;
    bytes32 constant R1 = 0x0000000000000000000000000000000000000000000000000000000000000001;
    bytes32 constant BIG = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141;

    function zeroAll() public pure returns (address) {
        return ecrecover(0, 0, 0, 0);
    }
    function zeroRS() public pure returns (address) {
        return ecrecover(H, 27, 0, 0);
    }
    function badV(uint8 v) public pure returns (address) {
        return ecrecover(H, v, R1, R1);
    }
    function rTooBig() public pure returns (address) {
        return ecrecover(H, 27, BIG, R1);
    }
    function sTooBig() public pure returns (address) {
        return ecrecover(H, 28, R1, BIG);
    }
}
