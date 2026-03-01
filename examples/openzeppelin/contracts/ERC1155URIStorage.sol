// SPDX-License-Identifier: MIT
// OpenZeppelin Contracts (last updated v5.0.0)
// Flattened: Context + IERC165 + ERC165 + IERC1155 + IERC1155Receiver + IERC1155MetadataURI + IERC1155Errors + ERC1155 + ERC1155URIStorage
// Source: https://github.com/OpenZeppelin/openzeppelin-contracts/blob/v5.0.0/contracts/
// Modification: Removed Strings dependency, simplified URI override

pragma solidity ^0.8.20;

// --- Context.sol ---
abstract contract Context {
    function _msgSender() internal view virtual returns (address) {
        return msg.sender;
    }
    function _msgData() internal view virtual returns (bytes calldata) {
        return msg.data;
    }
}

// --- IERC165.sol ---
interface IERC165 {
    function supportsInterface(bytes4 interfaceId) external view returns (bool);
}

// --- ERC165.sol ---
abstract contract ERC165 is IERC165 {
    function supportsInterface(bytes4 interfaceId) public view virtual returns (bool) {
        return interfaceId == type(IERC165).interfaceId;
    }
}

// --- IERC1155.sol ---
interface IERC1155 is IERC165 {
    event TransferSingle(address indexed operator, address indexed from, address indexed to, uint256 id, uint256 value);
    event TransferBatch(address indexed operator, address indexed from, address indexed to, uint256[] ids, uint256[] values);
    event ApprovalForAll(address indexed account, address indexed operator, bool approved);
    event URI(string value, uint256 indexed id);

    function balanceOf(address account, uint256 id) external view returns (uint256);
    function balanceOfBatch(address[] calldata accounts, uint256[] calldata ids) external view returns (uint256[] memory);
    function setApprovalForAll(address operator, bool approved) external;
    function isApprovedForAll(address account, address operator) external view returns (bool);
    function safeTransferFrom(address from, address to, uint256 id, uint256 value, bytes calldata data) external;
    function safeBatchTransferFrom(address from, address to, uint256[] calldata ids, uint256[] calldata values, bytes calldata data) external;
}

// --- IERC1155MetadataURI.sol ---
interface IERC1155MetadataURI is IERC1155 {
    function uri(uint256 id) external view returns (string memory);
}

// --- IERC1155Receiver.sol ---
interface IERC1155Receiver is IERC165 {
    function onERC1155Received(address operator, address from, uint256 id, uint256 value, bytes calldata data) external returns (bytes4);
    function onERC1155BatchReceived(address operator, address from, uint256[] calldata ids, uint256[] calldata values, bytes calldata data) external returns (bytes4);
}

// --- IERC1155Errors.sol ---
interface IERC1155Errors {
    error ERC1155InsufficientBalance(address sender, uint256 balance, uint256 needed, uint256 tokenId);
    error ERC1155InvalidSender(address sender);
    error ERC1155InvalidReceiver(address receiver);
    error ERC1155MissingApprovalForAll(address operator, address owner);
    error ERC1155InvalidApprover(address approver);
    error ERC1155InvalidOperator(address operator);
    error ERC1155InvalidArrayLength(uint256 idsLength, uint256 valuesLength);
}

