// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// CUSTOM regression fixture (NOT vendored / not an original Solidity semantic test).
// Guards slot-handle fixed-array element access (biguint .slot-rebound refs):
// (1) BOUNDS: an out-of-range runtime index addressed base+idx directly,
//     silently reading/writing a NEIGHBORING slot where EVM panics 0x32.
// (2) PACKED COMPOUND: `p[i] += v` bypassed the packed-aware intercept (plain
//     `=` only) and did an unscaled whole-word read-modify-write at slot
//     base+i — the wrong slot AND a whole-word clobber.
contract SlotHandleArrayBounds {
    uint8[8] packed;
    uint256[2] pair;

    // asm .slot rebind → the storage ref returns as a biguint slot handle.
    function getPacked() internal view returns (uint8[8] storage r) {
        r = packed;
        uint256 s = 100;
        assembly { r.slot := s }
    }

    function getPair() internal view returns (uint256[2] storage r) {
        r = pair;
        uint256 s = 200;
        assembly { r.slot := s }
    }

    // ── full-word elements (bound local → slot-ref path) ──
    function rdPair(uint256 i) external view returns (uint256) {
        uint256[2] storage p = getPair();
        return p[i];
    }

    function wrPair(uint256 i, uint256 v) external {
        uint256[2] storage p = getPair();
        p[i] = v;
    }

    // chained call form (generic biguint-base path)
    function rdPairChained(uint256 i) external view returns (uint256) {
        return getPair()[i];
    }

    // ── packed elements ──
    function rdPacked(uint256 i) external view returns (uint8) {
        uint8[8] storage p = getPacked();
        return p[i];
    }

    function wrPacked(uint256 i, uint8 v) external {
        uint8[8] storage p = getPacked();
        p[i] = v;
    }

    function bump(uint256 i, uint8 d) external {
        uint8[8] storage p = getPacked();
        p[i] += d;
    }

    function drop(uint256 i, uint8 d) external {
        uint8[8] storage p = getPacked();
        p[i] -= d;
    }
}
