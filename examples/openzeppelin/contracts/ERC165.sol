// SPDX-License-Identifier: MIT
// Based on OpenZeppelin Contracts v5.0.0 (utils/introspection/ERC165.sol)
// Demonstrates interface ID computation and supportsInterface pattern.

pragma solidity ^0.8.20;

interface IERC165 {
    function supportsInterface(bytes4 interfaceId) external view returns (bool);
}

interface IERC20 {
    function totalSupply() external view returns (uint256);
    function balanceOf(address account) external view returns (uint256);
    function transfer(address to, uint256 amount) external returns (bool);
    function allowance(address owner, address spender) external view returns (uint256);
    function approve(address spender, uint256 amount) external returns (bool);
    function transferFrom(address from, address to, uint256 amount) external returns (bool);
}

interface IERC721 {
    function balanceOf(address owner) external view returns (uint256);
    function ownerOf(uint256 tokenId) external view returns (address);
}

/**
 * @dev Demonstrates ERC165 interface detection and interface ID computation.
 */
contract ERC165Test is IERC165 {
    /// @dev See {IERC165-supportsInterface}.
    function supportsInterface(bytes4 interfaceId) external pure override returns (bool) {
        return interfaceId == type(IERC165).interfaceId;
    }

    /// @dev Returns the interface ID for IERC165.
    function getERC165InterfaceId() external pure returns (bytes4) {
        return type(IERC165).interfaceId;
    }

    /// @dev Returns the interface ID for IERC20.
    function getERC20InterfaceId() external pure returns (bytes4) {
        return type(IERC20).interfaceId;
    }

    /// @dev Returns the interface ID for IERC721.
    function getERC721InterfaceId() external pure returns (bytes4) {
        return type(IERC721).interfaceId;
    }

    /// @dev Check if two interface IDs are equal.
    function interfaceIdsEqual(bytes4 a, bytes4 b) external pure returns (bool) {
        return a == b;
    }
}
