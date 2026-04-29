// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/utils/SafeTransferLib.sol";

contract SafeTransferWrapper {
    function safeTransferETH(address to, uint256 amount) external {
        SafeTransferLib.safeTransferETH(to, amount);
    }

    function balanceOf(address account) external view returns (uint256) {
        return SafeTransferLib.balanceOf(address(0), account);
    }
}
