// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
contract BoolPackWrite {
    bool[] ba;
    // sequential (non-tuple) swap via temps — isolates packed-bool element writes
    function seqBSwap(bool a, bool b) external returns (bool,bool) {
        delete ba; ba.push(a); ba.push(b);
        bool t0 = ba[1]; bool t1 = ba[0];
        ba[0] = t0;
        ba[1] = t1;
        return (ba[0], ba[1]);
    }
    // two independent writes to distinct packed indices
    function twoWrite(bool a, bool b) external returns (bool,bool) {
        delete ba; ba.push(false); ba.push(false);
        ba[0] = a;
        ba[1] = b;
        return (ba[0], ba[1]);
    }
    // single dynamic-index write, leave sibling untouched
    function oneWrite(bool a, bool b, uint256 i) external returns (bool,bool) {
        delete ba; ba.push(a); ba.push(b);
        ba[i] = true;
        return (ba[0], ba[1]);
    }
}
