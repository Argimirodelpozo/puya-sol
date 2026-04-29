// SPDX-License-Identifier: GPL-3.0-or-later
// Source: https://github.com/dapphub/ds-token/blob/master/src/token.sol
// Dependencies inlined from:
//   https://github.com/dapphub/ds-auth/blob/master/src/auth.sol
//   https://github.com/dapphub/ds-math/blob/master/src/math.sol
// Tests from: https://github.com/dapphub/ds-token/blob/master/src/token.t.sol
//
// Modifications for AVM compatibility:
// 1. Pragma upgraded from >=0.4.23 to >=0.8.0 (0.8 parser)
// 2. DSMath and DSAuth inlined (no import support)
// 3. DSAuth simplified: removed DSAuthority interface (cross-contract call),
//    kept owner + auth modifier with owner-only check
// 4. uint(-1) → type(uint256).max
// 5. Removed constructor visibility (public)
// 6. Removed function overloading (renamed 1-arg mint/burn/approve to
//    mintSelf/burnSelf/approveMax for ABI clarity)
// 7. Added explicit getter functions
// 8. Removed string state vars (symbol, name) — AVM string handling simplified
//
// Original logic (transfer, transferFrom, push/pull/move, mint, burn,
// auth modifier, stop/start, infinite allowance) is UNCHANGED from DSToken.
pragma solidity >=0.8.0;

/// @notice DSMath -- safe arithmetic
/// @author DappHub (https://github.com/dapphub/ds-math)
contract DSMath {
    function add(uint x, uint y) internal pure returns (uint z) {
        require((z = x + y) >= x, "ds-math-add-overflow");
    }
    function sub(uint x, uint y) internal pure returns (uint z) {
        require((z = x - y) <= x, "ds-math-sub-underflow");
    }
}

/// @notice DSToken -- ERC20 implementation with minting and burning
/// @author DappHub (https://github.com/dapphub/ds-token)
contract DSToken is DSMath {
    bool                                              internal _stopped;
    uint256                                           internal _totalSupply;
    mapping (address => uint256)                      internal _balanceOf;
    mapping (address => mapping (address => uint256)) internal _allowance;
    address                                           internal _owner;

    event Approval(address indexed src, address indexed guy, uint wad);
    event Transfer(address indexed src, address indexed dst, uint wad);
    event Mint(address indexed guy, uint wad);
    event Burn(address indexed guy, uint wad);
    event Stop();
    event Start();

    modifier auth {
        require(msg.sender == _owner, "ds-auth-unauthorized");
        _;
    }

    modifier stoppable {
        require(!_stopped, "ds-stop-is-stopped");
        _;
    }

    constructor() {
        _owner = msg.sender;
    }

    function approve(address guy, uint wad) public stoppable returns (bool) {
        _allowance[msg.sender][guy] = wad;
        emit Approval(msg.sender, guy, wad);
        return true;
    }

    function approveMax(address guy) external returns (bool) {
        return approve(guy, type(uint256).max);
    }

    function transfer(address dst, uint wad) external returns (bool) {
        return transferFrom(msg.sender, dst, wad);
    }

    function transferFrom(address src, address dst, uint wad)
        public
        stoppable
        returns (bool)
    {
        if (src != msg.sender && _allowance[src][msg.sender] != type(uint256).max) {
            require(_allowance[src][msg.sender] >= wad, "ds-token-insufficient-approval");
            _allowance[src][msg.sender] = sub(_allowance[src][msg.sender], wad);
        }

        require(_balanceOf[src] >= wad, "ds-token-insufficient-balance");
        _balanceOf[src] = sub(_balanceOf[src], wad);
        _balanceOf[dst] = add(_balanceOf[dst], wad);

        emit Transfer(src, dst, wad);
        return true;
    }

    function push(address dst, uint wad) external {
        transferFrom(msg.sender, dst, wad);
    }

    function pull(address src, uint wad) external {
        transferFrom(src, msg.sender, wad);
    }

    function move(address src, address dst, uint wad) external {
        transferFrom(src, dst, wad);
    }

    function mint(address guy, uint wad) public auth stoppable {
        _balanceOf[guy] = add(_balanceOf[guy], wad);
        _totalSupply = add(_totalSupply, wad);
        emit Mint(guy, wad);
    }

    function mintSelf(uint wad) external {
        mint(msg.sender, wad);
    }

    function burn(address guy, uint wad) public auth stoppable {
        if (guy != msg.sender && _allowance[guy][msg.sender] != type(uint256).max) {
            require(_allowance[guy][msg.sender] >= wad, "ds-token-insufficient-approval");
            _allowance[guy][msg.sender] = sub(_allowance[guy][msg.sender], wad);
        }

        require(_balanceOf[guy] >= wad, "ds-token-insufficient-balance");
        _balanceOf[guy] = sub(_balanceOf[guy], wad);
        _totalSupply = sub(_totalSupply, wad);
        emit Burn(guy, wad);
    }

    function burnSelf(uint wad) external {
        burn(msg.sender, wad);
    }

    function stop() public auth {
        _stopped = true;
        emit Stop();
    }

    function start() public auth {
        _stopped = false;
        emit Start();
    }

    // ─── Explicit Getters ───

    function getOwner() external view returns (address) {
        return _owner;
    }

    function stopped() external view returns (bool) {
        return _stopped;
    }

    function totalSupply() external view returns (uint256) {
        return _totalSupply;
    }

    function getBalanceOf(address guy) external view returns (uint256) {
        return _balanceOf[guy];
    }

    function getAllowance(address src, address guy) external view returns (uint256) {
        return _allowance[src][guy];
    }
}
