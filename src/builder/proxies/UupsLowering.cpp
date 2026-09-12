/// @file UupsLowering.cpp
/// UUPS (EIP-1822) recognized-idiom folds and the native update gate
/// (proxy.md §3).

#include "builder/proxies/UupsLowering.h"
#include "builder/proxies/Erc1967Lowering.h"

namespace puyasol::builder::proxies
{

std::shared_ptr<awst::Block> UupsLowering::foldedBody(
	UupsFold _fold, awst::SourceLocation const& _loc)
{
	auto body = awst::makeBlock(_loc);
	// assert(false) is terminal to puya — no return after a trap.
	if (_fold == UupsFold::Trap)
		body->body.push_back(awst::makeExpressionStatement(
			awst::makeAssert(awst::makeFalse(_loc), _loc,
				"UUPS upgradeToAndCall has no in-contract lowering: the AVM "
				"upgrade is a native UpdateApplication transaction gated by "
				"_authorizeUpgrade (see proxy.md)"),
			_loc));
	else if (_fold == UupsFold::TrapDelegate)
		body->body.push_back(awst::makeExpressionStatement(
			awst::makeAssert(awst::makeFalse(_loc), _loc,
				"proxy delegation has no AVM lowering: the proxy and its "
				"implementation collapse to ONE updatable application — "
				"deploy the implementation contract (see proxy.md)"),
			_loc));
	else
		body->body.push_back(awst::makeReturnStatement(nullptr, _loc));
	return body;
}

awst::ContractMethod UupsLowering::updateGateMethod(
	std::string const& _cref,
	awst::ContractMethod const& _authorizeMethod,
	awst::SourceLocation const& _loc)
{
	auto method = awst::ContractMethod(
		_cref, GATE_NAME, awst::WType::voidType(), {}, _loc);

	auto body = method.body;
	// The user's permission hook IS the gate: its inlined modifiers
	// (onlyOwner and friends) and body run inside the UpdateApplication txn.
	// ProxyFacts has proved this parameter unreferenced, including in modifier
	// arguments and Yul. Zero is only an unused calling-convention placeholder,
	// never a claim about the identity of the proposed native program.
	auto call = awst::makeSubroutineCall(
		awst::SubroutineTarget{
			awst::InstanceMethodTarget{_authorizeMethod.memberName}},
		_authorizeMethod.returnType, _loc);
	auto const& arg = _authorizeMethod.args.at(0);
	awst::pushCallArg(call->args, arg.name,
		awst::makeReinterpretCast(awst::makeBzero(32, _loc), arg.wtype, _loc));
	body->body.push_back(awst::makeExpressionStatement(std::move(call), _loc));
	body->body.push_back(Erc1967Lowering::upgradedEvent(_loc));
	body->body.push_back(awst::makeReturnStatement(nullptr, _loc));

	// ABI (not bare) for the same reasons as the 1967 gate: ARC-56 event
	// aggregation and a declared update surface. UpdateApplication only,
	// never on create.
	awst::ARC4ABIMethodConfig config;
	config.sourceLocation = _loc;
	config.allowedCompletionTypes = {4};
	config.create = 3;
	config.name = GATE_NAME;
	method.arc4MethodConfig = config;
	return method;
}

} // namespace puyasol::builder::proxies
