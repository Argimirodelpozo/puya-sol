// Uniswap V4 LiquidityMath.addDelta: signextend(15, y) (constant byte index, int128 y)
// composed with a 128-bit add and an shr(128,...) overflow guard. Exercises a NEGATIVE
// int128 delta round-tripping through signextend INSIDE a larger expression — the
// composition that the constant-b signextend lowering used to corrupt (reverted instead
// of round-tripping), which broke Uniswap V4 remove-liquidity.
contract C {
    function addDelta(uint128 x, int128 y) public pure returns (uint128 z) {
        assembly ("memory-safe") {
            z := add(and(x, 0xffffffffffffffffffffffffffffffff), signextend(15, y))
            if shr(128, z) {
                mstore(0, 0x93dafdf1) // SafeCastOverflow()
                revert(0x1c, 0x04)
            }
        }
    }
}
