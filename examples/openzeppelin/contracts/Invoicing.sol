// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

abstract contract Invoicing {
    address private _admin;
    uint256 private _invoiceCount;
    uint256 private _totalBilled;
    uint256 private _totalPaid;
    uint256 private _totalCancelled;

    mapping(uint256 => address) internal _invoicePayer;
    mapping(uint256 => uint256) internal _invoiceAmount;
    mapping(uint256 => uint256) internal _invoicePaid;
    mapping(uint256 => uint256) internal _invoiceDueDate;
    mapping(uint256 => bool) internal _invoiceCancelled;

    constructor() {
        _admin = msg.sender;
        _invoiceCount = 0;
        _totalBilled = 0;
        _totalPaid = 0;
        _totalCancelled = 0;
    }

    function getAdmin() external view returns (address) {
        return _admin;
    }

    function getInvoiceCount() external view returns (uint256) {
        return _invoiceCount;
    }

    function getTotalBilled() external view returns (uint256) {
        return _totalBilled;
    }

    function getTotalPaid() external view returns (uint256) {
        return _totalPaid;
    }

    function getTotalCancelled() external view returns (uint256) {
        return _totalCancelled;
    }

    function getInvoicePayer(uint256 invoiceId) external view returns (address) {
        return _invoicePayer[invoiceId];
    }

    function getInvoiceAmount(uint256 invoiceId) external view returns (uint256) {
        return _invoiceAmount[invoiceId];
    }

    function getInvoicePaid(uint256 invoiceId) external view returns (uint256) {
        return _invoicePaid[invoiceId];
    }

    function getInvoiceDueDate(uint256 invoiceId) external view returns (uint256) {
        return _invoiceDueDate[invoiceId];
    }

    function isInvoiceCancelled(uint256 invoiceId) external view returns (bool) {
        return _invoiceCancelled[invoiceId];
    }

    function getInvoiceBalance(uint256 invoiceId) external view returns (uint256) {
        return _invoiceAmount[invoiceId] - _invoicePaid[invoiceId];
    }

    function isInvoiceFullyPaid(uint256 invoiceId) external view returns (bool) {
        return _invoicePaid[invoiceId] >= _invoiceAmount[invoiceId];
    }

    function createInvoice(address payer, uint256 amount, uint256 dueDate) external returns (uint256) {
        uint256 invoiceId = _invoiceCount;
        _invoicePayer[invoiceId] = payer;
        _invoiceAmount[invoiceId] = amount;
        _invoicePaid[invoiceId] = 0;
        _invoiceDueDate[invoiceId] = dueDate;
        _invoiceCancelled[invoiceId] = false;
        _invoiceCount = invoiceId + 1;
        _totalBilled = _totalBilled + amount;
        return invoiceId;
    }

    function payInvoice(uint256 invoiceId, uint256 amount) external {
        require(!_invoiceCancelled[invoiceId], "Invoice is cancelled");
        uint256 remaining = _invoiceAmount[invoiceId] - _invoicePaid[invoiceId];
        uint256 payment = amount;
        if (payment > remaining) {
            payment = remaining;
        }
        _invoicePaid[invoiceId] = _invoicePaid[invoiceId] + payment;
        _totalPaid = _totalPaid + payment;
    }

    function cancelInvoice(uint256 invoiceId) external {
        require(!_invoiceCancelled[invoiceId], "Already cancelled");
        require(_invoicePaid[invoiceId] < _invoiceAmount[invoiceId], "Already fully paid");
        _invoiceCancelled[invoiceId] = true;
        uint256 remaining = _invoiceAmount[invoiceId] - _invoicePaid[invoiceId];
        _totalCancelled = _totalCancelled + remaining;
    }

    function isOverdue(uint256 invoiceId, uint256 currentTime) external view returns (bool) {
        if (_invoiceCancelled[invoiceId]) {
            return false;
        }
        if (_invoicePaid[invoiceId] >= _invoiceAmount[invoiceId]) {
            return false;
        }
        return currentTime > _invoiceDueDate[invoiceId];
    }
}

contract InvoicingTest is Invoicing {
    constructor() Invoicing() {}

    function initInvoice(uint256 invoiceId) external {
        _invoicePayer[invoiceId] = address(0);
        _invoiceAmount[invoiceId] = 0;
        _invoicePaid[invoiceId] = 0;
        _invoiceDueDate[invoiceId] = 0;
        _invoiceCancelled[invoiceId] = false;
    }
}
