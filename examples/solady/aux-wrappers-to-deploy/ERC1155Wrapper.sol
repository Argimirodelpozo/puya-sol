// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/tokens/ERC1155.sol";

contract ERC1155Wrapper is ERC1155 {
    function uri(uint256) public pure override returns (string memory) {
        return "";
    }

    function mint(address to, uint256 id, uint256 amount, bytes calldata data) external {
        _mint(to, id, amount, data);
    }

    function burn(address from, uint256 id, uint256 amount) external {
        _burn(from, id, amount);
    }

    function getBalance(address account, uint256 id) external view returns (uint256) {
        return balanceOf(account, id);
    }
}
