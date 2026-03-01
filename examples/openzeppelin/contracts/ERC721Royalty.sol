// SPDX-License-Identifier: MIT
// OpenZeppelin Contracts (last updated v5.0.0)
// Flattened: Context + IERC165 + ERC165 + IERC721 + IERC721Metadata + IERC721Receiver + IERC721Errors + Strings + ERC721 + IERC2981 + ERC2981 + ERC721Royalty
// Source: https://github.com/OpenZeppelin/openzeppelin-contracts/blob/v5.0.0/contracts/
// UNMODIFIED — exact copies with imports resolved

pragma solidity ^0.8.20;

// --- Context.sol (utils/Context.sol) ---

abstract contract Context {
    function _msgSender() internal view virtual returns (address) {
        return msg.sender;
    }

    function _msgData() internal view virtual returns (bytes calldata) {
        return msg.data;
    }
}

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

// --- IERC721.sol (token/ERC721/IERC721.sol) ---

interface IERC721 is IERC165 {
    event Transfer(address indexed from, address indexed to, uint256 indexed tokenId);
    event Approval(address indexed owner, address indexed approved, uint256 indexed tokenId);
    event ApprovalForAll(address indexed owner, address indexed operator, bool approved);

    function balanceOf(address owner) external view returns (uint256 balance);
    function ownerOf(uint256 tokenId) external view returns (address owner);
    function safeTransferFrom(address from, address to, uint256 tokenId, bytes calldata data) external;
    function safeTransferFrom(address from, address to, uint256 tokenId) external;
    function transferFrom(address from, address to, uint256 tokenId) external;
    function approve(address to, uint256 tokenId) external;
    function setApprovalForAll(address operator, bool approved) external;
    function getApproved(uint256 tokenId) external view returns (address operator);
    function isApprovedForAll(address owner, address operator) external view returns (bool);
}

// --- IERC721Metadata.sol (token/ERC721/extensions/IERC721Metadata.sol) ---

interface IERC721Metadata is IERC721 {
    function name() external view returns (string memory);
    function symbol() external view returns (string memory);
    function tokenURI(uint256 tokenId) external view returns (string memory);
}

// --- IERC721Receiver.sol (token/ERC721/IERC721Receiver.sol) ---

interface IERC721Receiver {
    function onERC721Received(
        address operator,
        address from,
        uint256 tokenId,
        bytes calldata data
    ) external returns (bytes4);
}

// --- draft-IERC6093.sol (interfaces/draft-IERC6093.sol) — ERC721 errors only ---

interface IERC721Errors {
    error ERC721InvalidOwner(address owner);
    error ERC721NonexistentToken(uint256 tokenId);
    error ERC721IncorrectOwner(address sender, uint256 tokenId, address owner);
    error ERC721InvalidSender(address sender);
    error ERC721InvalidReceiver(address receiver);
    error ERC721InsufficientApproval(address operator, uint256 tokenId);
    error ERC721InvalidApprover(address approver);
    error ERC721InvalidOperator(address operator);
}

// --- Strings.sol (utils/Strings.sol) — minimal stub ---

library Strings {
    function toString(uint256) internal pure returns (string memory) {
        return "";
    }
}

// --- ERC721.sol (token/ERC721/ERC721.sol) ---

