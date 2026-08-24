// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract SlotRefMat {
    struct P { uint64 x; uint64 y; }
    P a;
    P b;

    constructor() { a = P(1, 2); b = P(30, 40); }

    function useMem(P memory m) internal pure returns (uint256) { return m.x + m.y; }

    // vardecl storage->memory copy
    function viaDecl(bool pick) external view returns (uint256) {
        P storage s = pick ? a : b;
        P memory m = s;
        return m.x + m.y;
    }
    // arg storage->memory coercion
    function viaArg(bool pick) external view returns (uint256) {
        P storage s = pick ? a : b;
        return useMem(s);
    }
    // return storage->memory
    function retMem(bool pick) internal view returns (P memory) {
        P storage s = pick ? a : b;
        return s;
    }
    function viaReturn(bool pick) external view returns (uint256) {
        P memory m = retMem(pick);
        return m.x + m.y;
    }
    // the ternary site (known handled)
    function viaTernary(bool pick) external view returns (uint256) {
        P memory m = pick ? a : b;
        return m.x + m.y;
    }
}
