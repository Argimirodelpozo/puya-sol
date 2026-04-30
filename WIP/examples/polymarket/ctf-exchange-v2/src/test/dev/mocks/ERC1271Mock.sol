// SPDX-License-Identifier: MIT
pragma solidity 0.8.34;

contract ERC1271Mock {
    address public signer;

    bytes4 internal constant MAGIC_VALUE_1271 = 0x1626ba7e;

    constructor(address _signer) {
        signer = _signer;
    }

    // AVM-PORT-ADAPTATION: see PUYA_BLOCKERS.md §2. solady's ECDSA.recover
    // uses `for { 1 } switch case` as a single-iter loop with explicit
    // break, which puya's optimizer collapses to `err` (unreachable). Match
    // the workaround in src/exchange/mixins/Signatures.sol — parse the
    // 65-byte (r||s||v) signature manually and call ecrecover via the
    // precompile (puya-sol routes it to AVM `ecdsa_pk_recover`).
    function isValidSignature(bytes32 hash, bytes memory signature) public view returns (bytes4) {
        if (signature.length != 65) return bytes4(0);
        bytes32 r;
        bytes32 s;
        uint8 v;
        assembly {
            r := mload(add(signature, 0x20))
            s := mload(add(signature, 0x40))
            v := byte(0, mload(add(signature, 0x60)))
        }
        address recovered = ecrecover(hash, v, r, s);
        if (recovered == address(0) || recovered != signer) return bytes4(0);
        return MAGIC_VALUE_1271;
    }
}
