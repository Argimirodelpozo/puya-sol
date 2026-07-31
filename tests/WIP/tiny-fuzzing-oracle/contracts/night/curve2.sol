// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
// Curve StableSwap core math, single contract, realistic-scale scenarios folded
// in so fuzzing drives the FULL Newton iteration (no inner `new`, no confound).
contract Curve2 {
    uint256 constant N = 3;
    uint256 constant A = 100;
    uint256 constant FEE = 4000000;
    uint256 constant FEE_DENOM = 10000000000;

    function _getD(uint256[3] memory xp) internal pure returns (uint256) {
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
    function _getY(uint256 i, uint256 j, uint256 x, uint256[3] memory xp) internal pure returns (uint256) {
        uint256 D = _getD(xp);
        uint256 Ann = A * N;
        uint256 c = D; uint256 S_ = 0;
        for (uint256 k = 0; k < N; k++) {
            uint256 _x;
            if (k == i) _x = x; else if (k != j) _x = xp[k]; else continue;
            S_ += _x;
            c = (c * D) / (_x * N);
        }
        c = (c * D) / (Ann * N);
        uint256 b = S_ + D / Ann;
        uint256 y = D;
        for (uint256 it = 0; it < 255; it++) {
            uint256 yprev = y;
            y = (y * y + c) / (2 * y + b - D);
            if (y > yprev) { if (y - yprev <= 1) break; } else { if (yprev - y <= 1) break; }
        }
        return y;
    }
    // realistic-scale D: scale small fuzz ints to 1e18 balances → full iteration
    function realisticD(uint256 a, uint256 b, uint256 cc) external pure returns (uint256) {
        uint256[3] memory xp;
        xp[0] = (a % 10_000_000 + 1) * 1e18;
        xp[1] = (b % 10_000_000 + 1) * 1e18;
        xp[2] = (cc % 10_000_000 + 1) * 1e18;
        return _getD(xp);
    }
    // realistic swap output: balanced-ish pool, swap dx of coin i for j
    function realisticDy(uint256 dxUnits, uint256 imb, uint256 ij) external pure returns (uint256) {
        uint256 base = 1_000_000 * 1e18;
        uint256[3] memory xp;
        xp[0] = base;
        xp[1] = base + (imb % base);
        xp[2] = base - (imb % (base / 2));
        uint256 i = ij % 3; uint256 j = (ij / 3) % 3;
        if (i == j) j = (j + 1) % 3;
        uint256 dx = (dxUnits % 100_000 + 1) * 1e18;
        uint256 y = _getY(i, j, xp[i] + dx, xp);
        uint256 dy = xp[j] - y - 1;
        return dy - (FEE * dy) / FEE_DENOM;
    }
    // direct getD/getY for boundary coverage too
    function getD(uint256[3] memory xp) external pure returns (uint256) { return _getD(xp); }
    function getY(uint256 i, uint256 j, uint256 x, uint256[3] memory xp) external pure returns (uint256) {
        if (i == j || i >= 3 || j >= 3) revert("bad");
        return _getY(i, j, x, xp);
    }
}
