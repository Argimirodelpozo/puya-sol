// SPDX-License-Identifier: AGPL-3.0-or-later

/// dai.sol -- Dai Stablecoin ERC-20 Token
///
/// SOURCE: https://github.com/makerdao/dss/blob/master/src/dai.sol
/// COMMIT: master branch (AGPL-3.0 license)
///
/// MODIFICATIONS FOR AVM COMPATIBILITY:
/// 1. Pragma upgraded from ^0.6.12 to ^0.8.0 (puya-sol parser requirement)
/// 2. Removed `constructor(uint256 chainId_) public` visibility keyword
///    (public constructors deprecated in 0.8.x; chainId_ param removed since
///    DOMAIN_SEPARATOR computation uses abi.encode which is not yet supported)
/// 3. Removed DOMAIN_SEPARATOR, PERMIT_TYPEHASH, and permit() function
///    (uses abi.encode and ecrecover - EIP-2612 not needed for AVM testing)
/// 4. Removed nonces mapping (only used by permit)
/// 5. Replaced uint(-1) with type(uint256).max (0.8.x syntax)
/// 6. Removed `now` keyword usage (only in permit, which is removed)
/// 7. Added explicit getter functions for public state vars
///    (puya-sol does not generate automatic getters for public vars)
///
/// ALL OTHER CODE IS UNCHANGED FROM THE ORIGINAL.

// Copyright (C) 2017, 2018, 2019 dbrock, rain, mrchico

pragma solidity ^0.8.0;

contract Dai {
    // --- Auth ---
    mapping (address => uint) public wards;
    function rely(address guy) external auth { wards[guy] = 1; }
    function deny(address guy) external auth { wards[guy] = 0; }
    modifier auth {
        require(wards[msg.sender] == 1, "Dai/not-authorized");
        _;
    }

    // --- ERC20 Data ---
    string  public constant name     = "Dai Stablecoin";
    string  public constant symbol   = "DAI";
    string  public constant version  = "1";
    uint8   public constant decimals = 18;
    uint256 public totalSupply;

    mapping (address => uint)                      public balanceOf;
    mapping (address => mapping (address => uint)) public allowance;

    event Approval(address indexed src, address indexed guy, uint wad);
    event Transfer(address indexed src, address indexed dst, uint wad);

    // --- Math ---
    function add(uint x, uint y) internal pure returns (uint z) {
        require((z = x + y) >= x);
    }
    function sub(uint x, uint y) internal pure returns (uint z) {
        require((z = x - y) <= x);
    }

    constructor() {
        wards[msg.sender] = 1;
    }

    // --- Token ---
    function transfer(address dst, uint wad) external returns (bool) {
        return transferFrom(msg.sender, dst, wad);
    }
    function transferFrom(address src, address dst, uint wad)
        public returns (bool)
    {
        require(balanceOf[src] >= wad, "Dai/insufficient-balance");
        if (src != msg.sender && allowance[src][msg.sender] != type(uint256).max) {
            require(allowance[src][msg.sender] >= wad, "Dai/insufficient-allowance");
            allowance[src][msg.sender] = sub(allowance[src][msg.sender], wad);
        }
        balanceOf[src] = sub(balanceOf[src], wad);
        balanceOf[dst] = add(balanceOf[dst], wad);
        emit Transfer(src, dst, wad);
        return true;
    }
    function mint(address usr, uint wad) external auth {
        balanceOf[usr] = add(balanceOf[usr], wad);
        totalSupply    = add(totalSupply, wad);
        emit Transfer(address(0), usr, wad);
    }
    function burn(address usr, uint wad) external {
        require(balanceOf[usr] >= wad, "Dai/insufficient-balance");
        if (usr != msg.sender && allowance[usr][msg.sender] != type(uint256).max) {
            require(allowance[usr][msg.sender] >= wad, "Dai/insufficient-allowance");
            allowance[usr][msg.sender] = sub(allowance[usr][msg.sender], wad);
        }
        balanceOf[usr] = sub(balanceOf[usr], wad);
        totalSupply    = sub(totalSupply, wad);
        emit Transfer(usr, address(0), wad);
    }
    function approve(address usr, uint wad) external returns (bool) {
        allowance[msg.sender][usr] = wad;
        emit Approval(msg.sender, usr, wad);
        return true;
    }

    // --- Alias ---
    function push(address usr, uint wad) external {
        transferFrom(msg.sender, usr, wad);
    }
    function pull(address usr, uint wad) external {
        transferFrom(usr, msg.sender, wad);
    }
    function move(address src, address dst, uint wad) external {
        transferFrom(src, dst, wad);
    }

    // --- Explicit Getters (AVM: public vars don't generate auto-getters) ---
    function getBalanceOf(address usr) external view returns (uint) {
        return balanceOf[usr];
    }
    function getAllowance(address src, address dst) external view returns (uint) {
        return allowance[src][dst];
    }
    function getTotalSupply() external view returns (uint) {
        return totalSupply;
    }
    function getWards(address usr) external view returns (uint) {
        return wards[usr];
    }
}
