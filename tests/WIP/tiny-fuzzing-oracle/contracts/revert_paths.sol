// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
error TooBig(uint256 v);
error Unauthorized();
error Range(int64 lo, int64 hi);
contract C {
    function reqSimple(uint256 a) external pure returns (uint256) {
        require(a < 100, "too big");
        return a * 2;
    }
    function customErr(uint256 a) external pure returns (uint256) {
        if (a > 50) revert TooBig(a);
        return a + 1;
    }
    function customNoArg(bool ok) external pure returns (uint256) {
        if (!ok) revert Unauthorized();
        return 42;
    }
    function customMulti(int64 x) external pure returns (int64) {
        if (x < -10 || x > 10) revert Range(-10, 10);
        return x * x;
    }
    function reqNoMsg(uint256 a) external pure returns (uint256) {
        require(a != 0);
        return 1000 / a;
    }
    function assertPath(uint256 a) external pure returns (uint256) {
        assert(a != 7);
        return a;
    }
    function nestedReq(uint256 a, uint256 b) external pure returns (uint256) {
        require(a > b, "a<=b");
        unchecked { return a - b; }
    }
}
