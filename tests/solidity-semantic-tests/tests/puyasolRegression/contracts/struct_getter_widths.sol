// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
contract StructGetterWidths {
    struct P2 { uint128 a; uint128 b; }
    struct P3 { uint128 a; uint128 b; mapping(address => uint256) m; }
    struct P4 { uint64 a; uint32 b; bool c; mapping(address => uint256) m; }
    struct P5 { int128 a; address who; mapping(address => uint256) m; }
    mapping(uint256 => P2) public plain;
    mapping(uint256 => P3) public withmap;
    mapping(uint256 => P4) public withmap64;
    mapping(uint256 => P5) public withaddr;
    function set(uint256 k) external {
        plain[k] = P2(1, 2);
        withmap[k].a = 3; withmap[k].b = 4;
        withmap64[k].a = 5; withmap64[k].b = 6; withmap64[k].c = true;
        withaddr[k].a = -7; withaddr[k].who = msg.sender;
    }
}
