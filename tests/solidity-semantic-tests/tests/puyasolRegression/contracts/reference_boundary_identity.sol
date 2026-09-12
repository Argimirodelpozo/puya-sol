pragma solidity ^0.8.20;

struct RefItem { uint256 x; }

library ReferenceBoundaryLibrary {
    function privateWrite(RefItem memory p) private pure { p.x = 9; }
    function write(RefItem memory p) internal pure { privateWrite(p); }
    function rebind(RefItem memory p) internal pure { p.x = 7; p = RefItem(9); p.x = 11; }
    function efficientKeccak256(bytes32, bytes32) internal pure returns (bytes32) {}
    function realHash(bytes32 a, bytes32 b) internal pure returns (bytes32 result) {
        assembly { mstore(0, a) mstore(32, b) result := keccak256(0, 64) }
    }
}

function freeWrite(RefItem memory p) pure {
    RefItem memory alias_ = p;
    alias_.x = 9;
}

contract ReferenceBoundaryIdentity {
    function privateWrite(RefItem memory p) private pure { p.x = 9; }
    function publicWrite(RefItem memory p) public pure { p.x = 9; }
    function internalWrite(RefItem memory p) internal pure { p.x = 9; }
    function aliasWrite(RefItem memory p) internal pure { RefItem memory q = p; q.x = 9; }
    function rebind(RefItem memory p) internal pure { p = RefItem(9); }
    function mutateRebind(RefItem memory p) internal pure { p.x = 7; p = RefItem(9); p.x = 11; }
    function aliasRebind(RefItem memory p) internal pure {
        RefItem memory q = p;
        p = RefItem(9);
        q.x = 13;
        p.x = 17;
    }
    function conditional(RefItem memory p, bool yes) internal pure returns (uint256) {
        p = yes ? RefItem(9) : p;
        p.x = 21;
        return p.x;
    }
    function tailWrite(RefItem memory p) internal pure returns (uint256) { p.x = 42; return p.x; }
    function returningCall(RefItem memory p) internal pure returns (uint256) {
        RefItem memory q = p;
        p = RefItem(9);
        return tailWrite(q);
    }
    function tupleRebind(RefItem memory p) internal pure {
        p.x = 7;
        uint256 n;
        (p, n) = (RefItem(9), 1);
        p.x += n;
    }
    function loopRebind(RefItem memory p) internal pure {
        p.x = 7;
        for (uint256 i; i < 2; ++i) { p = RefItem(i + 9); p.x += 1; }
    }
    function viaInternal() external pure returns (uint256) { RefItem memory p = RefItem(5); internalWrite(p); return p.x; }
    function viaPrivate() external pure returns (uint256) { RefItem memory p = RefItem(5); privateWrite(p); return p.x; }
    function viaPublic() external pure returns (uint256) { RefItem memory p = RefItem(5); publicWrite(p); return p.x; }
    function viaAlias() external pure returns (uint256) { RefItem memory p = RefItem(5); aliasWrite(p); return p.x; }
    function viaFree() external pure returns (uint256) { RefItem memory p = RefItem(5); freeWrite(p); return p.x; }
    function viaLibrary() external pure returns (uint256) { RefItem memory p = RefItem(5); ReferenceBoundaryLibrary.write(p); return p.x; }
    function viaLibraryRebind() external pure returns (uint256) { RefItem memory p = RefItem(5); ReferenceBoundaryLibrary.rebind(p); return p.x; }
    function viaRebind() external pure returns (uint256) { RefItem memory p = RefItem(5); rebind(p); return p.x; }
    function viaMutateRebind() external pure returns (uint256) { RefItem memory p = RefItem(5); mutateRebind(p); return p.x; }
    function viaAliasRebind() external pure returns (uint256) { RefItem memory p = RefItem(5); aliasRebind(p); return p.x; }
    function viaConditional(bool yes) external pure returns (uint256, uint256) {
        RefItem memory p = RefItem(5); uint256 x = conditional(p, yes); return (p.x, x);
    }
    function viaReturningCall() external pure returns (uint256, uint256) {
        RefItem memory p = RefItem(5); uint256 x = returningCall(p); return (p.x, x);
    }
    function viaTupleRebind() external pure returns (uint256) { RefItem memory p = RefItem(5); tupleRebind(p); return p.x; }
    function viaLoopRebind() external pure returns (uint256) { RefItem memory p = RefItem(5); loopRebind(p); return p.x; }
    function localRebind() external pure returns (uint256, uint256) {
        RefItem memory p = RefItem(5); RefItem memory q = p; p = RefItem(9); q.x = 7; return (p.x, q.x);
    }
    function arrayRebind(uint256[] memory p) internal pure { p[0] = 7; p = new uint256[](2); p[0] = 9; }
    function viaArrayRebind() external pure returns (uint256) {
        uint256[] memory p = new uint256[](1); p[0] = 5; arrayRebind(p); return p[0];
    }
    function emptyHash() external pure returns (bytes32) {
        return ReferenceBoundaryLibrary.efficientKeccak256(bytes32(uint256(1)), bytes32(uint256(2)));
    }
    function actualHash() external pure returns (bytes32) {
        return ReferenceBoundaryLibrary.realHash(bytes32(uint256(1)), bytes32(uint256(2)));
    }
}
