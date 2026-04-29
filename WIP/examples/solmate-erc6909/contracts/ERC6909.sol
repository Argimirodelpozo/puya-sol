// SPDX-License-Identifier: MIT
// Source: https://github.com/transmissions11/solmate/blob/main/src/tokens/ERC6909.sol
// Tests: https://github.com/transmissions11/solmate/blob/main/src/test/ERC6909.t.sol
//
// Modifications for AVM compatibility:
// 1. Changed from abstract to concrete contract (added public mint/burn)
// 2. Added explicit getter functions (AVM doesn't auto-generate getters for public state vars)
// 3. supportsInterface takes uint256 instead of bytes4 (simplified for AVM ABI)
//
// Original logic (transfer, transferFrom, approve, setOperator, infinite approval,
// operator bypass) is UNCHANGED from the solmate source.
pragma solidity >=0.8.0;

/// @notice Minimalist and gas efficient standard ERC6909 implementation.
/// @author Solmate (https://github.com/transmissions11/solmate/blob/main/src/tokens/ERC6909.sol)
contract ERC6909 {
    /*//////////////////////////////////////////////////////////////
                                 EVENTS
    //////////////////////////////////////////////////////////////*/

    event OperatorSet(address indexed owner, address indexed operator, bool approved);

    event Approval(address indexed owner, address indexed spender, uint256 indexed id, uint256 amount);

    event Transfer(address caller, address indexed from, address indexed to, uint256 indexed id, uint256 amount);

    /*//////////////////////////////////////////////////////////////
                             ERC6909 STORAGE
    //////////////////////////////////////////////////////////////*/

    mapping(address => mapping(address => bool)) internal _isOperator;

    mapping(address => mapping(uint256 => uint256)) internal _balanceOf;

    mapping(address => mapping(address => mapping(uint256 => uint256))) internal _allowance;

    /*//////////////////////////////////////////////////////////////
                              ERC6909 LOGIC
    //////////////////////////////////////////////////////////////*/

    function transfer(
        address receiver,
        uint256 id,
        uint256 amount
    ) public virtual returns (bool) {
        _balanceOf[msg.sender][id] -= amount;

        _balanceOf[receiver][id] += amount;

        emit Transfer(msg.sender, msg.sender, receiver, id, amount);

        return true;
    }

    function transferFrom(
        address sender,
        address receiver,
        uint256 id,
        uint256 amount
    ) public virtual returns (bool) {
        if (msg.sender != sender && !_isOperator[sender][msg.sender]) {
            uint256 allowed = _allowance[sender][msg.sender][id];
            if (allowed != type(uint256).max) _allowance[sender][msg.sender][id] = allowed - amount;
        }

        _balanceOf[sender][id] -= amount;

        _balanceOf[receiver][id] += amount;

        emit Transfer(msg.sender, sender, receiver, id, amount);

        return true;
    }

    function approve(
        address spender,
        uint256 id,
        uint256 amount
    ) public virtual returns (bool) {
        _allowance[msg.sender][spender][id] = amount;

        emit Approval(msg.sender, spender, id, amount);

        return true;
    }

    function setOperator(address operator, bool approved) public virtual returns (bool) {
        _isOperator[msg.sender][operator] = approved;

        emit OperatorSet(msg.sender, operator, approved);

        return true;
    }

    /*//////////////////////////////////////////////////////////////
                            EXPLICIT GETTERS
    //////////////////////////////////////////////////////////////*/

    function isOperator(address owner, address operator) external view returns (bool) {
        return _isOperator[owner][operator];
    }

    function balanceOf(address owner, uint256 id) external view returns (uint256) {
        return _balanceOf[owner][id];
    }

    function allowance(address owner, address spender, uint256 id) external view returns (uint256) {
        return _allowance[owner][spender][id];
    }

    /*//////////////////////////////////////////////////////////////
                        PUBLIC MINT/BURN LOGIC
    //////////////////////////////////////////////////////////////*/

    function mint(
        address receiver,
        uint256 id,
        uint256 amount
    ) external {
        _balanceOf[receiver][id] += amount;

        emit Transfer(msg.sender, address(0), receiver, id, amount);
    }

    function burn(
        address sender,
        uint256 id,
        uint256 amount
    ) external {
        _balanceOf[sender][id] -= amount;

        emit Transfer(msg.sender, sender, address(0), id, amount);
    }
}
