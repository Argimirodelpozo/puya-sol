// SPDX-License-Identifier: AGPL-3.0-only
// Source: https://github.com/transmissions11/solmate/blob/main/src/tokens/ERC721.sol
// Tests from: https://github.com/transmissions11/solmate/blob/main/src/test/ERC721.t.sol
//
// Modifications for AVM compatibility:
// 1. Changed from abstract to concrete contract (added public mint/burn)
// 2. Removed safeTransferFrom (uses to.code.length and cross-contract callback
//    to ERC721TokenReceiver which are not available on AVM)
// 3. Removed string state vars (name, symbol) and tokenURI
// 4. Removed supportsInterface (bytes4 parameter handling simplified)
// 5. Added explicit getter functions
// 6. Changed ++ and -- on mappings to += 1 and -= 1 (AVM compound assignment fix)
//
// Original core ERC721 logic (approve, setApprovalForAll, transferFrom,
// ownership tracking, balance tracking, approval clearing on transfer)
// is UNCHANGED from the solmate source.
pragma solidity >=0.8.0;

/// @notice Modern, minimalist, and gas efficient ERC-721 implementation.
/// @author Solmate (https://github.com/transmissions11/solmate/blob/main/src/tokens/ERC721.sol)
contract ERC721 {
    /*//////////////////////////////////////////////////////////////
                                 EVENTS
    //////////////////////////////////////////////////////////////*/

    event Transfer(address indexed from, address indexed to, uint256 indexed id);

    event Approval(address indexed owner, address indexed spender, uint256 indexed id);

    event ApprovalForAll(address indexed owner, address indexed operator, bool approved);

    /*//////////////////////////////////////////////////////////////
                      ERC721 BALANCE/OWNER STORAGE
    //////////////////////////////////////////////////////////////*/

    mapping(uint256 => address) internal _ownerOf;

    mapping(address => uint256) internal _balanceOf;

    /*//////////////////////////////////////////////////////////////
                         ERC721 APPROVAL STORAGE
    //////////////////////////////////////////////////////////////*/

    mapping(uint256 => address) internal _getApproved;

    mapping(address => mapping(address => bool)) internal _isApprovedForAll;

    /*//////////////////////////////////////////////////////////////
                              ERC721 LOGIC
    //////////////////////////////////////////////////////////////*/

    function approve(address spender, uint256 id) public virtual {
        address owner = _ownerOf[id];

        require(msg.sender == owner || _isApprovedForAll[owner][msg.sender], "NOT_AUTHORIZED");

        _getApproved[id] = spender;

        emit Approval(owner, spender, id);
    }

    function setApprovalForAll(address operator, bool approved) public virtual {
        _isApprovedForAll[msg.sender][operator] = approved;

        emit ApprovalForAll(msg.sender, operator, approved);
    }

    function transferFrom(
        address from,
        address to,
        uint256 id
    ) public virtual {
        require(from == _ownerOf[id], "WRONG_FROM");

        require(to != address(0), "INVALID_RECIPIENT");

        require(
            msg.sender == from || _isApprovedForAll[from][msg.sender] || msg.sender == _getApproved[id],
            "NOT_AUTHORIZED"
        );

        // Underflow of the sender's balance is impossible because we check for
        // ownership above and the recipient's balance can't realistically overflow.
        unchecked {
            _balanceOf[from] -= 1;

            _balanceOf[to] += 1;
        }

        _ownerOf[id] = to;

        _getApproved[id] = address(0);

        emit Transfer(from, to, id);
    }

    /*//////////////////////////////////////////////////////////////
                            EXPLICIT GETTERS
    //////////////////////////////////////////////////////////////*/

    function ownerOf(uint256 id) public view returns (address) {
        address owner = _ownerOf[id];
        require(owner != address(0), "NOT_MINTED");
        return owner;
    }

    function balanceOf(address owner) public view returns (uint256) {
        require(owner != address(0), "ZERO_ADDRESS");
        return _balanceOf[owner];
    }

    function getApproved(uint256 id) external view returns (address) {
        return _getApproved[id];
    }

    function isApprovedForAll(address owner, address operator) external view returns (bool) {
        return _isApprovedForAll[owner][operator];
    }

    /*//////////////////////////////////////////////////////////////
                        PUBLIC MINT/BURN LOGIC
    //////////////////////////////////////////////////////////////*/

    function mint(address to, uint256 id) external {
        require(to != address(0), "INVALID_RECIPIENT");

        require(_ownerOf[id] == address(0), "ALREADY_MINTED");

        // Counter overflow is incredibly unrealistic.
        unchecked {
            _balanceOf[to] += 1;
        }

        _ownerOf[id] = to;

        emit Transfer(address(0), to, id);
    }

    function burn(uint256 id) external {
        address owner = _ownerOf[id];

        require(owner != address(0), "NOT_MINTED");

        // Ownership check above ensures no underflow.
        unchecked {
            _balanceOf[owner] -= 1;
        }

        _ownerOf[id] = address(0);

        _getApproved[id] = address(0);

        emit Transfer(owner, address(0), id);
    }
}
