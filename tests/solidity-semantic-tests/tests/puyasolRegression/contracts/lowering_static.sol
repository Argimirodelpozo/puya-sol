pragma solidity ^0.8.20;

contract StaticProbe {
    uint256 public ticks;
    function write() internal returns (uint256) { ticks += 1; return ticks; }
    function launderedView() public view returns (uint256) {
        function() internal returns (uint256) raw = write;
        function() internal view returns (uint256) target;
        assembly { target := raw }
        return target();
    }
    function safeView() external view returns (uint256) { return ticks; }
    function internalViewNoStatic() external returns (uint256) {
        ticks = 0;
        return launderedView();
    }
    function inheritedStatic() external returns (uint256) {
        return this.launderedView();
    }
    function pointerStatic(bool choose) external returns (uint256) {
        function() external view returns (uint256) target = choose ? this.launderedView : this.safeView;
        return target();
    }
    function rawStatic() external returns (bool, bytes memory) {
        bytes memory payload = abi.encodeCall(this.launderedView, ());
        return address(this).staticcall(payload);
    }
    function staticThenInternal() external returns (uint256, uint256) {
        ticks = 0;
        uint256 before = this.safeView();
        return (before, launderedView());
    }
    function directStatic() external returns (bool, bytes memory) {
        return address(this).staticcall(abi.encodeCall(this.launderedView, ()));
    }
    function nestedView() public view returns (uint256) {
        this.safeView();
        return launderedView();
    }
    function nestedStatic() external returns (uint256) {
        return this.nestedView();
    }
    function rawNonStatic() external returns (bool, bytes memory) {
        ticks = 0;
        bytes memory payload = abi.encodeCall(this.launderedView, ());
        return address(this).call(payload);
    }
}
