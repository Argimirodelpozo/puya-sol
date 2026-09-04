// Modifier ARGUMENT that is a side-effecting expression (ternary with a
// negate/checked branch): its SolConditional if/else must be drained into the
// modifier body BEFORE the arg binding. Found by coverage-guided fuzzing
// (modifier-chain cold path). Was: `mod(a > 0 ? a : -a)` collapsed to the
// false branch -> reverted every call.
contract C {
    uint256 s;
    modifier gate(uint256 lim) { require(lim < 1000); _; }
    modifier addOne() { _; s += 1; }

    // ternary + unary negate as the modifier arg (the original discriminator)
    function absGate(int64 a) public gate(uint256(int256(a > 0 ? a : -a))) returns (int64) { return a; }
    // stacked: gated + named return + body multiply
    function stackedNamed(int64 a, int64 b) public gate(uint256(int256(a >= 0 ? a : -a))) addOne()
        returns (int64 r) { r = a * b; }
    function getS() public view returns (uint256) { return s; }
}
