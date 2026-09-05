// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

abstract contract PlanBase {
    uint64 public total;
    modifier addBase(uint64 amount) { total += amount; _; total += amount * 10; }
    constructor(uint64 amount) addBase(amount) {}
}

abstract contract PlanMiddle is PlanBase {
    constructor(uint64 amount) PlanBase(amount + 1) { total += amount * 100; }
}

contract InlineConstructorPlan is PlanMiddle {
    uint64 public observed = total;
    modifier addDerived() { total += 4; _; total += 40; }
    constructor() PlanMiddle(2) addDerived() {}
}

contract DeferredConstructorPlan is PlanMiddle {
    uint64[] unused;
    uint64 public observed = total;
    modifier addDerived() { total += 4; _; total += 40; }
    constructor() PlanMiddle(2) addDerived() {}
}
