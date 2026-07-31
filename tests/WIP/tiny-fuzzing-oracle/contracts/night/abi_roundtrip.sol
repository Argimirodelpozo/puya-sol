// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
contract AbiRoundtrip {
    function u256(uint256 x) external pure returns (uint256) { return abi.decode(abi.encode(x), (uint256)); }
    function tup(uint256 a, address b, bool c) external pure returns (uint256,address,bool) {
        return abi.decode(abi.encode(a,b,c), (uint256,address,bool));
    }
    function arr(uint256 a, uint256 b, uint256 c) external pure returns (uint256[] memory) {
        uint256[] memory m = new uint256[](3); m[0]=a;m[1]=b;m[2]=c;
        return abi.decode(abi.encode(m), (uint256[]));
    }
    struct S { uint256 x; address y; }
    function st(uint256 x, address y) external pure returns (uint256, address) {
        S memory s = abi.decode(abi.encode(S(x,y)), (S));
        return (s.x, s.y);
    }
    function packedLen(uint256 a, uint8 b, address c) external pure returns (uint256) {
        return abi.encodePacked(a, b, c).length;
    }
    function encodeLen(uint256 a, uint8 b, address c) external pure returns (uint256) {
        return abi.encode(a, b, c).length;
    }
}
