// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
// Faithful port of Curve StableSwap (3-coin) core invariant math: get_D (Newton
// iteration on the StableSwap invariant) + get_y (solve one balance) + get_dy
// (exchange output). Real, iterative, big-number arithmetic. Balances kept in
// storage; math is the exact Curve algorithm.
contract CurveStableSwap {
    uint256 constant N = 3;
    uint256 constant A = 100;            // amplification coefficient
    uint256 constant FEE = 4000000;      // 0.04% (1e10 base)
    uint256 constant FEE_DENOM = 10000000000;
    uint256[3] public balances;

    function setBalances(uint256 b0, uint256 b1, uint256 b2) external {
        balances[0] = b0; balances[1] = b1; balances[2] = b2;
    }

    function getD(uint256[3] memory xp) public pure returns (uint256) {
        uint256 S = 0;
        for (uint256 i = 0; i < N; i++) S += xp[i];
        if (S == 0) return 0;
        uint256 D = S;
        uint256 Ann = A * N;
        for (uint256 j = 0; j < 255; j++) {
            uint256 D_P = D;
            for (uint256 i = 0; i < N; i++) D_P = (D_P * D) / (xp[i] * N);
            uint256 Dprev = D;
            D = ((Ann * S + D_P * N) * D) / ((Ann - 1) * D + (N + 1) * D_P);
            if (D > Dprev) { if (D - Dprev <= 1) break; }
            else { if (Dprev - D <= 1) break; }
        }
        return D;
    }

    function getY(uint256 i, uint256 j, uint256 x, uint256[3] memory xp) public pure returns (uint256) {
        require(i != j && i < N && j < N, "bad idx");
        uint256 D = getD(xp);
        uint256 Ann = A * N;
        uint256 c = D;
        uint256 S_ = 0;
        for (uint256 k = 0; k < N; k++) {
            uint256 _x;
            if (k == i) _x = x;
            else if (k != j) _x = xp[k];
            else continue;
            S_ += _x;
            c = (c * D) / (_x * N);
        }
        c = (c * D) / (Ann * N);
        uint256 b = S_ + D / Ann;
        uint256 y = D;
        for (uint256 it = 0; it < 255; it++) {
            uint256 yprev = y;
            y = (y * y + c) / (2 * y + b - D);
            if (y > yprev) { if (y - yprev <= 1) break; }
            else { if (yprev - y <= 1) break; }
        }
        return y;
    }

    function getDy(uint256 i, uint256 j, uint256 dx) external view returns (uint256) {
        uint256[3] memory xp = balances;
        uint256 x = xp[i] + dx;
        uint256 y = getY(i, j, x, xp);
        uint256 dy = xp[j] - y - 1;
        uint256 fee = (FEE * dy) / FEE_DENOM;
        return dy - fee;
    }

    function invariant() external view returns (uint256) {
        return getD(balances);
    }
}

contract CurveScenario {
    CurveStableSwap immutable pool;
    constructor() { pool = new CurveStableSwap(); }
    function realisticDy(uint256 dxUnits, uint256 imbalance) external returns (uint256) {
        uint256 base = 1_000_000 * 1e18;
        pool.setBalances(base, base + (imbalance % base), base - (imbalance % (base / 2)));
        uint256 dx = (dxUnits % 100_000) * 1e18;
        return pool.getDy(0, 1, dx);
    }
    function realisticD(uint256 a, uint256 b, uint256 c) external pure returns (uint256) {
        uint256[3] memory xp;
        xp[0] = (a % 10_000_000 + 1) * 1e18;
        xp[1] = (b % 10_000_000 + 1) * 1e18;
        xp[2] = (c % 10_000_000 + 1) * 1e18;
        CurveStableSwap p;  // pure helper call not possible; inline via a fresh static call substitute
        return _d(xp);
    }
    function _d(uint256[3] memory xp) internal pure returns (uint256) {
        // duplicate getD to keep realisticD pure
        uint256 N = 3; uint256 A = 100;
        uint256 S = xp[0] + xp[1] + xp[2];
        if (S == 0) return 0;
        uint256 D = S; uint256 Ann = A * N;
        for (uint256 j = 0; j < 255; j++) {
            uint256 D_P = D;
            for (uint256 i = 0; i < N; i++) D_P = (D_P * D) / (xp[i] * N);
            uint256 Dprev = D;
            D = ((Ann * S + D_P * N) * D) / ((Ann - 1) * D + (N + 1) * D_P);
            if (D > Dprev) { if (D - Dprev <= 1) break; } else { if (Dprev - D <= 1) break; }
        }
        return D;
    }
}
