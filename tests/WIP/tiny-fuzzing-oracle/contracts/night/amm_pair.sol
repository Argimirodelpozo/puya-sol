// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
contract AmmPair {
    uint112 public reserve0;
    uint112 public reserve1;
    uint256 public totalLiquidity;
    mapping(address => uint256) public liquidity;

    function _min(uint256 a, uint256 b) internal pure returns (uint256) { return a < b ? a : b; }
    function _sqrt(uint256 y) internal pure returns (uint256 z) {
        if (y > 3) { z = y; uint256 x = y / 2 + 1; while (x < z) { z = x; x = (y / x + x) / 2; } }
        else if (y != 0) { z = 1; }
    }
    function mint(address to, uint256 a0, uint256 a1) external returns (uint256 liq) {
        if (totalLiquidity == 0) liq = _sqrt(a0 * a1);
        else liq = _min((a0 * totalLiquidity) / reserve0, (a1 * totalLiquidity) / reserve1);
        liquidity[to] += liq; totalLiquidity += liq;
        reserve0 += uint112(a0); reserve1 += uint112(a1);
    }
    function getAmountOut(uint256 amountIn, uint256 rin, uint256 rout) public pure returns (uint256) {
        uint256 amountInWithFee = amountIn * 997;
        return (amountInWithFee * rout) / (rin * 1000 + amountInWithFee);
    }
    function swap0for1(uint256 amountIn) external returns (uint256 out) {
        out = getAmountOut(amountIn, reserve0, reserve1);
        reserve0 += uint112(amountIn); reserve1 -= uint112(out);
    }
    function burn(address from, uint256 liq) external returns (uint256 a0, uint256 a1) {
        a0 = (liq * reserve0) / totalLiquidity;
        a1 = (liq * reserve1) / totalLiquidity;
        liquidity[from] -= liq; totalLiquidity -= liq;
        reserve0 -= uint112(a0); reserve1 -= uint112(a1);
    }
}
