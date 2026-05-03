// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

import "./AVM.sol";

/// @notice Algorand-native ERC20: standard Solidity surface, ASA-backed
/// balances and total supply via clawback inner transactions.
///
/// Inheriting contracts get a normal `is AERC20` look:
///
///     contract MyToken is AERC20 {
///         constructor() AERC20(1_000_000, 6, "My Token", "MTK") {}
///     }
///
/// At deploy, the constructor emits an `acfg` inner transaction creating
/// the underlying ASA with the contract as manager / reserve / clawback /
/// freeze. `transfer` and `transferFrom` move balances via clawback
/// `axfer` inner transactions; recipients must have opted in to the ASA.
///
/// `approve` / `allowance` keep the EVM mapping semantics (per
/// owner→spender) in box storage so existing tooling that watches
/// Approval logs Just Works.
abstract contract AERC20 {
    uint64 public asaId;

    mapping(address => mapping(address => uint256)) private _allowance;

    event Transfer(address indexed from, address indexed to, uint256 value);
    event Approval(address indexed owner, address indexed spender, uint256 value);

    constructor(
        uint64 total_,
        uint8 decimals_,
        string memory name_,
        string memory symbol_
    ) {
        asaId = AVM.asaCreate(total_, decimals_, name_, symbol_);
    }

    function totalSupply() public view returns (uint256) {
        return AVM.asaTotalSupply(asaId);
    }

    function decimals() public view returns (uint8) {
        return AVM.asaDecimals(asaId);
    }

    function symbol() public view returns (string memory) {
        return AVM.asaUnitName(asaId);
    }

    function name() public view returns (string memory) {
        return AVM.asaName(asaId);
    }

    function balanceOf(address holder) public view returns (uint256) {
        return AVM.asaBalance(holder, asaId);
    }

    function transfer(address to, uint256 amount) public returns (bool) {
        AVM.asaTransfer(asaId, msg.sender, to, amount);
        emit Transfer(msg.sender, to, amount);
        return true;
    }

    function approve(address spender, uint256 amount) public returns (bool) {
        _allowance[msg.sender][spender] = amount;
        emit Approval(msg.sender, spender, amount);
        return true;
    }

    function allowance(address owner, address spender) public view returns (uint256) {
        return _allowance[owner][spender];
    }

    function transferFrom(address from, address to, uint256 amount) public returns (bool) {
        uint256 currentAllowance = _allowance[from][msg.sender];
        require(currentAllowance >= amount, "AERC20: insufficient allowance");
        _allowance[from][msg.sender] = currentAllowance - amount;
        AVM.asaTransfer(asaId, from, to, amount);
        emit Transfer(from, to, amount);
        return true;
    }

    /// Distribute `amount` from the contract's reserve holding to `to`.
    /// Inheriting contracts should gate this — the prototype leaves it
    /// open so the test suite can exercise transfer paths without first
    /// having to bootstrap balances out-of-band.
    function mint(address to, uint256 amount) public returns (bool) {
        AVM.asaTransfer(asaId, address(this), to, amount);
        emit Transfer(address(this), to, amount);
        return true;
    }
}
