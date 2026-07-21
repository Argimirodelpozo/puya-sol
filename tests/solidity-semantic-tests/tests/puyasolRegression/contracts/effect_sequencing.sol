// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// CUSTOM regression fixture (NOT vendored / not an original Solidity semantic test).
// Guards OperandPlan intra-expression effect sequencing (fable-review-3 H4 + M5).
// ALL expected values verified against real solc 0.8.20 legacy codegen + py-evm
// (tests/WIP/tiny-fuzzing-oracle/evm_oracle.py):
//   - binary ops evaluate the RIGHT operand first;
//   - assignments evaluate the RHS fully first, and the store WINS over a
//     callee write-back;
//   - call args evaluate left-to-right, each arg's write-back visible to the
//     next arg and to the callee;
//   - &&/|| and ternary conditions run first with their write-backs visible
//     to the RHS/branches.
contract EffectSequencing {
    struct S { uint256 f; }
    S s;
    uint256[] arr;

    function bump(S storage p) internal returns (uint256) {
        p.f += 1;
        return 100;
    }

    function two(uint256 a, uint256 b) internal pure returns (uint256) {
        return a * 1000 + b;
    }

    // H4-a: mutation LEFT of a storage read — right evaluates first, reads 5.
    function h4a() external returns (uint256) {
        s.f = 5;
        return bump(s) + s.f; // 100 + 5
    }

    // H4-b: read LEFT of a mutation — mutation (right) runs first, read sees 6.
    function h4b() external returns (uint256) {
        s.f = 5;
        return s.f + bump(s); // 6 + 100
    }

    // H4-c: short-circuit LHS write-back visible to the RHS.
    function h4c() external returns (uint256) {
        s.f = 5;
        if (bump(s) > 0 && s.f == 6) return 1;
        return 0;
    }

    // H4-d: ternary-condition write-back visible to the branches.
    function h4d() external returns (uint256) {
        s.f = 5;
        return bump(s) > 0 ? s.f : 0; // 6
    }

    // H4-e: left-assoc chain — rightmost s.f evaluates FIRST, reads 5.
    function h4e() external returns (uint256) {
        s.f = 5;
        return bump(s) + bump(s) + s.f; // 100 + 100 + 5
    }

    // H4-h: parenthesized nesting keeps per-op right-first order.
    function h4h() external returns (uint256) {
        s.f = 5;
        return (s.f + bump(s)) + s.f; // (6 + 100) + 5
    }

    // H4-f/g: call args left-to-right; arg write-back visible to later args.
    function h4f() external returns (uint256) {
        s.f = 5;
        return two(bump(s), s.f); // 100*1000 + 6
    }

    function h4g() external returns (uint256) {
        s.f = 5;
        return two(s.f, bump(s)); // 5*1000 + 100
    }

    // M5-a: RHS evaluates before the LHS index side effect.
    function m5a() external returns (uint256 r0, uint256 r1, uint256 i) {
        delete arr;
        arr.push(); arr.push();
        uint256 j = 0;
        arr[j++] = j; // arr[0] = 0
        r0 = arr[0]; r1 = arr[1]; i = j;
    }

    // M5-b: RHS side effect first, LHS index reads the updated local.
    function m5b() external returns (uint256 r0, uint256 r1, uint256 i) {
        delete arr;
        arr.push(); arr.push();
        uint256 j = 0;
        arr[j] = ++j; // arr[1] = 1
        r0 = arr[0]; r1 = arr[1]; i = j;
    }

    // M5-c: side-effecting LHS index + composite RHS with callee mutation.
    function m5c() external returns (uint256 a0, uint256 sf) {
        s.f = 5;
        delete arr;
        arr.push(); arr.push();
        uint256 j = 0;
        arr[j++] = bump(s) + s.f; // arr[0] = 100 + 5
        a0 = arr[0]; sf = s.f;
    }

    // M5-d: RHS callee mutates the assigned slot — the STORE wins.
    function m5d() external returns (uint256) {
        s.f = 5;
        s.f = bump(s);
        return s.f; // 100
    }

    // M5-e: compound assign — RHS mutation first, LHS old-value read sees it.
    function m5e() external returns (uint256) {
        s.f = 5;
        s.f += bump(s);
        return s.f; // 6 + 100
    }

    // M5-f: post-inc RHS value, LHS index reads the updated local.
    function m5f() external returns (uint256 r0, uint256 r1, uint256 j) {
        delete arr;
        arr.push(); arr.push();
        uint256 i = 0;
        arr[i] = i++; // arr[1] = 0
        r0 = arr[0]; r1 = arr[1]; j = i;
    }
}
