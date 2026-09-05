// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract NativePaymentReceiver {
    uint256 public received;
    uint256 public creationValue;

    constructor() payable { creationValue = msg.value; }
    function deposit() external payable { received = msg.value; }
    receive() external payable { received = msg.value; }
    fallback() external payable { received = msg.value; }
}

contract NativePaymentRejectingReceiver {
    receive() external payable { revert("no_receive"); }
}

contract NativePaymentNoReceiver {
    function f() external pure returns (uint256) { return 1; }
}

contract NativePaymentFallbackReceiver {
    uint256 public received;
    uint256 public calls;

    fallback() external payable {
        received = msg.value;
        calls += 1;
    }
}

contract NativePaymentSender {
    uint256 public receiverEvaluations;
    uint256 public amountEvaluations;

    function transferTo(address payable target, uint256 amount) external {
        target.transfer(amount);
    }

    function sendTo(address payable target, uint256 amount) external returns (bool) {
        return target.send(amount);
    }

    function callTo(address target, uint256 amount) external returns (bool ok) {
        (ok,) = target.call{value: amount}("");
    }

    function assemblyTo(address target, uint256 amount) external returns (uint256 ok) {
        assembly { ok := call(gas(), target, amount, 0, 0, 0, 0) }
    }

    function assemblyWordTo(uint256 target, uint256 amount) external returns (uint256 ok) {
        assembly { ok := call(gas(), target, amount, 0, 0, 0, 0) }
    }

    function typedTo(NativePaymentReceiver target, uint256 amount) external {
        target.deposit{value: amount}();
    }

    function chooseReceiver(address payable target) internal returns (address payable) {
        receiverEvaluations += 1;
        return target;
    }

    function chooseAmount(uint256 amount) internal returns (uint256) {
        amountEvaluations += 1;
        return amount;
    }

    function transferOnce(address payable target, uint256 amount) external {
        chooseReceiver(target).transfer(chooseAmount(amount));
    }

    function create(uint256 amount) external returns (uint256) {
        NativePaymentReceiver child = new NativePaymentReceiver{value: amount}();
        return child.creationValue();
    }
}

contract NativePaymentClose {
    function close(address payable target) external {
        selfdestruct(target);
    }
}
