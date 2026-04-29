// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/tokens/ERC20Votes.sol";

contract ERC20VotesWrapper is ERC20Votes {
    function name() public pure override returns (string memory) {
        return "Vote Token";
    }

    function symbol() public pure override returns (string memory) {
        return "VOTE";
    }

    function mint(address to, uint256 amount) external {
        _mint(to, amount);
    }

    function burn(address from, uint256 amount) external {
        _burn(from, amount);
    }
}
