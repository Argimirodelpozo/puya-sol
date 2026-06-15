// CUSTOM (puya-sol): abi.decode of an array-of-dynamic-elements must
// hard-error (fail-loud), not silently return []. abi.encode of this type
// is correct; the recursive-offset-table decode is a missing feature (#20).
contract C {
    function f(bytes memory x) public pure returns (uint256[][] memory) {
        return abi.decode(x, (uint256[][]));
    }
}