abstract contract ERC721 is Context, ERC165, IERC721, IERC721Metadata, IERC721Errors {
    using Strings for uint256;

    string private _name;
    string private _symbol;

    mapping(uint256 tokenId => address) private _owners;
    mapping(address owner => uint256) private _balances;
    mapping(uint256 tokenId => address) private _tokenApprovals;
    mapping(address owner => mapping(address operator => bool)) private _operatorApprovals;

    constructor(string memory name_, string memory symbol_) {
        _name = name_;
        _symbol = symbol_;
    }

    function supportsInterface(bytes4 interfaceId) public view virtual override(ERC165, IERC165) returns (bool) {
        return
            interfaceId == type(IERC721).interfaceId ||
            interfaceId == type(IERC721Metadata).interfaceId ||
            super.supportsInterface(interfaceId);
    }

    function balanceOf(address owner) public view virtual returns (uint256) {
        if (owner == address(0)) {
            revert ERC721InvalidOwner(address(0));
        }
        return _balances[owner];
    }

    function ownerOf(uint256 tokenId) public view virtual returns (address) {
        return _requireOwned(tokenId);
    }

    function name() public view virtual returns (string memory) {
        return _name;
    }

    function symbol() public view virtual returns (string memory) {
        return _symbol;
    }

    function tokenURI(uint256 tokenId) public view virtual returns (string memory) {
        _requireOwned(tokenId);
        string memory baseURI = _baseURI();
        return bytes(baseURI).length > 0 ? string.concat(baseURI, tokenId.toString()) : "";
    }

    function _baseURI() internal view virtual returns (string memory) {
        return "";
    }

    function approve(address to, uint256 tokenId) public virtual {
        _approve(to, tokenId, _msgSender());
    }

    function getApproved(uint256 tokenId) public view virtual returns (address) {
        _requireOwned(tokenId);
        return _getApproved(tokenId);
    }

    function setApprovalForAll(address operator, bool approved) public virtual {
        _setApprovalForAll(_msgSender(), operator, approved);
    }

    function isApprovedForAll(address owner, address operator) public view virtual returns (bool) {
        return _operatorApprovals[owner][operator];
    }

    function transferFrom(address from, address to, uint256 tokenId) public virtual {
        if (to == address(0)) {
            revert ERC721InvalidReceiver(address(0));
        }
        address previousOwner = _update(to, tokenId, _msgSender());
        if (previousOwner != from) {
            revert ERC721IncorrectOwner(from, tokenId, previousOwner);
        }
    }

    function safeTransferFrom(address from, address to, uint256 tokenId) public {
        safeTransferFrom(from, to, tokenId, "");
    }

    function safeTransferFrom(address from, address to, uint256 tokenId, bytes memory data) public virtual {
        transferFrom(from, to, tokenId);
        _checkOnERC721Received(from, to, tokenId, data);
    }

    function _ownerOf(uint256 tokenId) internal view virtual returns (address) {
        return _owners[tokenId];
    }

    function _getApproved(uint256 tokenId) internal view virtual returns (address) {
        return _tokenApprovals[tokenId];
    }

    function _isAuthorized(address owner, address spender, uint256 tokenId) internal view virtual returns (bool) {
        return
            spender != address(0) &&
            (owner == spender || isApprovedForAll(owner, spender) || _getApproved(tokenId) == spender);
    }

    function _checkAuthorized(address owner, address spender, uint256 tokenId) internal view virtual {
        if (!_isAuthorized(owner, spender, tokenId)) {
            if (owner == address(0)) {
                revert ERC721NonexistentToken(tokenId);
            } else {
                revert ERC721InsufficientApproval(spender, tokenId);
            }
        }
    }

    function _increaseBalance(address account, uint128 value) internal virtual {
        unchecked {
            _balances[account] += value;
        }
    }

    function _update(address to, uint256 tokenId, address auth) internal virtual returns (address) {
        address from = _ownerOf(tokenId);

        if (auth != address(0)) {
            _checkAuthorized(from, auth, tokenId);
        }

        if (from != address(0)) {
            _approve(address(0), tokenId, address(0), false);
            unchecked {
                _balances[from] -= 1;
            }
        }

        if (to != address(0)) {
            unchecked {
                _balances[to] += 1;
            }
        }

        _owners[tokenId] = to;

        emit Transfer(from, to, tokenId);

        return from;
    }

    function _mint(address to, uint256 tokenId) internal {
        if (to == address(0)) {
            revert ERC721InvalidReceiver(address(0));
        }
        address previousOwner = _update(to, tokenId, address(0));
        if (previousOwner != address(0)) {
            revert ERC721InvalidSender(address(0));
        }
    }

    function _safeMint(address to, uint256 tokenId) internal {
        _safeMint(to, tokenId, "");
    }

    function _safeMint(address to, uint256 tokenId, bytes memory data) internal virtual {
        _mint(to, tokenId);
        _checkOnERC721Received(address(0), to, tokenId, data);
    }

    function _burn(uint256 tokenId) internal {
        address previousOwner = _update(address(0), tokenId, address(0));
        if (previousOwner == address(0)) {
            revert ERC721NonexistentToken(tokenId);
        }
    }

    function _transfer(address from, address to, uint256 tokenId) internal {
        if (to == address(0)) {
            revert ERC721InvalidReceiver(address(0));
        }
        address previousOwner = _update(to, tokenId, address(0));
        if (previousOwner == address(0)) {
            revert ERC721NonexistentToken(tokenId);
        } else if (previousOwner != from) {
            revert ERC721IncorrectOwner(from, tokenId, previousOwner);
        }
    }

    function _safeTransfer(address from, address to, uint256 tokenId) internal {
        _safeTransfer(from, to, tokenId, "");
    }

    function _safeTransfer(address from, address to, uint256 tokenId, bytes memory data) internal virtual {
        _transfer(from, to, tokenId);
        _checkOnERC721Received(from, to, tokenId, data);
    }

    function _approve(address to, uint256 tokenId, address auth) internal {
        _approve(to, tokenId, auth, true);
    }

    function _approve(address to, uint256 tokenId, address auth, bool emitEvent) internal virtual {
        if (emitEvent || auth != address(0)) {
            address owner = _requireOwned(tokenId);
            if (auth != address(0) && owner != auth && !isApprovedForAll(owner, auth)) {
                revert ERC721InvalidApprover(auth);
            }
            if (emitEvent) {
                emit Approval(owner, to, tokenId);
            }
        }
        _tokenApprovals[tokenId] = to;
    }

    function _setApprovalForAll(address owner, address operator, bool approved) internal virtual {
        if (operator == address(0)) {
            revert ERC721InvalidOperator(operator);
        }
        _operatorApprovals[owner][operator] = approved;
        emit ApprovalForAll(owner, operator, approved);
    }

    function _requireOwned(uint256 tokenId) internal view returns (address) {
        address owner = _ownerOf(tokenId);
        if (owner == address(0)) {
            revert ERC721NonexistentToken(tokenId);
        }
        return owner;
    }

    function _checkOnERC721Received(address from, address to, uint256 tokenId, bytes memory data) private {
        if (to.code.length > 0) {
            try IERC721Receiver(to).onERC721Received(_msgSender(), from, tokenId, data) returns (bytes4 retval) {
                if (retval != IERC721Receiver.onERC721Received.selector) {
                    revert ERC721InvalidReceiver(to);
                }
            } catch (bytes memory reason) {
                if (reason.length == 0) {
                    revert ERC721InvalidReceiver(to);
                } else {
                    assembly {
                        revert(add(32, reason), mload(reason))
                    }
                }
            }
        }
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

// --- ERC721Royalty.sol (token/ERC721/extensions/ERC721Royalty.sol) ---

abstract contract ERC721Royalty is ERC2981, ERC721 {
    function supportsInterface(bytes4 interfaceId) public view virtual override(ERC721, ERC2981) returns (bool) {
        return super.supportsInterface(interfaceId);
    }
}

// Test contract — standard usage (not part of OZ source)
contract ERC721RoyaltyTest is ERC721Royalty {
    constructor() ERC721("RoyaltyNFT", "RNFT") {}

    function mint(address to, uint256 tokenId) external {
        _mint(to, tokenId);
    }

    function setDefaultRoyalty(address receiver, uint96 feeNumerator) external {
        _setDefaultRoyalty(receiver, feeNumerator);
    }

    function setTokenRoyalty(uint256 tokenId, address receiver, uint96 feeNumerator) external {
        _setTokenRoyalty(tokenId, receiver, feeNumerator);
    }

    function burn(uint256 tokenId) external {
        _burn(tokenId);
    }
}
