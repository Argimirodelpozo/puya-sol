pragma solidity ^0.8.20;

contract ModifierArguments {
    struct S { uint256 v; }
    S s;
    mapping(uint256 => S) items;
    uint256 private keyCalls;
    modifier check(int128 v) { require(v == -1); _; }
    modifier touch(S storage r) { r.v++; _; }
    function widened(int8 a) external pure check(a) returns (bool) { return true; }
    function plain() external touch(s) returns (uint256) { return s.v; }
    function parens() external touch(((s))) returns (uint256) { return s.v; }
    function mapped() external touch(items[3]) returns (uint256) { return items[3].v; }
    function key() internal returns (uint256) { keyCalls++; return 5; }
    function computed() external touch(items[key()]) returns (uint256, uint256) {
        return (items[5].v, keyCalls);
    }
}
