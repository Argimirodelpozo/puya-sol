// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract ArrRefClean {
    struct P { uint256 x; uint256 y; }   // never a mapping value → element not box-keyed
    P[] arr;
    function whole() external returns (uint256) {            // pass whole array to a contract method
        delete arr; arr.push(P(0,0));
        _bump(arr);
        return arr[0].x;                                     // EVM 5
    }
    function _bump(P[] storage a) internal { a[0].x = 5; }
}
