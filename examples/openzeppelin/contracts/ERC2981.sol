// SPDX-License-Identifier: MIT
// OpenZeppelin Contracts (last updated v5.0.0)
// Flattened: IERC165 + ERC165 + IERC2981 + ERC2981
// Source: https://github.com/OpenZeppelin/openzeppelin-contracts/blob/v5.0.0/contracts/
// UNMODIFIED — exact copies with imports resolved

pragma solidity ^0.8.20;

// --- IERC165.sol (utils/introspection/IERC165.sol) ---

interface IERC165 {
    function supportsInterface(bytes4 interfaceId) external view returns (bool);
}

// --- ERC165.sol (utils/introspection/ERC165.sol) ---

abstract contract ERC165 is IERC165 {
    function supportsInterface(bytes4 interfaceId) public view virtual returns (bool) {
        return interfaceId == type(IERC165).interfaceId;
    }
}

// --- IERC2981.sol (interfaces/IERC2981.sol) ---

interface IERC2981 is IERC165 {
    function royaltyInfo(
        uint256 tokenId,
        uint256 salePrice
    ) external view returns (address receiver, uint256 royaltyAmount);
}

// --- ERC2981.sol (token/common/ERC2981.sol) ---

abstract contract ERC2981 is IERC2981, ERC165 {
    struct RoyaltyInfo {
        address receiver;
        uint96 royaltyFraction;
    }

    RoyaltyInfo private _defaultRoyaltyInfo;
    mapping(uint256 tokenId => RoyaltyInfo) private _tokenRoyaltyInfo;

    error ERC2981InvalidDefaultRoyalty(uint256 numerator, uint256 denominator);
    error ERC2981InvalidDefaultRoyaltyReceiver(address receiver);
    error ERC2981InvalidTokenRoyalty(uint256 tokenId, uint256 numerator, uint256 denominator);
    error ERC2981InvalidTokenRoyaltyReceiver(uint256 tokenId, address receiver);

    function supportsInterface(bytes4 interfaceId) public view virtual override(IERC165, ERC165) returns (bool) {
        return interfaceId == type(IERC2981).interfaceId || super.supportsInterface(interfaceId);
    }

    function royaltyInfo(uint256 tokenId, uint256 salePrice) public view virtual returns (address, uint256) {
        RoyaltyInfo memory royalty = _tokenRoyaltyInfo[tokenId];

        if (royalty.receiver == address(0)) {
            royalty = _defaultRoyaltyInfo;
        }

        uint256 royaltyAmount = (salePrice * royalty.royaltyFraction) / _feeDenominator();

        return (royalty.receiver, royaltyAmount);
    }

    function _feeDenominator() internal pure virtual returns (uint96) {
        return 10000;
    }

    function _setDefaultRoyalty(address receiver, uint96 feeNumerator) internal virtual {
        uint256 denominator = _feeDenominator();
        if (feeNumerator > denominator) {
            revert ERC2981InvalidDefaultRoyalty(feeNumerator, denominator);
        }
        if (receiver == address(0)) {
            revert ERC2981InvalidDefaultRoyaltyReceiver(address(0));
        }

        _defaultRoyaltyInfo = RoyaltyInfo(receiver, feeNumerator);
    }

    function _deleteDefaultRoyalty() internal virtual {
        delete _defaultRoyaltyInfo;
    }

    function _setTokenRoyalty(uint256 tokenId, address receiver, uint96 feeNumerator) internal virtual {
        uint256 denominator = _feeDenominator();
        if (feeNumerator > denominator) {
            revert ERC2981InvalidTokenRoyalty(tokenId, feeNumerator, denominator);
        }
        if (receiver == address(0)) {
            revert ERC2981InvalidTokenRoyaltyReceiver(tokenId, address(0));
        }

        _tokenRoyaltyInfo[tokenId] = RoyaltyInfo(receiver, feeNumerator);
    }

    function _resetTokenRoyalty(uint256 tokenId) internal virtual {
        delete _tokenRoyaltyInfo[tokenId];
    }
}

// Test contract — standard usage (not part of OZ source)
contract ERC2981Test is ERC2981 {
    function setDefaultRoyalty(address receiver, uint96 feeNumerator) external {
        _setDefaultRoyalty(receiver, feeNumerator);
    }

    function deleteDefaultRoyalty() external {
        _deleteDefaultRoyalty();
    }

    function setTokenRoyalty(uint256 tokenId, address receiver, uint96 feeNumerator) external {
        _setTokenRoyalty(tokenId, receiver, feeNumerator);
    }

    function resetTokenRoyalty(uint256 tokenId) external {
        _resetTokenRoyalty(tokenId);
    }
}
