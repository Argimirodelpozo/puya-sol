pragma solidity ^0.8.20;

interface IReachabilityToken {
    function mint(uint256 value) external returns (uint256);
}

contract ReachabilityToken is IReachabilityToken {
    uint256 public saved;
    modifier checked(uint256 value) { _check(value); _; }
    function _check(uint256 value) internal virtual { require(value < 100); }
    function mint(uint256 value) external checked(value) returns (uint256) {
        return _update(value);
    }
    function _update(uint256 value) internal virtual returns (uint256) {
        saved = value;
        return value + 1;
    }
}

contract ReachabilityDerivedToken is ReachabilityToken {
    function _update(uint256 value) internal override returns (uint256) {
        return super._update(value) + 10;
    }
}

library ReachabilityLibrary {
    function add(uint256 value) public pure returns (uint256) { return helper(value); }
    function helper(uint256 value) internal pure returns (uint256) { return value + 3; }
}

contract ReachabilityBridge {
    function concrete(ReachabilityToken token, uint256 value) external returns (uint256) {
        return ((token.mint))(value);
    }
    function throughInterface(IReachabilityToken token, uint256 value) external returns (uint256) {
        return token.mint(value);
    }
    function throughPointer(ReachabilityToken token, uint256 value) external returns (uint256) {
        function(uint256) external returns (uint256) mint = token.mint;
        return (mint)(value);
    }
    function local(uint256 value) external pure returns (uint256) {
        return ReachabilityLibrary.add(localUpdate(value));
    }
    function localUpdate(uint256 value) internal pure virtual returns (uint256) { return value + 100; }
    function self(uint256 value) external view returns (uint256) {
        return ReachabilityBridge(address(this)).local(value);
    }
}

contract ReachabilityShadowBridge is ReachabilityBridge {
    function _update(uint256 value) internal pure virtual returns (uint256) { return value + 200; }
    function localUpdate(uint256 value) internal pure override returns (uint256) { return _update(value); }
}
