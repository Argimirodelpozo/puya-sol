// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// CUSTOM puya-sol test contract (NOT vendored from the upstream Solidity
// semantic suite) — guards four bugs found in the sol-eb/ builder audit:
//   A: signed compound `-=` reverted (it used the unsigned-underflow-checked
//      subtract; `int x = 1; x -= 2` is a valid -1, not an underflow).
//   B: signed `>>` (SAR) was lowered as a LOGICAL shift (zero-fill), wrong for
//      negative values.
//   C: a `bool` in abi.encode occupied 8 bytes instead of the 32-byte ABI word,
//      misaligning every following argument.
//   D: enum `==` double-evaluated a side-effecting operand.
contract C {
    // A — signed compound subtraction (all widths)
    function cSub8(int8 a, int8 b) external pure returns (int8) { a -= b; return a; }
    function cSub128(int128 a, int128 b) external pure returns (int128) { a -= b; return a; }
    function cSub256(int256 a, int256 b) external pure returns (int256) { a -= b; return a; }
    // B — signed arithmetic shift right
    function sar(int256 a, uint256 n) external pure returns (int256) { return a >> n; }
    // C — bool ABI alignment
    function encBool() external pure returns (bytes memory) {
        return abi.encode(uint256(0xAA), true, uint256(0xBB));
    }
    // D — enum compare single-evaluation
    enum E { A, B, Cc }
    uint256 cnt;
    function bump() internal returns (E) { cnt += 1; return E.B; }
    function enumOneEval() external returns (bool) { bool r = (bump() == E.B); return r && cnt == 1; }
}
