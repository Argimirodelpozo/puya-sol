// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

abstract contract Payroll {
    address private _admin;
    uint256 private _employeeCount;
    uint256 private _totalPaidOut;
    uint256 private _totalPayroll;

    mapping(address => uint256) internal _salary;
    mapping(address => uint256) internal _totalPaid;
    mapping(address => uint256) internal _paymentCount;
    mapping(address => bool) internal _isEmployee;
    mapping(address => bool) internal _isTerminated;
    mapping(address => uint256) internal _employeeIndex;

    constructor() {
        _admin = msg.sender;
    }

    function admin() external view returns (address) {
        return _admin;
    }

    function employeeCount() external view returns (uint256) {
        return _employeeCount;
    }

    function totalPaidOut() external view returns (uint256) {
        return _totalPaidOut;
    }

    function totalPayroll() external view returns (uint256) {
        return _totalPayroll;
    }

    function getSalary(address employee) external view returns (uint256) {
        return _salary[employee];
    }

    function getTotalPaidTo(address employee) external view returns (uint256) {
        return _totalPaid[employee];
    }

    function getPaymentCount(address employee) external view returns (uint256) {
        return _paymentCount[employee];
    }

    function isEmployee(address employee) external view returns (bool) {
        return _isEmployee[employee];
    }

    function isTerminated(address employee) external view returns (bool) {
        return _isTerminated[employee];
    }

    function addEmployee(address employee, uint256 salary) external {
        require(msg.sender == _admin, "Only admin");
        require(!_isEmployee[employee], "Already employee");
        _salary[employee] = salary;
        _isEmployee[employee] = true;
        _isTerminated[employee] = false;
        _employeeCount = _employeeCount + 1;
        _employeeIndex[employee] = _employeeCount;
        _totalPayroll = _totalPayroll + salary;
    }

    function payEmployee(address employee) external {
        require(msg.sender == _admin, "Only admin");
        require(_isEmployee[employee], "Not employee");
        require(!_isTerminated[employee], "Terminated");
        uint256 salary = _salary[employee];
        _totalPaid[employee] = _totalPaid[employee] + salary;
        _totalPaidOut = _totalPaidOut + salary;
        _paymentCount[employee] = _paymentCount[employee] + 1;
    }

    function terminate(address employee) external {
        require(msg.sender == _admin, "Only admin");
        require(_isEmployee[employee], "Not employee");
        require(!_isTerminated[employee], "Already terminated");
        _isTerminated[employee] = true;
        _totalPayroll = _totalPayroll - _salary[employee];
    }

    function updateSalary(address employee, uint256 newSalary) external {
        require(msg.sender == _admin, "Only admin");
        require(_isEmployee[employee], "Not employee");
        require(!_isTerminated[employee], "Terminated");
        uint256 oldSalary = _salary[employee];
        _salary[employee] = newSalary;
        _totalPayroll = _totalPayroll - oldSalary + newSalary;
    }
}

contract PayrollTest is Payroll {
    constructor() Payroll() {}

    function initEmployee(address employee) external {
        _salary[employee] = 0;
        _totalPaid[employee] = 0;
        _paymentCount[employee] = 0;
        _isEmployee[employee] = false;
        _isTerminated[employee] = false;
        _employeeIndex[employee] = 0;
    }
}
