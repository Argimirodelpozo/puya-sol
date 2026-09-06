// SPDX-License-Identifier: MIT
pragma solidity >=0.8.28;

contract FunctionStorageInteriorField {
    struct Holder { function() internal[] items; }
    Holder holder;
    function append(function() internal[] storage target) internal { target.push(); }
    function member() external { append(holder.items); }
}
