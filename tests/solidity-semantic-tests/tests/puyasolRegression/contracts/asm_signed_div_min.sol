// SPDX-License-Identifier: MIT
pragma solidity >=0.8.4;
// CUSTOM regression fixture (NOT vendored). Guards asm sdiv/smod at int256.min.
// The signed asm div/mod lowering takes abs of both operands, divides unsigned,
// then re-applies the sign via negate256(). negate256(0) computed (2^256-1)+1 =
// 2^256, out of range → REVERT, whenever the result is a NEGATIVE ZERO — e.g.
// sdiv(x, int256.min) (quotient 0, opposite signs) or smod(int256.min, y)
// (remainder 0, negative dividend). EVM returns 0. Fixed by wrapping negate256
// mod 2^256 (two's complement of 0 is 0). Found fuzzing Solady FixedPointMathLib
// (sMulWad/sDivWad). See realworld-lib-certs.
contract AsmSignedDivMin {
    function sdiv_(int256 a, int256 b) external pure returns (int256 r) { assembly { r := sdiv(a, b) } }
    function smod_(int256 a, int256 b) external pure returns (int256 r) { assembly { r := smod(a, b) } }
}
