// THIS TEST MODIFIED FROM UPSTREAM SOLIDITY
// ============================================================
// Two AVM divergences are exercised by this test:
//
// 1. `.selector` returns the ARC4 method selector
//    (sha512_256("name(types)return")[:4]), NOT keccak256.
//    The `L.X.selector == bytes4(keccak256(...))` comparisons
//    are therefore always false on AVM (sha512_256 ≠ keccak256).
//
// 2. EVM-style cross-contract delegatecall to an external library
//    address has no AVM equivalent: puya inlines library functions
//    as Subroutines in the same compilation unit, and the runtime
//    `address(L).delegatecall(...)` path is stubbed (success=true,
//    empty returndata). `abi.decode` of empty data returns 0.
//
// Originally upstream:
//   f() -> true, true, 42
//   g() -> true, true, 23
// AVM-native: selector-equality check is false; delegatecall
// returns success=true with no data, so the decoded value is 0.
// ============================================================

pragma abicoder               v2;
library L {
    struct S { uint256 a; }
    function f(S storage s) external returns (uint) { return s.a; }
    function g(S memory m) public returns (uint) { return m.a; }
}
contract C {
    L.S s;
    constructor() { s.a = 42; }

    function f() public returns (bool, bool, uint256) {
		uint256 s_ptr;
		assembly { s_ptr := s.slot }
		(bool success, bytes memory data) = address(L).delegatecall(abi.encodeWithSelector(L.f.selector, s_ptr));
		return (L.f.selector == bytes4(keccak256("f(L.S storage)")), success, abi.decode(data, (uint256)));
    }
    function g() public returns (bool, bool, uint256) {
		(bool success, bytes memory data) = address(L).delegatecall(abi.encodeWithSelector(L.g.selector, L.S(23)));
		return (L.g.selector == bytes4(keccak256("g(L.S)")), success, abi.decode(data, (uint256)));
    }
}
// ====
// EVMVersion: >homestead
// ----
// library: L
// f() -> false, true, 0
// g() -> false, true, 0
