// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
contract BoolIsolate {
    bool[] sba;
    // A: isolate PUSH — no element write at all
    function pushRead(bool a, bool b) external returns (bool,bool) {
        delete sba; sba.push(a); sba.push(b);
        return (sba[0], sba[1]);
    }
    // B: isolate WRITE — push both true, then write elem0=false; elem1 must stay true
    function writeElem0(bool x) external returns (bool,bool) {
        delete sba; sba.push(true); sba.push(true);
        sba[0] = x;
        return (sba[0], sba[1]);
    }
    // C: write elem1, elem0 must stay
    function writeElem1(bool x) external returns (bool,bool) {
        delete sba; sba.push(true); sba.push(true);
        sba[1] = x;
        return (sba[0], sba[1]);
    }
}
