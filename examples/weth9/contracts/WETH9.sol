// SPDX-License-Identifier: GPL-3.0

/// WETH9 — Wrapped Ether
///
/// SOURCE: https://github.com/gnosis/canonical-weth/blob/master/contracts/WETH9.sol
/// LICENSE: GPL-3.0 (Copyright (C) 2015, 2016, 2017 Dapphub)
///
/// MODIFICATIONS FOR AVM COMPATIBILITY:
/// 1. Pragma upgraded from >=0.4.22 <0.6 to ^0.8.0
/// 2. Removed `function() external payable` fallback (old-style, AVM N/A)
/// 3. deposit() changed from payable (msg.value) to explicit amount parameter
///    (AVM doesn't have msg.value in app calls; deposit is accounting-only)
/// 4. withdraw() removed `msg.sender.transfer(wad)` ETH send
///    (AVM would need inner payment txn; accounting-only for testing)
/// 5. totalSupply() uses state variable instead of address(this).balance
///    (AVM contract balance != deposited amount conceptually)
/// 6. Replaced uint(-1) with type(uint256).max (0.8.x syntax)
/// 7. Added explicit getter functions (AVM: public vars don't auto-generate)
/// 8. `constructor() public` visibility removed (0.8.x)
///
/// ALL ERC20 LOGIC (approve, transfer, transferFrom, allowance) IS UNCHANGED.

pragma solidity ^0.8.0;

contract WETH9 {
    string public constant name     = "Wrapped Ether";
    string public constant symbol   = "WETH";
    uint8  public constant decimals = 18;

    event  Approval(address indexed src, address indexed guy, uint wad);
    event  Transfer(address indexed src, address indexed dst, uint wad);
    event  Deposit(address indexed dst, uint wad);
    event  Withdrawal(address indexed src, uint wad);

    mapping (address => uint)                       public  balanceOf;
    mapping (address => mapping (address => uint))  public  allowance;

    uint256 private _totalSupply;

    // Modified: explicit amount instead of msg.value
    function deposit(uint wad) public {
        balanceOf[msg.sender] = balanceOf[msg.sender] + wad;
        _totalSupply = _totalSupply + wad;
        emit Deposit(msg.sender, wad);
    }

    // Modified: removed msg.sender.transfer(wad)
    function withdraw(uint wad) public {
        require(balanceOf[msg.sender] >= wad, "WETH9: insufficient balance");
        balanceOf[msg.sender] = balanceOf[msg.sender] - wad;
        _totalSupply = _totalSupply - wad;
        emit Withdrawal(msg.sender, wad);
    }

    // Modified: uses state variable instead of address(this).balance
    function totalSupply() public view returns (uint) {
        return _totalSupply;
    }

    // UNCHANGED from original
    function approve(address guy, uint wad) public returns (bool) {
        allowance[msg.sender][guy] = wad;
        emit Approval(msg.sender, guy, wad);
        return true;
    }

    // UNCHANGED from original
    function transfer(address dst, uint wad) public returns (bool) {
        return transferFrom(msg.sender, dst, wad);
    }

    // UNCHANGED from original (except uint(-1) → type(uint256).max)
    function transferFrom(address src, address dst, uint wad)
        public
        returns (bool)
    {
        require(balanceOf[src] >= wad, "WETH9: insufficient balance");

        if (src != msg.sender && allowance[src][msg.sender] != type(uint256).max) {
            require(allowance[src][msg.sender] >= wad, "WETH9: insufficient allowance");
            allowance[src][msg.sender] = allowance[src][msg.sender] - wad;
        }

        balanceOf[src] = balanceOf[src] - wad;
        balanceOf[dst] = balanceOf[dst] + wad;

        emit Transfer(src, dst, wad);

        return true;
    }

    // Explicit getters
    function getBalanceOf(address account) external view returns (uint) {
        return balanceOf[account];
    }
    function getAllowance(address src, address guy) external view returns (uint) {
        return allowance[src][guy];
    }
}
