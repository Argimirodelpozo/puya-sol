// Struct-member array .slot route probe: sstore(x.slot, L) sets s.x length
// (COW through the struct box on AVM). Length values are CAPPED sanely by the
// setters (the fuzzer drives word writes through setLen) — sstore of a huge
// length is EVM-lazy but AVM-eager (bzero allocation), a known bounded-resources
// class; lenRaw exposes the uncapped path deliberately.
contract MemberArrayProbe {
    struct S { uint128 a; uint256[] x; uint240 b; }
    uint8 pre = 23;
    S s;
    uint8 post = 17;

    function push(uint256 v) public { s.x.push(v); }
    function pop() public { s.x.pop(); }
    function len() public view returns (uint256) { return s.x.length; }
    function at(uint256 i) public view returns (uint256) { return s.x[i]; }
    function setA(uint128 v) public { s.a = v; }
    function setB(uint240 v) public { s.b = v; }
    function getA() public view returns (uint128) { return s.a; }
    function getB() public view returns (uint240) { return s.b; }
    function guards() public view returns (uint8, uint8) { return (pre, post); }

    function lenViaSlot() public view returns (uint256 w) {
        uint256[] storage x = s.x;
        assembly { w := sload(x.slot) }
    }
    function setLen(uint256 v) public {
        uint256 capped = v % 8;   // keep AVM eager-allocation in bounds
        uint256[] storage x = s.x;
        assembly { sstore(x.slot, capped) }
    }
}
