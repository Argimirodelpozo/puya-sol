// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
contract StorSwapDet {
    uint256[] ua;
    uint256[3] fa;
    bool[] ba;
    // deterministic single-call: set, swap, return (storage dyn uint256)
    function uSwap(uint256 a, uint256 b) external returns (uint256, uint256) {
        delete ua; ua.push(a); ua.push(b);
        (ua[0], ua[1]) = (ua[1], ua[0]);
        return (ua[0], ua[1]);
    }
    // storage fixed uint256[3]
    function fSwap(uint256 a, uint256 b) external returns (uint256, uint256) {
        fa[0]=a; fa[1]=b; fa[2]=0;
        (fa[0], fa[1]) = (fa[1], fa[0]);
        return (fa[0], fa[1]);
    }
    // storage bool
    function bSwap(bool a, bool b) external returns (bool, bool) {
        delete ba; ba.push(a); ba.push(b);
        (ba[0], ba[1]) = (ba[1], ba[0]);
        return (ba[0], ba[1]);
    }
    // storage 3-way rotate
    function uRot3(uint256 a, uint256 b, uint256 c) external returns (uint256,uint256,uint256) {
        delete ua; ua.push(a); ua.push(b); ua.push(c);
        (ua[0], ua[1], ua[2]) = (ua[2], ua[0], ua[1]);
        return (ua[0], ua[1], ua[2]);
    }
}
