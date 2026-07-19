// Cold probe: bytes-array push, struct-field-array push/pop, dynamic array ops.
contract C {
    bytes b;
    uint256[] nums;
    struct S { uint32[] xs; uint256 tag; }
    S s;
    function pushByte(uint8 v) public { b.push(bytes1(v)); }
    function bLen() public view returns (uint256) { return b.length; }
    function bAt(uint256 i) public view returns (uint8) { return uint8(b[i]); }
    function pushNum(uint256 v) public { nums.push(v); }
    function popNum() public { nums.pop(); }
    function numsLen() public view returns (uint256) { return nums.length; }
    function numAt(uint256 i) public view returns (uint256) { return nums[i]; }
    function pushField(uint32 v) public { s.xs.push(v); s.tag += 1; }
    function popField() public { s.xs.pop(); }
    function fieldLen() public view returns (uint256) { return s.xs.length; }
    function fieldAt(uint256 i) public view returns (uint32) { return s.xs[i]; }
    function tag() public view returns (uint256) { return s.tag; }
}
