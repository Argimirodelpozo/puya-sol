// Guard: indexing an EMPTY calldata array kept as an ARC4 VALUE must revert
// (EVM Panic 0x32). In an asm-mode function the calldata struct skips the
// native decode, so `s.m[0]` indexes the raw ARC4 encoding — and puya's
// IndexExpression lowering has no length check, only the physical extract
// boundary. An empty array inside a larger encoding stays within valid bytes:
// `s.m[0]` with s = ([]) read adjacent struct bytes (the length word) and
// returned 0 instead of reverting. Found by the night-3 stmt-del mutant on
// viaYul/dirty_calldata_struct — the deleted statement was incidental; the
// empty inner array was the trigger.
pragma abicoder v2;
contract CalldataEmptyArrayIndex {
    struct S { uint16[] m; }

    // asm-mode: struct stays ARC4, the buggy path
    function f(S calldata s) public pure returns (uint r) {
        uint x = uint(s.m[0]);
        assembly { r := x }
    }

    // runtime index variant
    function fi(S calldata s, uint i) public pure returns (uint r) {
        uint x = uint(s.m[i]);
        assembly { r := x }
    }
}
