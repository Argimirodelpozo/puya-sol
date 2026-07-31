// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
contract Control {
    function fib(uint256 n) external pure returns (uint256) {
        if (n < 2) return n;
        return _fib(n);
    }
    function _fib(uint256 n) internal pure returns (uint256) {
        if (n < 2) return n; return _fib(n-1) + _fib(n-2);
    }
    function ackermann(uint256 m, uint256 n) external pure returns (uint256) { return _ack(m % 3, n % 4); }
    function _ack(uint256 m, uint256 n) internal pure returns (uint256) {
        if (m == 0) return n + 1;
        if (n == 0) return _ack(m-1, 1);
        return _ack(m-1, _ack(m, n-1));
    }
    function nestedLoop(uint256 a, uint256 b) external pure returns (uint256 s) {
        for (uint256 i=0;i<a%10;i++) {
            for (uint256 j=0;j<b%10;j++) {
                if (j == 5) continue;
                if (i*j > 30) break;
                s += i*j;
            }
        }
    }
    function whileSum(uint256 n) external pure returns (uint256 s) {
        uint256 i = n % 50; while (i > 0) { s += i; i--; }
    }
}
