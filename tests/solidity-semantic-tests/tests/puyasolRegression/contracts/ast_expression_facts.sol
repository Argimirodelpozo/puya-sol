pragma solidity ^0.8.20;

contract AstOptionsTarget {
    function ping() external payable returns (uint256) { return msg.value; }
}

contract AstExpressionFacts {
    uint64 private visits;
    function f() external pure returns (uint256) { return 1; }
    function g() external pure returns (uint256) { return 2; }

    function addresses(bool choose) external view returns (bool) {
        function() external pure returns (uint256) p = choose ? this.f : this.g;
        return address(this) == address((this))
            && address(this) == payable(address(((this))))
            && this.f.address == ((this)).f.address
            && p.address == address(this);
    }

    function foreign(address receiver) external pure returns (bool) {
        function() external pure returns (uint256) p = AstExpressionFacts(receiver).f;
        return p.address == receiver;
    }

    function take(uint256[1] memory a) internal pure returns (uint256) { return a[0]; }
    function singleton() external pure returns (uint256) { return take(([uint256(7)])); }

    function gasOption() internal returns (uint256) { visits = visits * 10 + 1; return 200000; }
    function valueOption() internal returns (uint256) { visits = visits * 10 + 2; return 7; }
    function options(AstOptionsTarget target, bool wrapped) external payable returns (uint64, uint256) {
        visits = 0;
        uint256 received;
        if (wrapped) received = ((target.ping{gas: gasOption(), value: valueOption()}))();
        else received = target.ping{gas: gasOption(), value: valueOption()}();
        return (visits, received);
    }
}
