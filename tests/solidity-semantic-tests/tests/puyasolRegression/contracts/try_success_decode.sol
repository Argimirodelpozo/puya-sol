// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract TryCallee {
    function multi() external pure returns (uint256, string memory) {
        return (7, "ok");
    }
    function echoArr(uint16[] memory a) external pure returns (uint16[] memory) {
        return a;
    }
}

contract TryProbe {
    TryCallee public c;
    uint256 public sideEffect;

    constructor() { c = new TryCallee(); }

    // success-path decode: multi-return destructure into try clause params
    function t1() public returns (uint256 a, string memory s) {
        sideEffect = 1;
        try c.multi() returns (uint256 x, string memory y) {
            sideEffect = 2;
            return (x, y);
        } catch {
            return (0, "caught");
        }
    }

    // try around contract CREATION, success path
    function t2() public returns (uint256) {
        try new TryCallee() returns (TryCallee nc) {
            return nc.multi.selector == TryCallee.multi.selector ? 1 : 9;
        } catch {
            return 2;
        }
    }

    // dynamic-array return decode through try
    function t3() public returns (uint256) {
        uint16[] memory a = new uint16[](3);
        a[0] = 5; a[1] = 6; a[2] = 300;
        try c.echoArr(a) returns (uint16[] memory got) {
            return uint256(got[0]) + got[1] + got[2];
        } catch {
            return 0;
        }
    }
}
