// Auto-generated PUBLIC array/UDVT getter for a signed sub-256 element must
// sign-extend to canonical 256-bit TC. The sign-extension was gated on bits<=64,
// so a 64<bits<256 signed element (int72/int128) or a UDVT over one returned the
// element at its NATURAL width (int72 -1 = 2^72-1, a huge positive on the wire).
// Found by the corpus-mutation fuzzer (memory_to_storage uint16->int72).
pragma abicoder v2;
type Small is int72;
contract C {
    Small[] public small;    // UDVT int72
    int72[] public a72;
    int128[] public a128;
    int32[] public a32;      // <=64 signed (was already ok — regression check)
    int72 public scalar72;   // scalar signed sub-256 (regression check)
    function setAll(int72[] memory v) public {
        Small[] memory w = new Small[](v.length);
        int128[] memory c = new int128[](v.length);
        int32[] memory d = new int32[](v.length);
        for (uint i = 0; i < v.length; i++) { w[i] = Small.wrap(v[i]); c[i] = int128(v[i]); d[i] = int32(v[i]); }
        small = w; a72 = v; a128 = c; a32 = d;
    }
    function setScalar(int72 v) public { scalar72 = v; }
}
