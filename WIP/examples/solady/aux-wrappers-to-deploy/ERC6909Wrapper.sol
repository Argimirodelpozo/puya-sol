// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/tokens/ERC6909.sol";

contract ERC6909Wrapper is ERC6909 {
    function name(uint256) public pure override returns (string memory) {
        return "Token";
    }

    function symbol(uint256) public pure override returns (string memory) {
        return "TKN";
    }

    function tokenURI(uint256) public pure override returns (string memory) {
        return "";
    }

    function decimals(uint256) public pure override returns (uint8) {
        return 18;
    }

    function mint(address to, uint256 id, uint256 amount) external {
        _mint(to, id, amount);
    }

    function burn(address from, uint256 id, uint256 amount) external {
        _burn(from, id, amount);
    }

    function getBalance(address owner_, uint256 id) external view returns (uint256) {
        return balanceOf(owner_, id);
    }
}
