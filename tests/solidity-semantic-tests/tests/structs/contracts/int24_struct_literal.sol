// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// Guard for ARC4 encoding of signed/unsigned sub-word ints in a STRUCT LITERAL
// (the Uniswap V4 `Pool.ModifyLiquidityParams({tickLower: params.tickLower, ...})`
// shape). makeARC4Encode must emit the minimal n/8-byte two's-complement value;
// the previous `itob; b& mask; len<=n/8 assert` always reverted because AVM `b&`
// keeps the 8-byte itob width.
library Lib {
    struct SOut { address owner; int24 a; int24 b; int128 liq; int24 c; }
    struct UOut { address owner; uint24 a; uint24 b; uint128 liq; uint24 c; }
    function sconsume(SOut memory o) internal pure returns (int128) {
        return o.liq + int128(o.a) + int128(o.b) + int128(o.c);
    }
    function uconsume(UOut memory o) internal pure returns (uint128) {
        return o.liq + uint128(o.a) + uint128(o.b) + uint128(o.c);
    }
}

contract Int24StructLiteral {
    using Lib for Lib.SOut;
    using Lib for Lib.UOut;
    struct SIn { int24 a; int24 b; int128 liq; }
    struct UIn { uint24 a; uint24 b; uint128 liq; }

    function fs(SIn memory p, int24 spacing) external view returns (int128) {
        Lib.SOut memory o = Lib.SOut({owner: msg.sender, a: p.a, b: p.b, liq: p.liq, c: spacing});
        return o.sconsume();
    }
    function fu(UIn memory p, uint24 spacing) external view returns (uint128) {
        Lib.UOut memory o = Lib.UOut({owner: msg.sender, a: p.a, b: p.b, liq: p.liq, c: spacing});
        return o.uconsume();
    }
}
