// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// CUSTOM regression fixture (NOT vendored / not an original Solidity semantic test).
// Guards operand pinning for unary ops, checked **, and the enum-assign range
// check: checked `-g()` evaluated g THREE times (overflow assert + negation),
// `~g()` twice, unsigned `x ** f()` ran f twice (0**0 case + pow), and an
// enum-typed call RHS `st = e()` ran e twice (range assert + store). Each
// counter must advance exactly once.
contract EvalOnceUnaryPow {
    enum E { A, B, C }

    uint256 public cnt;
    E st;

    function g128() internal returns (int128) {
        cnt++;
        return -5;
    }

    function g64() internal returns (int64) {
        cnt++;
        return -5;
    }

    function fexp() internal returns (uint64) {
        cnt++;
        return 3;
    }

    function e() internal returns (E) {
        cnt++;
        return E.B;
    }

    function negWide() external returns (int128 r, uint256 n) {
        cnt = 0;
        r = -g128();
        n = cnt;
    }

    function negNarrow() external returns (int64 r, uint256 n) {
        cnt = 0;
        r = -g64();
        n = cnt;
    }

    function invWide() external returns (int128 r, uint256 n) {
        cnt = 0;
        r = ~g128();
        n = cnt;
    }

    function powRhs(uint64 x) external returns (uint64 r, uint256 n) {
        cnt = 0;
        r = x ** fexp();
        n = cnt;
    }

    function enumAssign() external returns (uint256 stv, uint256 n) {
        cnt = 0;
        st = e();
        stv = uint256(st);
        n = cnt;
    }
}
