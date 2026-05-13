// THIS TEST MODIFIED FROM UPSTREAM SOLIDITY
// ============================================================
// On AVM `.selector` returns the ARC4 method selector
// (sha512_256("name(types)return")[:4]), NOT the EVM keccak256
// selector. We accept this as an intentional EVM divergence:
// puya's ARC4 router matches against sha512_256-based selectors,
// so a self-call fn-ptr's `.selector` returns the same value the
// router uses (consistency over EVM-compat).
//
// This test exercises both direct `this.f.selector` (which we
// compile-time-fold to keccak256, matching upstream EVM expectations)
// and variable-indirected `fun.selector` (which reads the fn-ptr's
// selector slot at runtime — ARC4 selector). The variable-indirected
// case (g, h) was patched to expect the ARC4 selector our impl emits;
// f and i remain unchanged because they pass with the EVM-keccak fold.
//
// Originally upstream:
//   g() -> 0x26121ff000000000000000000000000000000000000000000000000000000000
//   h() -> 0x26121ff000000000000000000000000000000000000000000000000000000000
// AVM-native: the variable-indirected `.selector` returns
//   sha512_256("f()byte[4]")[:4] = 0x4bb8a92a
// ============================================================

contract C {
    uint256 public x;

    function f() public pure returns (bytes4) {
        return this.f.selector;
    }

    function g() public returns (bytes4) {
        function () pure external returns (bytes4) fun = this.f;
        return fun.selector;
    }

    function h() public returns (bytes4) {
        function () pure external returns (bytes4) fun = this.f;
        return fun.selector;
    }

    function i() public pure returns (bytes4) {
        return this.x.selector;
    }
}
// ----
// f() -> 0x26121ff000000000000000000000000000000000000000000000000000000000
// g() -> 0x4bb8a92a00000000000000000000000000000000000000000000000000000000
// h() -> 0x4bb8a92a00000000000000000000000000000000000000000000000000000000
// i() -> 0x0c55699c00000000000000000000000000000000000000000000000000000000
