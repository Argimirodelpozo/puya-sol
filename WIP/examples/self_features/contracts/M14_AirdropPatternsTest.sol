// SPDX-License-Identifier: MIT
pragma solidity 0.8.28;

import "libraries/CircuitAttributeHandlerV2.sol";
import "constants/AttestationId.sol";

/**
 * M14: Tests patterns used in the Airdrop contract.
 * Exercises: modifiers, msg.sender, events with indexed params,
 * address-to-uint cast, mapping(uint256 => bool), mapping(uint256 => uint256),
 * struct field access from memory params, multiple state variables.
 */
contract AirdropPatternsTest {
    // State
    address public owner;
    bool public isRegistrationOpen;
    bool public isClaimOpen;
    bytes32 public merkleRoot;
    bytes32 public verificationConfigId;

    mapping(uint256 => bool) public registeredUsers;
    mapping(uint256 => uint256) public nullifierToUser;
    mapping(address => bool) public claimed;

    // Events
    event RegistrationOpen();
    event RegistrationClose();
    event ClaimOpen();
    event ClaimClose();
    event UserRegistered(uint256 indexed userId, uint256 indexed nullifier);
    event MerkleRootUpdated(bytes32 newMerkleRoot);
    event TokensClaimed(uint256 index, address account, uint256 amount);

    // Custom errors
    error NotOwner();
    error RegistrationNotOpen();
    error AlreadyRegistered();
    error InvalidUser();

    // Modifier
    modifier onlyOwner() {
        if (msg.sender != owner) {
            revert NotOwner();
        }
        _;
    }

    constructor() {
        owner = msg.sender;
    }

    // Owner-only functions
    function setMerkleRoot(bytes32 newRoot) external onlyOwner {
        merkleRoot = newRoot;
        emit MerkleRootUpdated(newRoot);
    }

    function openRegistration() external onlyOwner {
        isRegistrationOpen = true;
        emit RegistrationOpen();
    }

    function closeRegistration() external onlyOwner {
        isRegistrationOpen = false;
        emit RegistrationClose();
    }

    function openClaim() external onlyOwner {
        isClaimOpen = true;
        emit ClaimOpen();
    }

    function closeClaim() external onlyOwner {
        isClaimOpen = false;
        emit ClaimClose();
    }

    function setConfigId(bytes32 configId) external onlyOwner {
        verificationConfigId = configId;
    }

    // Registration logic (simplified from Airdrop.customVerificationHook)
    function register(uint256 userId, uint256 nullifier) external {
        if (!isRegistrationOpen) {
            revert RegistrationNotOpen();
        }
        if (userId == 0) {
            revert InvalidUser();
        }
        if (registeredUsers[userId]) {
            revert AlreadyRegistered();
        }

        nullifierToUser[nullifier] = userId;
        registeredUsers[userId] = true;

        emit UserRegistered(userId, nullifier);
    }

    // Check if address is registered (uses uint256(uint160(address)) pattern)
    function isAddressRegistered(address addr) external view returns (bool) {
        return registeredUsers[uint256(uint160(addr))];
    }

    // Register by address
    function registerAddress(address addr) external {
        uint256 userId = uint256(uint160(addr));
        registeredUsers[userId] = true;
    }

    // Get owner
    function getOwner() external view returns (address) {
        return owner;
    }

    // Test msg.sender
    function getSender() external view returns (address) {
        return msg.sender;
    }

    // Test keccak256-based node computation (same as Airdrop.claim)
    function computeClaimNode(uint256 index, address account, uint256 amount) external pure returns (bytes32) {
        return keccak256(abi.encodePacked(index, account, amount));
    }

    // Test configId getter
    function getConfigId() external view returns (bytes32) {
        return verificationConfigId;
    }

    // Test cross-library: get issuing state
    function testIssuingState(bytes memory charcodes) external pure returns (string memory) {
        return CircuitAttributeHandlerV2.getIssuingState(AttestationId.E_PASSPORT, charcodes);
    }
}
