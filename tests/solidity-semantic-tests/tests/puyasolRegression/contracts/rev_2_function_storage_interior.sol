// SPDX-License-Identifier: MIT
pragma solidity >=0.8.28;

contract FunctionStorageInterior {
    function() internal[][] rows;
    function append(function() internal[] storage target) internal { target.push(); }
    function nested() external { rows.push(); append(rows[0]); }
}
