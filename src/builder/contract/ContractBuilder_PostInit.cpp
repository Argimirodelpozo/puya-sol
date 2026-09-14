/// @file PostInitBuilder.cpp
/// Synthesises the deferred-constructor `__postInit` ABI method.
///
/// Deployment framing surrounds the same constructor schedule as inline creation.

#include "builder/contract/ContractBuilder.h"
#include "builder/storage/StateVarWalker.h"
#include "builder/contract/PostInitTriggers.h"
#include "builder/storage/StorageMapper.h"
#include "builder/types/TypeCoercion.h"
#include "builder/types/TypeMapper.h"
#include "builder/context/ProgramAnalysis.h"
#include "builder/storage/slot/EvmSlotLowering.h"
#include "builder/types/ConstructorWirePlan.h"

#include "awst/NameGen.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/Types.h>

namespace puyasol::builder
{

void ContractBuilder::buildPostInitMethod(
	solidity::frontend::ContractDefinition const& _contract,
	std::string const& _contractName,
	awst::ContractMethod& method,
	std::shared_ptr<awst::Block> const& createBlock,
	std::function<void(solidity::frontend::ContractDefinition const&,
		std::vector<std::shared_ptr<awst::Statement>>&)> const& emitStateVarInit)
{
	// Recomputed rather than threaded through: it is just this lookup.
	auto const* constructor = _contract.constructor();
	// Defer all init to __postInit; create call only sets the pending flag.
	auto pendingKey = awst::makeUtf8BytesConstant("__ctor_pending", method.sourceLocation);

	auto one = awst::makeOne(method.sourceLocation);

	auto setPending = awst::makeAppGlobalPut(pendingKey, one, method.sourceLocation);

	auto setPendingStmt = awst::makeExpressionStatement(setPending, method.sourceLocation);
	createBlock->body.push_back(std::move(setPendingStmt));

	// Build __postInit method with deferred constructor body
	auto postInit = awst::ContractMethod(m_contractId, "__postInit",
		awst::WType::voidType(), {}, method.sourceLocation);

	ConstructorWirePlan wire(m_typeMapper, constructor, true);
	std::vector<std::pair<std::string, awst::WType const*>> paramContext;
	for (auto const& parameter: wire.parameters)
	{
		postInit.args.emplace_back(parameter.wireName(), parameter.wireType, method.sourceLocation);
		paramContext.emplace_back(parameter.name, parameter.type);
	}
	setFunctionContext(paramContext, postInit.returnType);

	awst::ARC4ABIMethodConfig postInitConfig;
	postInitConfig.name = "__postInit";
	postInitConfig.sourceLocation = method.sourceLocation;
	postInitConfig.allowedCompletionTypes = {0}; // NoOp
	postInitConfig.create = 3; // Disallow
	postInitConfig.readonly = false;
	postInit.arc4MethodConfig = postInitConfig;

	auto postInitBody = postInit.body;

	// Guard: assert(__ctor_pending == 1)
	auto readPending = awst::makeIntrinsicCall("app_global_get", awst::WType::uint64Type(), method.sourceLocation);
	readPending->stackArgs.push_back(
		awst::makeUtf8BytesConstant("__ctor_pending", method.sourceLocation));

	auto assertStmt = awst::makeExpressionStatement(awst::makeAssert(
		readPending, method.sourceLocation, "__postInit already called"), method.sourceLocation);
	postInitBody->body.push_back(std::move(assertStmt));

	// Guard: CREATOR-ONLY. __postInit is a public ABI method that re-supplies
	// the constructor args and runs the ctor body — an unauthenticated caller
	// front-running the deployer's postInit could capture ownership-style
	// initializers. The __ctor_pending flag only prevents a DOUBLE call.
	// Deploy tooling groups create+postInit from one sender, so the app
	// creator IS the legitimate postInit caller; anyone else reverts.
	{
		auto sender = awst::makeAsBytes(
			awst::makeTxn("Sender", awst::WType::accountType(), method.sourceLocation),
			method.sourceLocation);
		auto creator = awst::makeAsBytes(
			awst::makeGlobal(std::string("CreatorAddress"), awst::WType::accountType(), method.sourceLocation),
			method.sourceLocation);
		auto isCreator = awst::makeBytesComparison(
			std::move(sender), awst::EqualityComparison::Eq, std::move(creator), method.sourceLocation);
		postInitBody->body.push_back(awst::makeExpressionStatement(
			awst::makeAssert(std::move(isCreator), method.sourceLocation,
				"__postInit callable only by the app creator"), method.sourceLocation));
	}

	// Clear flag: __ctor_pending = 0
	auto clearKey = awst::makeUtf8BytesConstant("__ctor_pending", method.sourceLocation);

	auto zeroVal = awst::makeZero(method.sourceLocation);

	auto clearPending = awst::makeAppGlobalPut(clearKey, zeroVal, method.sourceLocation);

	auto clearStmt = awst::makeExpressionStatement(clearPending, method.sourceLocation);
	postInitBody->body.push_back(std::move(clearStmt));

	// EVM profile: ADDRESS ctor params enter the 160-bit namespace
	// (bzero12 ++ low-20) like every decoded address argument and the
	// normalized msg.sender. Deploy tooling passes either the EVM word or
	// a full 32-byte AVM account; keying storage on the RAW form orphans
	// ctor-written state (BORG: ctor mint invisible to transfer).
	if (m_typeMapper.profile().contractAbi == ContractAbi::Evm && constructor)
		for (size_t pi = 0;
			pi < postInit.args.size() && pi < constructor->parameters().size();
			++pi)
		{
			auto const& arg = postInit.args[pi];
			if (arg.wtype != awst::WType::accountType())
				continue;
			auto loc2 = method.sourceLocation;
			auto normalized = awst::makeAsAccount(
				awst::makeConcat(
					awst::makeBzero(12, loc2),
					awst::makeExtractLastN(
						awst::makeAsBytes(
							awst::makeVarExpression(
								arg.name, awst::WType::accountType(), loc2),
							loc2),
						20, loc2),
					loc2),
				loc2);
			postInitBody->body.push_back(awst::makeAssignmentStatement(
				awst::makeVarExpression(
					arg.name, awst::WType::accountType(), loc2),
				std::move(normalized), loc2));
		}

	for (size_t i = 0; i < wire.parameters.size(); ++i)
	{
		auto const& parameter = wire.parameters[i];
		if (parameter.type == parameter.wireType) continue;
		auto value = awst::makeVarExpression(parameter.wireName(), parameter.wireType, method.sourceLocation);
		postInitBody->body.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(parameter.name, parameter.type, method.sourceLocation),
			wire.decodeParameter(i, std::move(value), method.sourceLocation), method.sourceLocation));
	}

	emitBoxCreateForStateVars(*postInitBody, method.sourceLocation);

	emitConstructorPlan(_contract, postInitBody, emitStateVarInit);

	// `--ensure-budget __postInit:N`: __postInit is built here, not
	// through FunctionBuilder's per-method path, so budget injection
	// is explicit. fee_source=1 (AppAccount) — deploy_app doesn't
	// pad extra_fee like a user-driven group, so the contract's funded
	// balance covers the pump's ITxnCreate fees instead (GroupCredit=0
	// would underflow). Pump inserted at the very top so box-init /
	// inline-asm / EIP-712 hashing all draw from the expanded pool.
	if (auto it = m_ensureBudget.find("__postInit");
		it != m_ensureBudget.end() && it->second > 0)
	{
		auto budgetVal = awst::makeIntegerConstant(
			it->second, postInit.sourceLocation);
		auto feeSource = awst::makeIntegerConstant(
			"1", postInit.sourceLocation);
		auto call = awst::makePuyaLibCall("ensure_budget",
			{
				awst::CallArg{std::string("required_budget"), std::move(budgetVal)},
				awst::CallArg{std::string("fee_source"), std::move(feeSource)},
			},
			awst::WType::voidType(), postInit.sourceLocation);
		auto stmt = awst::makeExpressionStatement(
			std::move(call), postInit.sourceLocation);
		postInitBody->body.insert(postInitBody->body.begin(), std::move(stmt));
	}

	m_postInitMethod = std::move(postInit);
}

} // namespace puyasol::builder
