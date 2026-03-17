// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/tokens/ERC721.sol";

contract ERC721Wrapper is ERC721 {
    function name() public pure override returns (string memory) {
        return "TestNFT";
    }

    function symbol() public pure override returns (string memory) {
        return "TNFT";
    }

    function tokenURI(uint256) public pure override returns (string memory) {
        return "";
    }

    function mint(address to, uint256 tokenId) external {
        _mint(to, tokenId);
    }

    function burn(uint256 tokenId) external {
        _burn(tokenId);
    }

    function getOwnerOf(uint256 tokenId) external view returns (address) {
        return ownerOf(tokenId);
    }

    function getBalance(address owner_) external view returns (uint256) {
        return balanceOf(owner_);
    }
}
