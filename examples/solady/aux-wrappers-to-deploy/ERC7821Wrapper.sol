// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/accounts/ERC7821.sol";

contract ERC7821Wrapper is ERC7821 {
    function ping() external pure returns (uint256) {
        return 1;
    }
}
