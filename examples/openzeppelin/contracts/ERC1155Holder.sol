// SPDX-License-Identifier: MIT
// OpenZeppelin Contracts (last updated v5.0.0)
// Flattened: IERC165 + ERC165 + IERC1155Receiver + ERC1155Holder
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

// --- IERC1155Receiver.sol (token/ERC1155/IERC1155Receiver.sol) ---

interface IERC1155Receiver is IERC165 {
    function onERC1155Received(
        address operator,
        address from,
        uint256 id,
        uint256 value,
        bytes calldata data
    ) external returns (bytes4);

    function onERC1155BatchReceived(
        address operator,
        address from,
        uint256[] calldata ids,
        uint256[] calldata values,
        bytes calldata data
    ) external returns (bytes4);
}

// --- ERC1155Holder.sol (token/ERC1155/utils/ERC1155Holder.sol) ---

abstract contract ERC1155Holder is ERC165, IERC1155Receiver {
    function supportsInterface(bytes4 interfaceId) public view virtual override(ERC165, IERC165) returns (bool) {
        return interfaceId == type(IERC1155Receiver).interfaceId || super.supportsInterface(interfaceId);
    }

    function onERC1155Received(
        address,
        address,
        uint256,
        uint256,
        bytes memory
    ) public virtual override returns (bytes4) {
        return 0xf23a6e61;
    }

    function onERC1155BatchReceived(
        address,
        address,
        uint256[] memory,
        uint256[] memory,
        bytes memory
    ) public virtual override returns (bytes4) {
        return 0xbc197c81;
    }
}

// --- Test contract ---

contract ERC1155HolderTest is ERC1155Holder {
    function testOnERC1155Received(address operator, address from, uint256 id, uint256 value, bytes calldata data) external returns (bytes4) {
        return onERC1155Received(operator, from, id, value, data);
    }
}
