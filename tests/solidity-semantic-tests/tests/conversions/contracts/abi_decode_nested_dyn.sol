// CUSTOM (puya-sol): abi.decode of a nested-dynamic array whose elements are
// sub-32-byte dynamic arrays (uint128[][]) must still hard-error (fail-loud) —
// EVM 32-pads each uint128 element while ARC4 packs it to 16 bytes, a repack the
// recursive decoder does not implement. (uint256[][]/uint256[][][]/string[]/
// bytes[] ARE supported — see test_abi_decode_nested_dynamic_arrays.) Never
// silently return wrong data.
contract C {
    function f(bytes memory x) public pure returns (uint128[][] memory) {
        return abi.decode(x, (uint128[][]));
    }
}
