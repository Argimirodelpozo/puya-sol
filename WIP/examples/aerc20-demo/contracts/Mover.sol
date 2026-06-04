// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

import "tokens/AVM.sol";

// De-risk for V4 "take" with AERC20 currencies: a contract holding AERC20 (an
// ASA-backed ERC20) moving it via an EXTERNAL call (V4's CurrencyLibrary.transfer
// shape). Plus opt-in (the pool app must opt into the ASA to hold it).
interface IERC20Min {
    function balanceOf(address who) external view returns (uint256);
    function transfer(address to, uint256 amount) external returns (bool);
}

contract Mover {
    // opt THIS app account into the ASA so it can hold the token
    function optIn(uint64 asaId) external {
        AVM.asaOptIn(asaId);
    }

    // view external call — tests the call mechanism (no holding needed)
    function checkBalance(address token, address who) external view returns (uint256) {
        return IERC20Min(token).balanceOf(who);
    }

    // write external call — moves AERC20 this contract holds (the "take" shape:
    // AERC20.transfer uses msg.sender = this app account, clawback moves it out)
    function move(address token, address to, uint256 amount) external returns (bool) {
        return IERC20Min(token).transfer(to, amount);
    }
}
