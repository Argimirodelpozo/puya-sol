// SPDX-License-Identifier: MIT
pragma solidity >=0.8.4;
// CUSTOM regression fixture (NOT vendored). Guards asm byte(n,x) with an
// out-of-range index. EVM's byte opcode returns 0 for any n >= 32. The handler
// range-checked the btoi-TRUNCATED index, so a huge n (>= 2^64) truncated to a
// small in-range index (e.g. 2^128+5 -> 5) and wrongly extracted a byte instead
// of returning 0. Fixed by range-checking the ORIGINAL biguint index. Found
// fuzzing Solady DateTimeLib.daysInMonth (byte(2^128+5, ...) -> 31 not 0).
contract AsmByteOOB {
    function tbyte(uint256 n, uint256 x) external pure returns (uint256 r) {
        assembly { r := byte(n, x) }
    }
}
