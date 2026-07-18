// Guard: a mapping inside a struct held in a storage ARRAY must be isolated
// PER ELEMENT. The mapping-key derivation walks the index chain and folds each
// level into the box key, but `arr[i].m[k]` stops the walk at the MemberAccess
// (`arr[i].m`), so the array index was dropped and the prefix fell back to plain
// utf8("m") — every element's mapping shared ONE box, so a write to arr[1].m[k]
// silently clobbered arr[0].m[k]. No revert, just wrong data.
//
// Over-firing guard for the sibling MAPPING base (`mapping(uint=>S) mm; mm[a].m[b]`,
// which must keep the per-layer sha256 derivation) is the pre-existing
// types/struct_mapping_abstract_constructor_param.sol — the first version of this
// fix broke it by treating a mapping base as an array.
contract ArrayStructMappingAlias {
    struct S { mapping(uint => uint) m; uint x; }

    S[] sarr;                       // array of structs holding a mapping
    mapping(uint => uint)[] aom;    // array of mappings (control)
    mapping(uint => S) mm;          // MAPPING to struct holding a mapping
    S st1;                          // two same-typed struct STATE VARS
    S st2;
    struct Outer { S inner; uint y; }
    Outer o1;                       // struct-IN-struct state vars (chain depth 2)
    Outer o2;

    function seed() public {
        st1.m[7] = 100;
        st2.m[7] = 200;             // must NOT clobber st1.m[7]
        S storage p = st1;          // aliased write lands where direct reads look
        p.m[8] = 111;

        o1.inner.m[7] = 1000;
        o2.inner.m[7] = 2000;       // must NOT clobber o1.inner.m[7]

        sarr.push(); sarr.push();
        sarr[0].m[7] = 100;
        sarr[1].m[7] = 200;         // must NOT clobber sarr[0].m[7]
        sarr[0].x = 1; sarr[1].x = 2;

        aom.push(); aom.push();
        aom[0][7] = 300;
        aom[1][7] = 400;

        mm[0].m[7] = 500;
        mm[1].m[7] = 600;           // must NOT clobber mm[0].m[7]
    }

    function s(uint i) public view returns (uint) { return sarr[i].m[7]; }
    function x(uint i) public view returns (uint) { return sarr[i].x; }
    function a(uint i) public view returns (uint) { return aom[i][7]; }
    function m(uint i) public view returns (uint) { return mm[i].m[7]; }
    function st(uint i, uint k) public view returns (uint) {
        return i == 0 ? st1.m[k] : st2.m[k];
    }
    function o(uint i, uint k) public view returns (uint) {
        return i == 0 ? o1.inner.m[k] : o2.inner.m[k];
    }

    // re-write: element isolation must survive later writes too
    function bump(uint i, uint v) public { sarr[i].m[7] = v; }
}
