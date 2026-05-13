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
//   f() -> true, true, 7
//   g() -> true, true, 42
//   h() -> true, true, 23
// AVM-native: selector-equality check is false; delegatecall
// returns success=true with no data, so the decoded value is 0.
// ============================================================

library L {
    function f(uint256 x) external returns (uint) { return x; }
    function g(uint256[] storage s) external returns (uint) { return s.length; }
    function h(uint256[] memory m) public returns (uint) { return m.length; }
}
contract C {
    uint256[] s;
    constructor() { while (s.length < 42) s.push(0); }
    function f() public returns (bool, bool, uint256) {
		(bool success, bytes memory data) = address(L).delegatecall(abi.encodeWithSelector(L.f.selector, 7));
		return (L.f.selector == bytes4(keccak256("f(uint256)")), success, abi.decode(data, (uint256)));
    }
    function g() public returns (bool, bool, uint256) {
		uint256 s_ptr;
		assembly { s_ptr := s.slot }
		(bool success, bytes memory data) = address(L).delegatecall(abi.encodeWithSelector(L.g.selector, s_ptr));
		return (L.g.selector == bytes4(keccak256("g(uint256[] storage)")), success, abi.decode(data, (uint256)));
    }
    function h() public returns (bool, bool, uint256) {
		(bool success, bytes memory data) = address(L).delegatecall(abi.encodeWithSelector(L.h.selector, new uint256[](23)));
		return (L.h.selector == bytes4(keccak256("h(uint256[])")), success, abi.decode(data, (uint256)));
    }
}
// ====
// EVMVersion: >homestead
// ----
// library: L
// f() -> false, true, 0
// g() -> false, true, 0
// h() -> false, true, 0
