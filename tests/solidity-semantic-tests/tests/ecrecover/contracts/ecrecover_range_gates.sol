// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// Guard: every ecrecover lowering yields address(0)/empty output for inputs the
// EVM precompile rejects, instead of panicking ecdsa_pk_recover or accepting
// what EVM would not (review item C18).
//
// The known-good vector is upstream's ecrecover.sol fixture:
//   h = 0x18c5...3d1c, v = 28, r = 0x73b1...a75f, s = 0xeeb9...4549
//   -> 0xa94f5374fce5edbc8e2a8697c15331677e6ebf0b
contract C {
    bytes32 constant H = 0x18c547e4f7b0f325ad1e56f57e26c745b09a3e503d86e00e5255ff7f715d3d1c;
    bytes32 constant R = 0x73b1693892219d736caba55bdb67216e485557ea6b6af75f37096c9aa6a5a75f;
    bytes32 constant S = 0xeeb940b1d03b21e36b0e47e79769f095fe2ab855bd91e3a38756b7d75a9c4549;
    uint256 constant N = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141;

    // Builtin: r/s out of range must be address(0), not a panic.
    function builtinBadR() public returns (address) { return ecrecover(H, 28, 0, S); }
    function builtinBadS() public returns (address) { return ecrecover(H, 28, R, bytes32(N)); }
    function builtinGood() public returns (address) { return ecrecover(H, 28, R, S); }

    // Raw precompile shape through the Solidity-level staticcall: the v WORD is
    // validated in full — dirty high bytes with a plausible low byte are
    // invalid on EVM (empty returndata) and must not recover an address.
    function rawCall(uint256 vWord, bytes32 r, bytes32 s)
        internal returns (bool ok, bytes memory ret)
    {
        (ok, ret) = address(0x1).staticcall(abi.encode(H, vWord, r, s));
    }
    function staticcallDirtyV() public returns (uint256) {
        (, bytes memory ret) = rawCall((1 << 200) | 28, R, S);
        // EVM: empty (or zero-address) output for an invalid v word.
        return ret.length == 0 ? 0 : uint256(bytes32(ret));
    }
    function staticcallBadR() public returns (uint256) {
        (, bytes memory ret) = rawCall(28, bytes32(0), S);
        return ret.length == 0 ? 0 : uint256(bytes32(ret));
    }
    function staticcallGood() public returns (address) {
        (, bytes memory ret) = rawCall(28, R, S);
        return address(uint160(uint256(bytes32(ret))));
    }
}
