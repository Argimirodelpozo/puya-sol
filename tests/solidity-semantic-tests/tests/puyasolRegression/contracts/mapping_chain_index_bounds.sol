// Guard: ARRAY levels feeding the mapping-key derivation must bounds-check the
// element index (EVM Panic 0x32). The index folds into the derived box key, so
// an out-of-bounds `a[a.length][k]` used to read/write a PHANTOM element's box
// (returning 0 / silently storing) where EVM reverts. Covers: dynamic array of
// mappings, fixed-size array of mappings, and array-of-structs-with-mapping
// (whose index feeds the concat prefix instead of a hash layer).
contract MappingChainIndexBounds {
    mapping(uint => uint)[] aom;        // dynamic array of mappings
    mapping(uint => uint)[3] fixedAom;  // fixed-size array of mappings
    struct S { mapping(uint => uint) m; uint x; }
    S[] sarr;                           // dynamic array of structs w/ mapping

    function seed() public {
        aom.push(); aom[0][1] = 11;
        fixedAom[2][1] = 22;
        sarr.push(); sarr[0].m[1] = 33;
    }

    function readAom(uint i, uint k) public view returns (uint) { return aom[i][k]; }
    function writeAom(uint i, uint k, uint v) public { aom[i][k] = v; }
    function readFixed(uint i, uint k) public view returns (uint) { return fixedAom[i][k]; }
    function readSarrM(uint i, uint k) public view returns (uint) { return sarr[i].m[k]; }

    // in-bounds `length - 1` idiom must keep working
    function readLast(uint k) public view returns (uint) { return aom[aom.length - 1][k]; }
}
