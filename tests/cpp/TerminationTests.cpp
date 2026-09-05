#include "awst/Termination.hpp"

#include <iostream>

using namespace puyasol::awst;

namespace
{
bool require(bool condition, char const* message)
{
	if (!condition) std::cerr << message << '\n';
	return condition;
}

std::shared_ptr<Block> returningBlock()
{
	auto block = makeBlock({});
	block->body = {makeReturnStatement(nullptr, {}), makeBlock({})};
	return block;
}

std::shared_ptr<Switch> returningSwitch()
{
	auto branch = std::make_shared<Switch>();
	branch->value = makeZero({});
	branch->cases.emplace_back(makeZero({}), returningBlock());
	branch->defaultCase = returningBlock();
	return branch;
}
}

int main()
{
	bool ok = true;
	auto branch = returningSwitch();
	auto block = makeBlock({});
	block->body = {branch, makeBlock({})};
	ok &= require(blockAlwaysTerminates(*block), "exhaustive switch must terminate before cleanup");
	removeDeadCode(block->body);
	ok &= require(block->body.size() == 1, "remove the tail after an exhaustive switch");
	ok &= require(branch->cases[0].second->body.size() == 1
		&& branch->defaultCase->body.size() == 1, "clean case and default bodies");

	branch->defaultCase.reset();
	block->body.push_back(makeBlock({}));
	ok &= require(!blockAlwaysTerminates(*block), "switch without default must fall through");
	removeDeadCode(block->body);
	ok &= require(block->body.size() == 2, "retain the tail after a non-exhaustive switch");
	branch->defaultCase = makeBlock({});
	ok &= require(!blockAlwaysTerminates(*block), "fallthrough default must not terminate");

	auto nested = makeIfElse(makeTrue({}), returningBlock(), returningBlock(), {});
	branch->defaultCase->body = {nested};
	ok &= require(blockAlwaysTerminates(*block), "nested returning branches must terminate");
	removeDeadCode(block->body);
	ok &= require(block->body.size() == 1, "nested termination must prune outer tail");

	for (auto transfer: {std::shared_ptr<Statement>(makeLoopExit({})),
		std::shared_ptr<Statement>(makeLoopContinue({}))})
	{
		auto body = makeBlock({});
		body->body = {transfer, makeReturnStatement(nullptr, {})};
		auto loop = makeWhileLoop(makeTrue({}), body, {});
		block->body = {loop, makeBlock({})};
		removeDeadCode(block->body);
		ok &= require(body->body.size() == 1, "prune after loop transfers");
		ok &= require(block->body.size() == 2 && !blockAlwaysTerminates(*block),
			"loop-body termination must not escape the loop");
	}

	auto forLoop = std::make_shared<ForInLoop>();
	forLoop->loopBody = makeBlock({});
	forLoop->loopBody->body = {returningSwitch(), makeBlock({})};
	block->body = {forLoop, makeBlock({})};
	removeDeadCode(block->body);
	ok &= require(forLoop->loopBody->body.size() == 1 && block->body.size() == 2,
		"for-in traversal must clean nested switches without assuming an iteration");
	return ok ? 0 : 1;
}
