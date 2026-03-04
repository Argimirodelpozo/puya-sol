// SPDX-License-Identifier: GPL-3.0-or-later
pragma solidity ^0.8.0;

/// @notice Simple whitelist implementation of DSAuthority — 3D Access Control List
/// @author DappHub (https://github.com/dapphub/ds-guard/blob/master/src/guard.sol)
/// @dev Modifications for AVM: inlined DSAuth (simplified to owner-only), removed
///      DSGuardFactory, added explicit getters. Uses bytes32 for all ACL dimensions
///      (original used bytes4 for sig, but bytes32 needed for ANY wildcard constant).
///
/// The ACL maps (src, dst, sig) → bool, where ANY (all 1s) is a wildcard.
/// canCall checks all 8 combinations of specific/ANY for each dimension.
contract DSGuard {
    /*//////////////////////////////////////////////////////////////
                                 EVENTS
    //////////////////////////////////////////////////////////////*/

    event LogPermit(bytes32 indexed src, bytes32 indexed dst, bytes32 indexed sig);
    event LogForbid(bytes32 indexed src, bytes32 indexed dst, bytes32 indexed sig);
    event OwnershipTransferred(address indexed previousOwner, address indexed newOwner);

    /*//////////////////////////////////////////////////////////////
                               CONSTANTS
    //////////////////////////////////////////////////////////////*/

    /// @notice Wildcard constant — matches any src, dst, or sig
    bytes32 public constant ANY = bytes32(type(uint256).max);

    /*//////////////////////////////////////////////////////////////
                                STORAGE
    //////////////////////////////////////////////////////////////*/

    address internal _owner;

    /// @notice 3D access control list: acl[src][dst][sig] → bool
    mapping(bytes32 => mapping(bytes32 => mapping(bytes32 => bool))) internal _acl;

    /*//////////////////////////////////////////////////////////////
                             CONSTRUCTOR
    //////////////////////////////////////////////////////////////*/

    constructor() {
        _owner = msg.sender;
        emit OwnershipTransferred(address(0), msg.sender);
    }

    modifier auth() {
        require(msg.sender == _owner, "ds-guard-auth-unauthorized");
        _;
    }

    function owner() public view returns (address) {
        return _owner;
    }

    /*//////////////////////////////////////////////////////////////
                           ACL QUERY LOGIC
    //////////////////////////////////////////////////////////////*/

    /// @notice Check if src can call dst with sig, checking all wildcard combinations
    function canCall(
        bytes32 src,
        bytes32 dst,
        bytes32 sig
    ) public view returns (bool) {
        return _acl[src][dst][sig]
            || _acl[src][dst][ANY]
            || _acl[src][ANY][sig]
            || _acl[src][ANY][ANY]
            || _acl[ANY][dst][sig]
            || _acl[ANY][dst][ANY]
            || _acl[ANY][ANY][sig]
            || _acl[ANY][ANY][ANY];
    }

    /// @notice Read a specific ACL entry without wildcard expansion
    function getAcl(bytes32 src, bytes32 dst, bytes32 sig) public view returns (bool) {
        return _acl[src][dst][sig];
    }

    /*//////////////////////////////////////////////////////////////
                          ACL MUTATION LOGIC
    //////////////////////////////////////////////////////////////*/

    /// @notice Grant permission for src to call dst with sig
    function permit(bytes32 src, bytes32 dst, bytes32 sig) public auth {
        _acl[src][dst][sig] = true;
        emit LogPermit(src, dst, sig);
    }

    /// @notice Revoke permission for src to call dst with sig
    function forbid(bytes32 src, bytes32 dst, bytes32 sig) public auth {
        _acl[src][dst][sig] = false;
        emit LogForbid(src, dst, sig);
    }
}
