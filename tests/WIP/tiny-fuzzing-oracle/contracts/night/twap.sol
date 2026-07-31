// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
contract Twap {
    int256 public cumulative;
    int56 public lastTick;
    uint32 public lastTs;
    function update(int56 tick, uint32 ts) external {
        int256 dt = int256(uint256(ts - lastTs));
        cumulative += int256(lastTick) * dt;
        lastTick = tick; lastTs = ts;
    }
    function consult(uint32 ts0, int256 cum0, uint32 ts1, int256 cum1) external pure returns (int56) {
        uint32 dt = ts1 - ts0;
        if (dt == 0) return 0;
        int256 avg = (cum1 - cum0) / int256(uint256(dt));
        return int56(avg);
    }
    function tickToRatio(int24 tick) external pure returns (int256) {
        int256 t = int256(tick);
        return t * t * (t < 0 ? int256(-1) : int256(1));
    }
    function absDiff(int128 a, int128 b) external pure returns (uint128) {
        return a >= b ? uint128(a - b) : uint128(b - a);
    }
}
