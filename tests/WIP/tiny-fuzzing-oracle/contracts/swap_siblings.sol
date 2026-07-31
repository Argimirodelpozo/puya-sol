// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
contract SwapSiblings {
    struct P { uint256 a; uint256 b; uint256 c; }
    P sp;
    mapping(uint256 => uint256) sm;
    // storage struct value-field swap (LHS = MemberAccess)
    function structSwap(uint256 a, uint256 b) external returns (uint256,uint256) {
        sp.a=a; sp.b=b;
        (sp.a, sp.b) = (sp.b, sp.a);
        return (sp.a, sp.b);
    }
    // storage struct 3-field rotate
    function structRot3(uint256 a, uint256 b, uint256 c) external returns (uint256,uint256,uint256) {
        sp.a=a; sp.b=b; sp.c=c;
        (sp.a, sp.b, sp.c) = (sp.c, sp.a, sp.b);
        return (sp.a, sp.b, sp.c);
    }
    // memory struct value-field swap
    function memStructSwap(uint256 a, uint256 b) external pure returns (uint256,uint256) {
        P memory m = P(a, b, 0);
        (m.a, m.b) = (m.b, m.a);
        return (m.a, m.b);
    }
    // storage mapping-element swap (LHS = IndexAccess on MappingType)
    function mapSwap(uint256 a, uint256 b) external returns (uint256,uint256) {
        sm[0]=a; sm[1]=b;
        (sm[0], sm[1]) = (sm[1], sm[0]);
        return (sm[0], sm[1]);
    }
}
