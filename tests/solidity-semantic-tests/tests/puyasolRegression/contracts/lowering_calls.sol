pragma solidity ^0.8.20;

contract Sink {
    function f() external pure returns (uint256) { return 11; }
    function g() external pure returns (uint256) { return 22; }
    function pay() external payable returns (uint256) { return msg.value; }
    function sum(uint256 a, uint256 b) external pure returns (uint256) { return a + b; }
}

library Pretend {
    function encodeWithSignature(string memory) internal pure returns (bytes memory) {
        return abi.encodeWithSignature("g()");
    }
}

contract Probe {
    uint256 public ticks;
    function write() internal returns (uint256) { ticks += 1; return ticks; }
    function pureTarget() internal pure returns (uint256) { return 9; }
    function internalPurePointer() external returns (uint256) {
        ticks = 0;
        function() internal returns (uint256) raw = write;
        function() internal pure returns (uint256) target = pureTarget;
        assembly { target := raw }
        return target();
    }
    function valueOption() internal returns (uint256) { ticks += 100; return 7; }
    function gasOption() internal returns (uint256) { ticks += 1000; return 100000; }
    function pointerOptions(address sink) external payable returns (uint256, uint256) {
        ticks = 0;
        function() external payable returns (uint256) target = Sink(sink).pay;
        uint256 got = target{value: valueOption(), gas: gasOption()}();
        return (got, ticks);
    }
    function foreignSelector(address sink) external returns (uint256) {
        (bool ok, bytes memory data) = sink.call(((abi.encodeWithSelector)((Sink.f.selector))));
        require(ok);
        return abi.decode(data, (uint256));
    }
    function foreignSelectorSum(address sink) external returns (uint256) {
        (bool ok, bytes memory data) = sink.call(((abi.encodeWithSelector)(Sink.sum.selector, uint256(7), uint256(9))));
        require(ok);
        return abi.decode(data, (uint256));
    }
    function fakeEncoder(address sink) external returns (uint256) {
        (bool ok, bytes memory data) = sink.call(Pretend.encodeWithSignature("f()"));
        require(ok);
        return abi.decode(data, (uint256));
    }
    function f() external pure returns (uint256) { return 11; }
    function g() external pure returns (uint256) { return 22; }
    function fakeSelfEncoder() external returns (uint256) {
        (bool ok, bytes memory data) = address(this).call(Pretend.encodeWithSignature("f()"));
        require(ok);
        return abi.decode(data, (uint256));
    }
    function ping() external pure returns (uint256) { return 42; }
    function directSelf() external returns (bool, bytes memory) {
        return address(this).call(abi.encodeCall(this.ping, ()));
    }
    function rawSelf() external returns (bool, bytes memory) {
        bytes memory payload = abi.encodeCall(this.ping, ());
        return address(this).call(payload);
    }
    function rawSelectorSelf() external returns (bool, bytes memory) {
        bytes memory payload = abi.encodeWithSelector((this.ping).selector);
        return address(this).call((payload));
    }
    function parenthesizedSelf() external returns (bool, bytes memory) {
        return address(this).call((abi.encodeCall(this.ping, ())));
    }
    receive() external payable { ticks += 5; }
    function emptySelf() external returns (bool, uint256) {
        ticks = 0;
        (bool ok,) = address(this).call("");
        return (ok, ticks);
    }
}
