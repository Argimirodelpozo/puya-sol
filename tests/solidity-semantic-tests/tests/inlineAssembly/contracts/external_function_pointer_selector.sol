// THIS TEST MODIFIED FROM UPSTREAM SOLIDITY
// ============================================================
// On AVM `.selector` returns the ARC4 method selector
// (sha512_256("name(types)return")[:4]), NOT the EVM keccak256
// selector. We accept this as an intentional EVM divergence:
// puya's ARC4 router matches against sha512_256-based selectors,
// so a fn-ptr's `.selector` slot stores the same value the router
// uses (consistency over EVM-compat).
//
// testSol() unchanged — `this.testFunction.selector` is compile-time
// folded to keccak256, matching upstream EVM expectations.
//
// testYul() patched: assembly `fp.selector` reads the runtime selector
// slot, which on AVM holds the ARC4 selector
// sha512_256("testFunction()void")[:4] = 0x89aac53b.
//
// Originally upstream:
//   testYul() -> 0xe16b4a9b
// AVM-native:
//   testYul() -> 0x89aac53b
// ============================================================

contract C {
	function testFunction() external {}

	function testYul() public returns (uint32) {
		function() external fp = this.testFunction;
		uint selectorValue = 0;

		assembly {
			selectorValue := fp.selector
		}

		// Value is right-aligned, we shift it so it can be compared
		return uint32(bytes4(bytes32(selectorValue << (256 - 32))));
	}
	function testSol() public returns (uint32) {
		return uint32(this.testFunction.selector);
	}
}
// ----
// testYul() -> 0x89aac53b
// testSol() -> 0xe16b4a9b
