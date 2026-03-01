// SPDX-License-Identifier: MIT
// OpenZeppelin Contracts (last updated v5.0.0)
// Flattened: IERC165 + ERC165 + IERC721Receiver + ERC721Holder
// Source: https://github.com/OpenZeppelin/openzeppelin-contracts/blob/v5.0.0/contracts/
// MODIFIED — selector literals replace .selector expressions

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

// --- IERC721Receiver.sol (token/ERC721/IERC721Receiver.sol) ---

interface IERC721Receiver {
    function onERC721Received(
        address operator,
        address from,
        uint256 tokenId,
        bytes calldata data
    ) external returns (bytes4);
}

// --- ERC721Holder.sol (token/ERC721/utils/ERC721Holder.sol) ---

abstract contract ERC721Holder is IERC721Receiver {
    function onERC721Received(address, address, uint256, bytes memory) public virtual returns (bytes4) {
        return 0x150b7a02;
    }
}

// --- Test contract ---

contract ERC721HolderTest is ERC165, ERC721Holder {
    function supportsInterface(bytes4 interfaceId) public view virtual override returns (bool) {
        return interfaceId == type(IERC721Receiver).interfaceId || super.supportsInterface(interfaceId);
    }

    function testOnERC721Received(address operator, address from, uint256 tokenId, bytes calldata data) external returns (bytes4) {
        return onERC721Received(operator, from, tokenId, data);
    }
}
