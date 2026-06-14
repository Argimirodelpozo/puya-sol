// CUSTOM (puya-sol): abi.decode of a struct with mixed-width fields must
// field-walk (each field its own EVM slot), not slab-reinterpret. The
// decode counterpart to the signed-aggregate encode fix.
contract C {
    struct P { uint256 a; int128 b; address c; bool d; }
    function rt(P memory p) public pure returns (uint256, int128, address, bool) {
        P memory q = abi.decode(abi.encode(p), (P));
        return (q.a, q.b, q.c, q.d);
    }
    struct Q { int128 x; uint8 y; bool z; }   // small (totalSize<=32) mixed
    function rtSmall(Q memory q) public pure returns (int128, uint8, bool) {
        Q memory r = abi.decode(abi.encode(q), (Q));
        return (r.x, r.y, r.z);
    }
}
