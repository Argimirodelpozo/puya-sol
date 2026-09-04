// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
// CUSTOM regression fixture (NOT vendored). Guards modifier-argument wtype.
//
// `onlyRole(MINTER_ROLE)` binds `keccak256("MINTER_ROLE")` — whose AWST wtype is
// UNSIZED `bytes` — to a `bytes32` modifier parameter. The bytes are already
// correct; only the wtype bookkeeping disagreed, but puya type-checks the pair
// and rejected the whole program with
//     assignment target type differs from expression value type
// That one mismatch blocked five real deployed contracts in the chainwide
// sweep (gho, strk, imx, xerc20, burnminterc20) — it is the OZ AccessControl
// `onlyRole(SOME_ROLE)` idiom, so it is everywhere.
//
// Fixed by relabelling the value to the declared bytesN at modifier argument
// binding.
contract ModifierArgBytes32 {
    bytes32 public constant MINTER_ROLE = keccak256("MINTER_ROLE");
    bytes32 public constant BURNER_ROLE = keccak256("BURNER_ROLE");
    mapping(bytes32 => mapping(address => bool)) public members;
    bytes32 public lastRole;

    modifier onlyRole(bytes32 role) {
        lastRole = role;
        _;
    }

    function grant(bytes32 role, address a) external { members[role][a] = true; }
    // constant-keccak argument — the shape that failed
    function mint() external onlyRole(MINTER_ROLE) returns (uint256) { return 7; }
    function burn() external onlyRole(BURNER_ROLE) returns (uint256) { return 9; }
    // runtime bytes32 argument — must keep working
    function anyRole(bytes32 r) external onlyRole(r) returns (uint256) { return 11; }
    function isMember(bytes32 role, address a) external view returns (bool) {
        return members[role][a];
    }
}
