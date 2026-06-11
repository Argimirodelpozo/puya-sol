// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

contract C {
    // arr[i++]++ : EVM evaluates i++ ONCE (i: 0->1), increments arr[0].
    // Expected: arr=[11,20,30], i=1.
    function memArr() external pure returns (uint256, uint256, uint256, uint256) {
        uint256[3] memory arr;
        arr[0] = 10; arr[1] = 20; arr[2] = 30;
        uint256 i = 0;
        arr[i++]++;
        return (arr[0], arr[1], arr[2], i);
    }
    // post-dec index variant: arr[j--]++ with j=2 -> arr[2]=31, j=1
    function memArrDec() external pure returns (uint256, uint256, uint256, uint256) {
        uint256[3] memory arr;
        arr[0] = 10; arr[1] = 20; arr[2] = 30;
        uint256 j = 2;
        arr[j--]++;
        return (arr[0], arr[1], arr[2], j);
    }
}
