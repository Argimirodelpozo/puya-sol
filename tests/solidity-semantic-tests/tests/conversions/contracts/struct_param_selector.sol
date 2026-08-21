// CUSTOM (puya-sol regression): selectors for methods with struct/array/
// enum parameters must expand to the ARC4 tuple form the callee router
// dispatches on, not "struct Name".
contract C {
    struct P { uint a; uint b; }
    struct Nested { P p; uint8 c; }
    enum E { A, B }
    struct WithEnum { E e; int8 s; bytes3 b; }

    function takeStruct(P memory p) public returns (uint) { return p.a; }
    function takeNested(Nested memory n) public returns (uint) { return n.c; }
    function takeStructArr(P[] memory ps) public returns (uint) { return ps.length; }
    function takeEnumStruct(WithEnum memory w) public returns (uint) { return uint(uint8(w.e)); }

    function selStruct() public view returns (bytes4) { return this.takeStruct.selector; }
    function selNested() public view returns (bytes4) { return this.takeNested.selector; }
    function selStructArr() public view returns (bytes4) { return this.takeStructArr.selector; }
    function selEnumStruct() public view returns (bytes4) { return this.takeEnumStruct.selector; }
}
