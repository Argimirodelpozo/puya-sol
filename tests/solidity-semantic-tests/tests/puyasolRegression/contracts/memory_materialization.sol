// SPDX-License-Identifier: MIT
pragma solidity ^0.8.28;
contract LiteralDirect {
    function f() external pure returns (uint256) { return bytes("abcd").length; }
}
contract LiteralReturn {
    function literal() internal pure returns (string memory) { return "abcd"; }
    function f() external pure returns (uint256) { return bytes(literal()).length; }
}
contract StructDirect {
    struct S { uint256 x; uint256 y; }
    function f(uint256 x) external pure returns (uint256) { S memory s = S(x, 2); return s.x + s.y; }
}
contract StructReturn {
    struct S { uint256 x; uint256 y; }
    function make(uint256 x) internal pure returns (S memory) { return S(x, 2); }
    function f(uint256 x) external pure returns (uint256) { S memory s = make(x); return s.x + s.y; }
    function aliasResult() external pure returns (uint256) {
        S memory a = make(1); S memory b = a; b.x = 9; return a.x;
    }
    function rebindResult() external pure returns (uint256, uint256) {
        S memory a = make(1); S memory b = a; a = make(2); b.x = 9; return (a.x, b.x);
    }
}
contract IgnoredReturn {
    struct S { uint256 x; uint256 y; }
    function make() internal pure returns (S memory) { return S(1, 2); }
    function f() external pure returns (uint256) { make(); return 7; }
}
contract NoIgnoredReturn {
    function f() external pure returns (uint256) { return 7; }
}
