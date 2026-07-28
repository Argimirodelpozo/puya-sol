// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
// CUSTOM regression fixture (NOT vendored). Guards bare address literals.
//
// A 40-hex-digit address literal (`0x9BA1...`) is typed `address` by solc but
// SolLiteral built it as a biguint IntegerConstant; nothing coerced biguint→
// account, so assigning it to an `address` var/param/state failed in the puya
// backend with "assignment target type differs from expression value type".
// `msg.sender` (account expr) and `address(0x1234)` (explicit cast) took other
// paths and worked — only BARE literals broke. Fixed by a biguint/uint64→account
// case in TypeCoercion::implicitNumericCast (right-align to a 32-byte address,
// mirroring the explicit cast). Found in real deployed tokens (BRETT/MOG hardcode
// router/multisig/dead addresses).
contract AddressLiteral {
    address public router = 0x7a250d5630B4cF539739dF2C5dAcb4c659F2488D;   // state-var init
    address constant DEAD = 0x000000000000000000000000000000000000dEaD;

    function getRouter() external view returns (address) { return router; }
    function isRouter(address a) external view returns (bool) { return a == router; }
    function localLit() external pure returns (address) {
        address m = 0x9BA188E4B2C46C15450EA5Eac83A048E5E5D9444;   // bare literal → local
        return m;
    }
    function dead() external pure returns (address) { return DEAD; }
    function eqDead(address a) external pure returns (bool) { return a == DEAD; }
    function pick(bool b) external pure returns (address) {
        // two bare literals through a ternary
        return b ? 0x1111111111111111111111111111111111111111
                 : 0x2222222222222222222222222222222222222222;
    }
    function setRouter(address a) external { router = a; }
}
