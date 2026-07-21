// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// CUSTOM regression fixture (NOT vendored / not an original Solidity semantic test).
// Guards `new Child()` inside a constructor: the child's create/fund/__postInit
// inner-txn chain must complete before later parent-ctor statements call into
// the child (was a documented known-gap, since closed by the ctor/postInit
// sequencing fixes — this pins it).
contract ChildNC {
    mapping(uint256 => uint256) public m;
    uint256 public plain = 77;
    uint256 public fromArg;

    constructor(uint256 a) {
        fromArg = a;
        m[5] = a * 10;
    }
}

contract ParentNC {
    uint256[] parr; // box-backed: the parent itself defers its ctor to __postInit
    uint256 public got;
    uint256 public gotPlain;
    uint256 public gotArg;

    constructor() {
        parr.push(1);
        ChildNC c = new ChildNC(50);
        got = c.m(5);
        gotPlain = c.plain();
        gotArg = c.fromArg();
        parr.push(got);
    }

    function parrLen() external view returns (uint256) { return parr.length; }
    function parr1() external view returns (uint256) { return parr[1]; }
}