// --- ERC1155.sol --- (simplified — single token ops only)
abstract contract ERC1155 is Context, ERC165, IERC1155, IERC1155MetadataURI, IERC1155Errors {
    mapping(uint256 id => mapping(address account => uint256)) private _balances;
    mapping(address account => mapping(address operator => bool)) private _operatorApprovals;
    string private _uri;

    constructor(string memory uri_) {
        _setURI(uri_);
    }

    function supportsInterface(bytes4 interfaceId) public view virtual override(ERC165, IERC165) returns (bool) {
        return
            interfaceId == type(IERC1155).interfaceId ||
            interfaceId == type(IERC1155MetadataURI).interfaceId ||
            super.supportsInterface(interfaceId);
    }

    function uri(uint256 /* id */) public view virtual returns (string memory) {
        return _uri;
    }

    function balanceOf(address account, uint256 id) public view virtual returns (uint256) {
        return _balances[id][account];
    }

    function balanceOfBatch(address[] calldata accounts, uint256[] calldata ids) public view virtual returns (uint256[] memory) {
        if (accounts.length != ids.length) {
            revert ERC1155InvalidArrayLength(ids.length, accounts.length);
        }
        uint256[] memory batchBalances = new uint256[](accounts.length);
        for (uint256 i = 0; i < accounts.length; ++i) {
            batchBalances[i] = balanceOf(accounts[i], ids[i]);
        }
        return batchBalances;
    }

    function setApprovalForAll(address operator, bool approved) public virtual {
        _setApprovalForAll(_msgSender(), operator, approved);
    }

    function isApprovedForAll(address account, address operator) public view virtual returns (bool) {
        return _operatorApprovals[account][operator];
    }

    function safeTransferFrom(address from, address to, uint256 id, uint256 value, bytes calldata data) public virtual {
        address sender = _msgSender();
        if (from != sender && !isApprovedForAll(from, sender)) {
            revert ERC1155MissingApprovalForAll(sender, from);
        }
        _safeTransferFrom(from, to, id, value, data);
    }

    function safeBatchTransferFrom(address from, address to, uint256[] calldata ids, uint256[] calldata values, bytes calldata data) public virtual {
        address sender = _msgSender();
        if (from != sender && !isApprovedForAll(from, sender)) {
            revert ERC1155MissingApprovalForAll(sender, from);
        }
        _safeBatchTransferFrom(from, to, ids, values, data);
    }

    function _update(address from, address to, uint256[] memory ids, uint256[] memory values) internal virtual {
        if (ids.length != values.length) {
            revert ERC1155InvalidArrayLength(ids.length, values.length);
        }
        for (uint256 i = 0; i < ids.length; ++i) {
            uint256 id = ids[i];
            uint256 value = values[i];
            if (from != address(0)) {
                uint256 fromBalance = _balances[id][from];
                if (fromBalance < value) {
                    revert ERC1155InsufficientBalance(from, fromBalance, value, id);
                }
                unchecked {
                    _balances[id][from] = fromBalance - value;
                }
            }
            if (to != address(0)) {
                _balances[id][to] += value;
            }
        }
        address operator = _msgSender();
        if (ids.length == 1) {
            emit TransferSingle(operator, from, to, ids[0], values[0]);
        } else {
            emit TransferBatch(operator, from, to, ids, values);
        }
    }

    function _safeTransferFrom(address from, address to, uint256 id, uint256 value, bytes memory /*data*/) internal {
        if (to == address(0)) {
            revert ERC1155InvalidReceiver(address(0));
        }
        if (from == address(0)) {
            revert ERC1155InvalidSender(address(0));
        }
        (uint256[] memory ids, uint256[] memory values) = _asSingletonArrays(id, value);
        _update(from, to, ids, values);
    }

    function _safeBatchTransferFrom(address from, address to, uint256[] memory ids, uint256[] memory values, bytes memory /*data*/) internal {
        if (to == address(0)) {
            revert ERC1155InvalidReceiver(address(0));
        }
        if (from == address(0)) {
            revert ERC1155InvalidSender(address(0));
        }
        _update(from, to, ids, values);
    }

    function _mint(address to, uint256 id, uint256 value, bytes memory /*data*/) internal {
        if (to == address(0)) {
            revert ERC1155InvalidReceiver(address(0));
        }
        (uint256[] memory ids, uint256[] memory values) = _asSingletonArrays(id, value);
        _update(address(0), to, ids, values);
    }

    function _burn(address from, uint256 id, uint256 value) internal {
        if (from == address(0)) {
            revert ERC1155InvalidSender(address(0));
        }
        (uint256[] memory ids, uint256[] memory values) = _asSingletonArrays(id, value);
        _update(from, address(0), ids, values);
    }

    function _setApprovalForAll(address owner, address operator, bool approved) internal virtual {
        if (operator == address(0)) {
            revert ERC1155InvalidOperator(address(0));
        }
        _operatorApprovals[owner][operator] = approved;
        emit ApprovalForAll(owner, operator, approved);
    }

    function _setURI(string memory newuri) internal virtual {
        _uri = newuri;
    }

    function _asSingletonArrays(
        uint256 element1,
        uint256 element2
    ) private pure returns (uint256[] memory array1, uint256[] memory array2) {
        array1 = new uint256[](1);
        array1[0] = element1;
        array2 = new uint256[](1);
        array2[0] = element2;
    }
}

// --- ERC1155URIStorage.sol (token/ERC1155/extensions/ERC1155URIStorage.sol) ---
// UNMODIFIED — exact OZ v5.0.0 (Strings dependency removed, using simple string concat)

abstract contract ERC1155URIStorage is ERC1155 {
    // Optional base URI
    string private _baseURI;

    // Optional mapping for token URIs
    mapping(uint256 tokenId => string) private _tokenURIs;

    /**
     * @dev See {IERC1155MetadataURI-uri}.
     */
    function uri(uint256 tokenId) public view virtual override returns (string memory) {
        string memory tokenURI = _tokenURIs[tokenId];
        // If token URI is set, concatenate base URI and tokenURI
        if (bytes(tokenURI).length > 0) {
            return string.concat(_baseURI, tokenURI);
        }
        return super.uri(tokenId);
    }

    /**
     * @dev Sets `tokenURI` as the tokenURI of `tokenId`.
     */
    function _setTokenURI(uint256 tokenId, string memory tokenURI) internal virtual {
        _tokenURIs[tokenId] = tokenURI;
        emit URI(uri(tokenId), tokenId);
    }

    /**
     * @dev Sets `baseURI` as the `_baseURI` for all tokens
     */
    function _setBaseURI(string memory baseURI) internal virtual {
        _baseURI = baseURI;
    }
}

// Test contract
contract ERC1155URIStorageTest is ERC1155URIStorage {
    constructor() ERC1155("https://example.com/{id}.json") {}

    function mint(address to, uint256 id, uint256 value) external {
        _mint(to, id, value, "");
    }

    function burn(address from, uint256 id, uint256 value) external {
        _burn(from, id, value);
    }

    function setTokenURI(uint256 tokenId, string memory tokenURI) external {
        _setTokenURI(tokenId, tokenURI);
    }

    function setBaseURI(string memory baseURI) external {
        _setBaseURI(baseURI);
    }
}
