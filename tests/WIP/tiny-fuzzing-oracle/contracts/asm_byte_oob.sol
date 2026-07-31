// SPDX-License-Identifier: MIT
pragma solidity >=0.8.4;
// Isolate asm byte(n,x) with out-of-range / huge n (EVM: n>=32 → 0).
contract AsmByteOOB {
    function tbyte(uint256 n, uint256 x) external pure returns (uint256 r) {
        assembly { r := byte(n, x) }
    }
}
