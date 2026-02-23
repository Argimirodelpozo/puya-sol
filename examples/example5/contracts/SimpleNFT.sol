// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

import {ERC721} from "@openzeppelin/contracts/token/ERC721/ERC721.sol";
import {Ownable} from "@openzeppelin/contracts/access/Ownable.sol";

/**
 * @title SimpleNFT
 * @dev A minimal ERC721 NFT with owner-only minting and public burn.
 *
 * Exercises key OpenZeppelin patterns:
 *   - ERC721 base: per-token ownership, nested mappings (_operatorApprovals),
 *     approval management, transfer logic
 *   - Ownable: single-owner access control
 *   - ERC165: interface introspection (supportsInterface)
 *
 * Overrides tokenURI, safeTransferFrom(4), _safeMint(3), _safeTransfer(4)
 * to avoid unsupported Solidity features on AVM (inline assembly in Strings.sol,
 * try-catch in ERC721Utils.sol).
 */
contract SimpleNFT is ERC721, Ownable {
    uint256 private _nextTokenId;

    constructor(
        string memory name_,
        string memory symbol_
    ) ERC721(name_, symbol_) Ownable(msg.sender) {}

    /**
     * @dev Mint a new token to `to`. Only callable by the owner.
     * Returns the new token ID.
     */
    function mint(address to) external onlyOwner returns (uint256) {
        uint256 tokenId = _nextTokenId;
        _nextTokenId = tokenId + 1;
        _mint(to, tokenId);
        return tokenId;
    }

    /**
     * @dev Burn a token. Only the token owner can burn.
     */
    function burn(uint256 tokenId) external {
        address tokenOwner = ownerOf(tokenId);
        require(tokenOwner == msg.sender, "SimpleNFT: caller is not owner");
        _burn(tokenId);
    }

    /**
     * @dev Returns the total number of tokens minted (including burned).
     */
    function totalMinted() external view returns (uint256) {
        return _nextTokenId;
    }

    /**
     * @dev Override tokenURI to return empty string — avoids Strings.toString()
     * which uses inline assembly not supported on AVM.
     */
    function tokenURI(uint256 tokenId) public view virtual override returns (string memory) {
        ownerOf(tokenId); // reverts if token doesn't exist
        return "";
    }

    /**
     * @dev Override safeTransferFrom to skip ERC721Utils.checkOnERC721Received
     * which uses try-catch and inline assembly not supported on AVM.
     */
    function safeTransferFrom(address from, address to, uint256 tokenId, bytes memory /*data*/) public virtual override {
        transferFrom(from, to, tokenId);
    }

    /**
     * @dev Override _safeMint to skip the receiver check.
     */
    function _safeMint(address to, uint256 tokenId, bytes memory /*data*/) internal virtual override {
        _mint(to, tokenId);
    }

    /**
     * @dev Override _safeTransfer to skip the receiver check.
     */
    function _safeTransfer(address from, address to, uint256 tokenId, bytes memory /*data*/) internal virtual override {
        _transfer(from, to, tokenId);
    }
}
