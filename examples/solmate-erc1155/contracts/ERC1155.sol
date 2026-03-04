// SPDX-License-Identifier: AGPL-3.0-only
pragma solidity ^0.8.0;

/// @notice Minimalist and gas efficient standard ERC1155 implementation.
/// @author Solmate (https://github.com/transmissions11/solmate/blob/main/src/tokens/ERC1155.sol)
/// @dev Modifications for AVM: removed batch operations (dynamic array params),
///      removed safe transfer callbacks (cross-contract), removed uri/supportsInterface (string returns),
///      made concrete with public mint/burn, added explicit getter functions.
contract ERC1155 {
    /*//////////////////////////////////////////////////////////////
                                 EVENTS
    //////////////////////////////////////////////////////////////*/

    event TransferSingle(
        address indexed operator,
        address indexed from,
        address indexed to,
        uint256 id,
        uint256 amount
    );

    event ApprovalForAll(address indexed owner, address indexed operator, bool approved);

    /*//////////////////////////////////////////////////////////////
                             ERC1155 STORAGE
    //////////////////////////////////////////////////////////////*/

    mapping(address => mapping(uint256 => uint256)) internal _balanceOf;

    mapping(address => mapping(address => bool)) internal _isApprovedForAll;

    /*//////////////////////////////////////////////////////////////
                            EXPLICIT GETTERS
    //////////////////////////////////////////////////////////////*/

    function balanceOf(address owner, uint256 id) public view returns (uint256) {
        return _balanceOf[owner][id];
    }

    function isApprovedForAll(address owner, address operator) public view returns (bool) {
        return _isApprovedForAll[owner][operator];
    }

    /*//////////////////////////////////////////////////////////////
                              ERC1155 LOGIC
    //////////////////////////////////////////////////////////////*/

    function setApprovalForAll(address operator, bool approved) public virtual {
        _isApprovedForAll[msg.sender][operator] = approved;

        emit ApprovalForAll(msg.sender, operator, approved);
    }

    function safeTransferFrom(
        address from,
        address to,
        uint256 id,
        uint256 amount
    ) public virtual {
        require(msg.sender == from || _isApprovedForAll[from][msg.sender], "NOT_AUTHORIZED");

        require(to != address(0), "INVALID_RECIPIENT");

        _balanceOf[from][id] -= amount;
        _balanceOf[to][id] += amount;

        emit TransferSingle(msg.sender, from, to, id, amount);
    }

    /*//////////////////////////////////////////////////////////////
                        PUBLIC MINT/BURN LOGIC
    //////////////////////////////////////////////////////////////*/

    function mint(
        address to,
        uint256 id,
        uint256 amount
    ) public virtual {
        require(to != address(0), "INVALID_RECIPIENT");

        _balanceOf[to][id] += amount;

        emit TransferSingle(msg.sender, address(0), to, id, amount);
    }

    function burn(
        address from,
        uint256 id,
        uint256 amount
    ) public virtual {
        _balanceOf[from][id] -= amount;

        emit TransferSingle(msg.sender, from, address(0), id, amount);
    }
}
