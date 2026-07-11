// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    function deadDiv(uint256 a, uint256 d) external pure returns (uint256) { return (((d % a) > 0) ? d : d); }
}
