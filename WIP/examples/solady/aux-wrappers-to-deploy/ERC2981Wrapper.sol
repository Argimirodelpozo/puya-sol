// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/tokens/ERC2981.sol";

contract ERC2981Wrapper is ERC2981 {
    function supportsInterface(bytes4 interfaceId) public view virtual override returns (bool) {
        return super.supportsInterface(interfaceId);
    }

    function setDefaultRoyalty(address receiver, uint96 feeNumerator) external {
        _setDefaultRoyalty(receiver, feeNumerator);
    }

    function setTokenRoyalty(uint256 tokenId, address receiver, uint96 feeNumerator) external {
        _setTokenRoyalty(tokenId, receiver, feeNumerator);
    }

    function getRoyalty(uint256 tokenId, uint256 salePrice) external view returns (address, uint256) {
        return royaltyInfo(tokenId, salePrice);
    }
}
