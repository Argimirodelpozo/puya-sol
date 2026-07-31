// SPDX-License-Identifier: MIT
pragma solidity >=0.8.4;
// Deep extreme-operand sweep of the bug-prone signed/mod/shift asm handlers
// (fewer fns → generator spends more of its cap on extreme×extreme combos).
contract AsmOpsRisky {
    function sdiv_(uint256 a, uint256 b) external pure returns (uint256 r) { assembly { r := sdiv(a, b) } }
    function smod_(uint256 a, uint256 b) external pure returns (uint256 r) { assembly { r := smod(a, b) } }
    function div_(uint256 a, uint256 b) external pure returns (uint256 r) { assembly { r := div(a, b) } }
    function mod_(uint256 a, uint256 b) external pure returns (uint256 r) { assembly { r := mod(a, b) } }
    function sar_(uint256 s, uint256 v) external pure returns (uint256 r) { assembly { r := sar(s, v) } }
    function signextend_(uint256 i, uint256 x) external pure returns (uint256 r) { assembly { r := signextend(i, x) } }
    function addmod_(uint256 a, uint256 b, uint256 m) external pure returns (uint256 r) { assembly { r := addmod(a, b, m) } }
    function mulmod_(uint256 a, uint256 b, uint256 m) external pure returns (uint256 r) { assembly { r := mulmod(a, b, m) } }
    function byte_(uint256 n, uint256 x) external pure returns (uint256 r) { assembly { r := byte(n, x) } }
}
