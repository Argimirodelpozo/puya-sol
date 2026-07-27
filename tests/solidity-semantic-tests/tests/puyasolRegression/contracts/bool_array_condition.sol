// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
// CUSTOM regression fixture (NOT vendored). Guards a bool[] ELEMENT used directly
// as a condition. arc4.bool is an ARC4BasicWType of kind `Basic` (same kind as
// native `bool`), so SolArrayBuilder's kind-based needsDecode check missed it —
// a bool[] element stayed arc4.bool and `if (flags[i])` tripped the puya backend
// ("IfElse.condition expected bool"). Fixed by decoding arc4.bool array elements
// to native bool. Found via OZ MerkleProof.multiProofVerify (`bool[] proofFlags`).
contract BoolArrCond {
    function ifElem(bool[] calldata f, uint256 i) external pure returns (uint256) { if (f[i]) return 1; return 0; }
    function ternElem(bool[] calldata f, uint256 i) external pure returns (uint256) { return f[i] ? 7 : 9; }
    function reqElem(bool[] calldata f, uint256 i) external pure returns (uint256) { require(f[i]); return 3; }
    function andElem(bool[] calldata f, uint256 i, uint256 j) external pure returns (bool) { return f[i] && f[j]; }
    function retElem(bool[] calldata f, uint256 i) external pure returns (bool) { return f[i]; }
}
