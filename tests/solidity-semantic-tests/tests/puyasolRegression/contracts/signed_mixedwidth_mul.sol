// Mixed-width signed add/sub/mul: a narrower signed operand must sign-extend to
// the common width before the biguint arithmetic. Found by the corpus-mutation
// fuzzer (small_signed_types.sol int64->int192).
contract C {
    // uint64-backed narrower operand (int32/int64) × biguint-backed wider (int192)
    function mul32_192(int32 a, int192 b) external pure returns (int256) { return a * b; }
    function mul64_192(int64 a, int192 b) external pure returns (int256) { return a * b; }
    // both biguint-backed but different widths (int128 × int192)
    function mul128_192(int128 a, int192 b) external pure returns (int256) { return a * b; }
    // add / sub mixed width
    function add32_192(int32 a, int192 b) external pure returns (int256) { return a + b; }
    function sub192_32(int192 a, int32 b) external pure returns (int256) { return a - b; }
    // the original fuzzer discriminator: negated sub-word literals
    function run() external pure returns (int256) { return -int32(10) * -int192(20); }
    // narrower operand on the RIGHT
    function mul192_32(int192 a, int32 b) external pure returns (int256) { return a * b; }
}
