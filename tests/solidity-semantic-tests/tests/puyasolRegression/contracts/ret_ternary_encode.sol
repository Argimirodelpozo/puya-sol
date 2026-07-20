// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// CUSTOM regression fixture (NOT vendored / not an original Solidity semantic test).
// Guards encodeReturnValue's ternary handling: for a multi-value return with an
// encoded element (signed → arc4.uint256), a ternary branch that is a CALL or
// a nested ternary could not be wrapped in place — the node was retyped to the
// wire tuple but the branch shipped raw native values (minimal-length biguint
// where a 32-byte arc4.uint256 is expected) → corrupt ABI return blob. Such
// shapes now spill through the opaque-tuple path.
contract RetTernaryEncode {
    function inner() internal pure returns (int128, uint256) {
        return (int128(-7), 9);
    }

    function callBranch(bool c) external pure returns (int128, uint256) {
        return c ? (int128(-3), uint256(4)) : inner();
    }

    function nestedTernary(bool c, bool d) external pure returns (int128, uint256) {
        return
            c
                ? (int128(-3), uint256(4))
                : (d ? (int128(-5), uint256(6)) : (int128(-7), uint256(8)));
    }
}
