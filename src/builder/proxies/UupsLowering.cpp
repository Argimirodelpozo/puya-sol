/// @file UupsLowering.cpp
/// UUPS (EIP-1822) recognized-idiom folds and the native update gate
/// (proxy.md §3).

#include "builder/proxies/UupsLowering.h"
#include "builder/proxies/Erc1967Lowering.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder::proxies
{

namespace
{

bool inUupsBase(solidity::frontend::FunctionDefinition const& _func)
{
	auto const* scope = dynamic_cast<solidity::frontend::ContractDefinition const*>(
		_func.scope());
	return scope && scope->name() == "UUPSUpgradeable";
}

} // namespace

UupsFold UupsLowering::classify(
	solidity::frontend::FunctionDefinition const& _func)
{
	// OZ Proxy's delegation core (transparent/1967 proxies inherit it).
	if (auto const* scope =
			dynamic_cast<solidity::frontend::ContractDefinition const*>(
				_func.scope());
		scope && scope->name() == "Proxy" && _func.name() == "_delegate")
		return UupsFold::TrapDelegate;
	if (!inUupsBase(_func))
		return UupsFold::None;
	auto const& name = _func.name();
	// The through-the-proxy context checks: no delegated-vs-direct
	// distinction exists on the AVM, so both PASS (proxy.md §3's
	// onlyProxy/notDelegated → constant-true, done at the check functions
	// the modifiers delegate to).
	if (name == "_checkProxy" || name == "_checkNotDelegated")
		return UupsFold::EmptyBody;
	// The in-contract upgrade path: writes the 1967 implementation slot,
	// delegatecalls into the new code — none of it exists here. Trapping the
	// whole family also cuts UUPSUpgradeable's poison off the demand graph
	// (ERC1967Utils' escaped-slot storage runtime, the rescue-mode
	// delegatecall, the ERC-1822 proxiableUUID staticcall probe).
	if (name == "upgradeToAndCall" || name == "upgradeTo"
		|| name == "_upgradeToAndCallUUPS")
		return UupsFold::Trap;
	return UupsFold::None;
}

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

bool UupsLowering::isUupsImplementation(
	solidity::frontend::ContractDefinition const& _contract)
{
	for (auto const* base: _contract.annotation().linearizedBaseContracts)
		if (base && base->name() == "UUPSUpgradeable")
			return true;
	return false;
}

awst::ContractMethod UupsLowering::updateGateMethod(
	std::string const& _cref,
	awst::ContractMethod const& _authorizeMethod,
	awst::SourceLocation const& _loc)
{
	awst::ContractMethod method;
	method.sourceLocation = _loc;
	method.cref = _cref;
	method.memberName = GATE_NAME;
	method.returnType = awst::WType::voidType();

	auto body = awst::makeBlock(_loc);
	// The user's permission hook IS the gate: its inlined modifiers
	// (onlyOwner and friends) and body run inside the UpdateApplication txn.
	// The "new implementation" argument has no meaningful value in the
	// native ceremony (the update txn carries the program); pass this app's
	// own address.
	auto call = awst::makeSubroutineCall(
		awst::SubroutineTarget{
			awst::InstanceMethodTarget{_authorizeMethod.memberName}},
		_authorizeMethod.returnType, _loc);
	if (!_authorizeMethod.args.empty())
	{
		auto const& arg = _authorizeMethod.args[0];
		std::shared_ptr<awst::Expression> self = awst::makeGlobal(
			"CurrentApplicationAddress", awst::WType::accountType(), _loc);
		if (arg.wtype != awst::WType::accountType())
			self = awst::makeReinterpretCast(std::move(self), arg.wtype, _loc);
		awst::pushCallArg(call->args, arg.name, std::move(self));
	}
	body->body.push_back(awst::makeExpressionStatement(std::move(call), _loc));
	body->body.push_back(Erc1967Lowering::upgradedEvent(_loc));
	body->body.push_back(awst::makeReturnStatement(nullptr, _loc));
	method.body = std::move(body);

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
