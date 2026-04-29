// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/tokens/ERC20.sol";

contract ERC20Wrapper is ERC20 {
    function name() public pure override returns (string memory) {
        return "TestToken";
    }

    function symbol() public pure override returns (string memory) {
        return "TT";
    }

    function mint(address to, uint256 amount) external {
        _mint(to, amount);
    }

    function burn(address from, uint256 amount) external {
        _burn(from, amount);
    }

    function getBalance(address account) external view returns (uint256) {
        return balanceOf(account);
    }

    function getTotal() external view returns (uint256) {
        return totalSupply();
    }

    function doTransfer(address from, address to, uint256 amount) external {
        _transfer(from, to, amount);
    }

    function doApprove(address owner_, address spender, uint256 amount) external {
        _approve(owner_, spender, amount);
    }

    function getAllowance(address owner_, address spender) external view returns (uint256) {
        return allowance(owner_, spender);
    }
}
