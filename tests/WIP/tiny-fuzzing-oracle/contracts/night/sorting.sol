// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
contract Sorting {
    uint256[] a;
    function seed(uint256 x0, uint256 x1, uint256 x2, uint256 x3, uint256 x4) external {
        delete a; a.push(x0); a.push(x1); a.push(x2); a.push(x3); a.push(x4);
    }
    function bubble() external {
        uint256 n = a.length;
        for (uint256 i = 0; i < n; i++)
            for (uint256 j = 0; j + 1 < n - i; j++)
                if (a[j] > a[j+1]) (a[j], a[j+1]) = (a[j+1], a[j]);   // tuple swap on storage elems
    }
    function insertion() external {
        for (uint256 i = 1; i < a.length; i++) {
            uint256 key = a[i]; uint256 j = i;
            while (j > 0 && a[j-1] > key) { a[j] = a[j-1]; j--; }
            a[j] = key;
        }
    }
    function reverseInPlace() external {
        uint256 n = a.length;
        for (uint256 i = 0; i < n / 2; i++) { uint256 t = a[i]; a[i] = a[n-1-i]; a[n-1-i] = t; }
    }
    function get(uint256 i) external view returns (uint256) { return a[i]; }
    function sum() external view returns (uint256 s) { for (uint256 i=0;i<a.length;i++) s += a[i]; }
}
