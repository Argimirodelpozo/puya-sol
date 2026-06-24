// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// Found by the differential fuzzer: unchecked x++/x-- didn't WRAP at the boundary — AVM reverted
// (native uint64 +/- and biguint b- opcodes revert at over/underflow) where EVM wraps mod 2^N.
contract C {
    function decU8(uint8 a) external pure returns (uint8) { unchecked { uint8 x=a; x--; return x; } }
    function decU128(uint128 a) external pure returns (uint128) { unchecked { uint128 x=a; x--; return x; } }
    function decU256(uint256 a) external pure returns (uint256) { unchecked { uint256 x=a; x--; return x; } }
    function decU64(uint64 a) external pure returns (uint64) { unchecked { uint64 x=a; x--; return x; } }
    function incU256(uint256 a) external pure returns (uint256) { unchecked { uint256 x=a; x++; return x; } }
    function incU64(uint64 a) external pure returns (uint64) { unchecked { uint64 x=a; x++; return x; } }
    function preDecU8(uint8 a) external pure returns (uint8) { unchecked { uint8 x=a; --x; return x; } }
}
