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

    function seed() public {
        sarr.push(); sarr.push();
        sarr[0].m[7] = 100;
        sarr[1].m[7] = 200;         // must NOT clobber sarr[0].m[7]
        sarr[0].x = 1; sarr[1].x = 2;

        aom.push(); aom.push();
        aom[0][7] = 300;
        aom[1][7] = 400;
    }

    function s(uint i) public view returns (uint) { return sarr[i].m[7]; }
    function x(uint i) public view returns (uint) { return sarr[i].x; }
    function a(uint i) public view returns (uint) { return aom[i][7]; }

    // re-write: element isolation must survive later writes too
    function bump(uint i, uint v) public { sarr[i].m[7] = v; }
}
