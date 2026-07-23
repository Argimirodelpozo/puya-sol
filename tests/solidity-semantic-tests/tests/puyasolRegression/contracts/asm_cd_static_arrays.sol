// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// CUSTOM regression fixture (NOT vendored / not an original Solidity semantic test).
// Guards the static-array calldata-layout bugs the fuzz_cd campaign found in the
// item-2 rewrite: bytesN array elements are ONE left-aligned EVM word each (not
// N byte-granular words — bytes4[2] was emitting 8 words and shifting every
// following param), and signed sub-word array elements SIGN-extend in their word
// (int16[2] was zero-padding). Reads go through the constant-offset map path.
contract AsmCdStaticArrays {
    // bytes4[2]: two left-aligned words at 4 and 36; tail param lands at 68.
    function bytesArr(bytes4[2] calldata a, uint256 tail)
        external pure returns (bytes32 e0, bytes32 e1, uint256 t, uint256 cs)
    {
        assembly { e0 := calldataload(4) e1 := calldataload(36)
                   t := calldataload(68) cs := calldatasize() } a; tail;
    }
    // int16[2]: signed elements sign-extend.
    function intArr(int16[2] calldata a, uint256 tail)
        external pure returns (bytes32 e0, bytes32 e1, uint256 t)
    {
        assembly { e0 := calldataload(4) e1 := calldataload(36)
                   t := calldataload(68) } a; tail;
    }
    // uint8[3]: unsigned zero-pad; the following param lands at 100 (3 words).
    function u8Arr(uint8[3] calldata a, uint256 tail)
        external pure returns (bytes32 e0, bytes32 e2, uint256 t)
    {
        assembly { e0 := calldataload(4) e2 := calldataload(68)
                   t := calldataload(100) } a; tail;
    }
}
