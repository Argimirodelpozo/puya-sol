// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev ERC20 with permit (EIP-2612) - domain separator and permit hash construction.
 * Demonstrates EIP-712 typed data hashing on Algorand.
 * Signature verification is stubbed since AVM uses ed25519, not secp256k1.
 */
contract ERC20PermitTest {
    string private _name;
    string private _symbol;
    uint8 private _decimals;

    mapping(address => uint256) private _balances;
    mapping(address => mapping(address => uint256)) private _allowances;
    mapping(address => uint256) private _nonces;
    uint256 private _totalSupply;

    // EIP-712 constants
    bytes32 private constant TYPE_HASH = keccak256("EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)");
    bytes32 private constant PERMIT_TYPEHASH = keccak256("Permit(address owner,address spender,uint256 value,uint256 nonce,uint256 deadline)");

    constructor() {
        _name = "PermitToken";
        _symbol = "PMT";
        _decimals = 18;
    }

    function name() external view returns (string memory) {
        return _name;
    }

    function symbol() external view returns (string memory) {
        return _symbol;
    }

    function decimals() external view returns (uint8) {
        return _decimals;
    }

    function totalSupply() external view returns (uint256) {
        return _totalSupply;
    }

    function balanceOf(address account) external view returns (uint256) {
        return _balances[account];
    }

    function allowance(address owner, address spender) external view returns (uint256) {
        return _allowances[owner][spender];
    }

    function nonces(address owner) external view returns (uint256) {
        return _nonces[owner];
    }

    // Mint tokens (for testing)
    function mint(address to, uint256 amount) external {
        _balances[to] += amount;
        _totalSupply += amount;
    }

    function approve(address spender, uint256 amount) external returns (bool) {
        _allowances[msg.sender][spender] = amount;
        return true;
    }

    function transfer(address to, uint256 amount) external returns (bool) {
        require(_balances[msg.sender] >= amount, "ERC20: insufficient balance");
        _balances[msg.sender] -= amount;
        _balances[to] += amount;
        return true;
    }

    function transferFrom(address from, address to, uint256 amount) external returns (bool) {
        require(_allowances[from][msg.sender] >= amount, "ERC20: insufficient allowance");
        require(_balances[from] >= amount, "ERC20: insufficient balance");
        _allowances[from][msg.sender] -= amount;
        _balances[from] -= amount;
        _balances[to] += amount;
        return true;
    }

    // EIP-712 domain separator
    function DOMAIN_SEPARATOR() public pure returns (bytes32) {
        return keccak256(abi.encodePacked(
            TYPE_HASH,
            keccak256(bytes("PermitToken")),
            keccak256(bytes("1")),
            uint256(0),  // chainId (0 for Algorand)
            uint256(0)   // verifyingContract placeholder
        ));
    }

    // Compute permit struct hash
    function getPermitHash(
        address owner,
        address spender,
        uint256 value,
        uint256 nonce,
        uint256 deadline
    ) external pure returns (bytes32) {
        return keccak256(abi.encodePacked(
            PERMIT_TYPEHASH,
            uint256(uint160(owner)),
            uint256(uint160(spender)),
            value,
            nonce,
            deadline
        ));
    }

    // Compute full EIP-712 digest (what would be signed)
    function getDigest(
        address owner,
        address spender,
        uint256 value,
        uint256 nonce,
        uint256 deadline
    ) external pure returns (bytes32) {
        bytes32 structHash = keccak256(abi.encodePacked(
            PERMIT_TYPEHASH,
            uint256(uint160(owner)),
            uint256(uint160(spender)),
            value,
            nonce,
            deadline
        ));
        return keccak256(abi.encodePacked(
            bytes2(0x1901),
            DOMAIN_SEPARATOR(),
            structHash
        ));
    }

    // Use a nonce (for testing nonce tracking)
    function useNonce(address owner) external returns (uint256) {
        uint256 current = _nonces[owner];
        _nonces[owner] = current + 1;
        return current;
    }
}
