// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// Regression: a pure-but-REVERTING div/mod under a folding consumer lost its revert. Two layers:
// (1) frontend: buildBigUIntShift's >=256 saturation conditional evaluated the shifted VALUE
//     lazily, so `(d/d) << n` with a RUNTIME n >= 256 skipped the division at runtime
//     (fixed: value pinned eagerly via comma-expr, like the SAR helper);
// (2) backend: with a LITERAL amount the guard constant-folds, the pinned var goes unused, and
//     puya's DCE dropped the division — "/", "%", "b/", "b%" were in SIDE_EFFECT_FREE_AVM_OPS.
//     Their zero-divisor panic IS the EVM revert (no explicit assert exists for it, unlike
//     checked +/-/*), so they are now excluded from the droppable set (puya fork).
// Shapes from the fuzzer (f7/f18) + the older backend-dce-drops-reverting-subexpr memory class.
contract C {
    function divdivShl(uint256 d) external pure returns (uint256) { return (d / d) << 256; }
    function modShl(uint256 a, uint256 b) external pure returns (uint256) { return (a % b) << 256; }
    function modShrRt(uint256 a, uint256 b, uint256 n) external pure returns (uint256) {
        unchecked { return (a % b) >> n; }
    }
    function ternFold(uint256 a, uint256 c) external pure returns (uint256) { return (a % c <= 5) ? c : c; }
    function expZero(uint256 a, uint256 b) external pure returns (uint256) { unchecked { return (a % b) ** 0; } }
    function mulZero(uint256 a, uint256 b) external pure returns (uint256) { unchecked { return (a / b) * 0; } }
}
