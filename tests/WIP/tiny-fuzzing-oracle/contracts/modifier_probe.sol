// Directed probe for the COLD ModifierBodyInliner paths (coverage_map: 39.9%).
// Cold lines = modifiers on functions with NAMED/MULTIPLE returns, conditional
// placeholders, stacked modifiers, args. All differentially fuzzable (numeric).
contract C {
    uint256 s;

    modifier addOne() { _; s += 1; }
    modifier gate(uint256 lim) { require(lim < 1000); _; }
    modifier doubleWrap() { s *= 2; _; s *= 2; }
    modifier condSkip(bool go) { if (go) { _; } }        // conditional placeholder
    modifier clampRet() { _; }                            // wraps a named return

    // modifier on a function with a NAMED single return (cold path)
    function namedRet(int64 a, int64 b) public gate(uint256(int256(a > 0 ? a : -a))) clampRet()
        returns (int64 r)
    {
        r = a * b;
    }

    // modifier on MULTIPLE named returns (cold __mod_retval_ multi path)
    function multiRet(uint64 x, uint64 y) public addOne() returns (uint64 p, uint64 q, uint64 d) {
        p = x + y;
        q = x * y;
        d = x >= y ? x - y : y - x;
    }

    // STACKED modifiers wrapping logic + return
    function stacked(uint128 v) public doubleWrap() gate(v % 500) returns (uint256) {
        return uint256(v) + s;
    }

    // conditional-placeholder modifier: body may not run
    function maybe(bool go, uint256 add) public condSkip(go) returns (uint256) {
        s += add;
        return s;
    }

    // plain state reads to observe modifier side effects
    function getS() public view returns (uint256) { return s; }

    // signed sub-word return through a modifier (interacts with sign-extension)
    function signedRet(int32 a) public addOne() returns (int32) {
        return -a * 3;
    }
}
