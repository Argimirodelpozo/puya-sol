// SPDX-License-Identifier: MIT
pragma solidity >=0.8.4;
// Targeted differential of puya-sol's asm-opcode handlers vs EVM, with extreme
// operands (the seam where negate256(0) and byte-OOB bugs lived). Yul operand
// semantics preserved; exp excluded (runtime exp hard-errors on non-const args).
contract AsmOps {
    function add_(uint256 a, uint256 b) external pure returns (uint256 r) { assembly { r := add(a, b) } }
    function sub_(uint256 a, uint256 b) external pure returns (uint256 r) { assembly { r := sub(a, b) } }
    function mul_(uint256 a, uint256 b) external pure returns (uint256 r) { assembly { r := mul(a, b) } }
    function div_(uint256 a, uint256 b) external pure returns (uint256 r) { assembly { r := div(a, b) } }
    function sdiv_(uint256 a, uint256 b) external pure returns (uint256 r) { assembly { r := sdiv(a, b) } }
    function mod_(uint256 a, uint256 b) external pure returns (uint256 r) { assembly { r := mod(a, b) } }
    function smod_(uint256 a, uint256 b) external pure returns (uint256 r) { assembly { r := smod(a, b) } }
    function addmod_(uint256 a, uint256 b, uint256 m) external pure returns (uint256 r) { assembly { r := addmod(a, b, m) } }
    function mulmod_(uint256 a, uint256 b, uint256 m) external pure returns (uint256 r) { assembly { r := mulmod(a, b, m) } }
    function lt_(uint256 a, uint256 b) external pure returns (uint256 r) { assembly { r := lt(a, b) } }
    function gt_(uint256 a, uint256 b) external pure returns (uint256 r) { assembly { r := gt(a, b) } }
    function slt_(uint256 a, uint256 b) external pure returns (uint256 r) { assembly { r := slt(a, b) } }
    function sgt_(uint256 a, uint256 b) external pure returns (uint256 r) { assembly { r := sgt(a, b) } }
    function eq_(uint256 a, uint256 b) external pure returns (uint256 r) { assembly { r := eq(a, b) } }
    function iszero_(uint256 a) external pure returns (uint256 r) { assembly { r := iszero(a) } }
    function and_(uint256 a, uint256 b) external pure returns (uint256 r) { assembly { r := and(a, b) } }
    function or_(uint256 a, uint256 b) external pure returns (uint256 r) { assembly { r := or(a, b) } }
    function xor_(uint256 a, uint256 b) external pure returns (uint256 r) { assembly { r := xor(a, b) } }
    function not_(uint256 a) external pure returns (uint256 r) { assembly { r := not(a) } }
    function byte_(uint256 n, uint256 x) external pure returns (uint256 r) { assembly { r := byte(n, x) } }
    function shl_(uint256 s, uint256 v) external pure returns (uint256 r) { assembly { r := shl(s, v) } }
    function shr_(uint256 s, uint256 v) external pure returns (uint256 r) { assembly { r := shr(s, v) } }
    function sar_(uint256 s, uint256 v) external pure returns (uint256 r) { assembly { r := sar(s, v) } }
    function signextend_(uint256 i, uint256 x) external pure returns (uint256 r) { assembly { r := signextend(i, x) } }
}
