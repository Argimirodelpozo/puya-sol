/// @file PostInitBuilder.cpp
/// Synthesises the deferred-constructor `__postInit` ABI method.
///
/// Deployment framing surrounds the same constructor schedule as inline creation.

#include "builder/contract/ContractBuilder.h"
#include "builder/contract/StateVarWalker.h"
#include "builder/contract/PostInitTriggers.h"
#include "builder/storage/StorageMapper.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/ProgramAnalysis.h"
#include "builder/sol-ast/EvmSlotLowering.h"
#include "builder/sol-types/SolIntType.h"

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
		awst::ContractMethod postInit;
		postInit.sourceLocation = method.sourceLocation;
		postInit.returnType = awst::WType::voidType();
		postInit.cref = m_contractId;
		postInit.memberName = "__postInit";

		// Mirror constructor params on __postInit so the caller passes the same values.
		if (constructor)
		{
			int paramIdx = 0;
			for (auto const& param: constructor->parameters())
			{
				awst::SubroutineArgument arg;
				arg.name = param->name().empty()
					? "_param" + std::to_string(paramIdx)
					: param->name();
				arg.sourceLocation = method.sourceLocation;
				arg.wtype = m_typeMapper.map(param->type());
				postInit.args.push_back(std::move(arg));
				++paramIdx;
			}
		}

		awst::ARC4ABIMethodConfig postInitConfig;
		postInitConfig.name = "__postInit";
		postInitConfig.sourceLocation = method.sourceLocation;
		postInitConfig.allowedCompletionTypes = {0}; // NoOp
		postInitConfig.create = 3; // Disallow
		postInitConfig.readonly = false;
		postInit.arc4MethodConfig = postInitConfig;

		// Remap biguint→ARC4UIntN and aggregates→ARC4 for correct ABI signature,
		// matching regular method-param remap. Track (origName, arc4Name) for
		// the decode statements emitted at the top of __postInit body.
		struct PostInitDecode { std::string origName; std::string arc4Name; awst::WType const* arc4Type; awst::WType const* origType; };
		std::vector<PostInitDecode> postInitDecodes;
		for (size_t pi = 0; pi < postInit.args.size(); ++pi)
		{
			auto& arg = postInit.args[pi];

			if (arg.wtype == awst::WType::biguintType() && constructor
				&& pi < constructor->parameters().size())
			{
				auto const* solType = constructor->parameters()[pi]->annotation().type;
				auto intInfo = builder::SolIntType::fromSol(solType);
				// Signed stays as biguint (two's-complement); ARC4UIntN would reject it.
				if (intInfo && !intInfo->isSigned)
				{
					unsigned bits = intInfo->bits;
					auto const* arc4Type = m_typeMapper.createType<awst::ARC4UIntN>(static_cast<int>(bits));
					std::string origName = arg.name;
					std::string arc4Name = "__arc4_" + origName;
					postInitDecodes.push_back({origName, arc4Name, arc4Type, arg.wtype});
					arg.name = arc4Name;
					arg.wtype = arc4Type;
					continue;
				}
			}

			bool isAggregate = arg.wtype
				&& (arg.wtype->kind() == awst::WTypeKind::ReferenceArray
					|| arg.wtype->kind() == awst::WTypeKind::ARC4StaticArray
					|| arg.wtype->kind() == awst::WTypeKind::ARC4DynamicArray
					|| arg.wtype->kind() == awst::WTypeKind::WTuple);
			if (isAggregate)
			{
				awst::WType const* arc4Type = m_typeMapper.mapToARC4Type(arg.wtype);
				if (arc4Type != arg.wtype)
					arg.wtype = arc4Type;
			}
		}

		// Function context uses original names+types (remapped biguints resolved via
		// the decoded local emitted below, not the __arc4_* shim arg).
		{
			std::vector<std::pair<std::string, awst::WType const*>> paramContext;
			std::set<std::string> arc4Names;
			for (auto const& d: postInitDecodes)
				arc4Names.insert(d.arc4Name);
			for (auto const& arg: postInit.args)
			{
				if (arc4Names.count(arg.name)) continue; // skip the __arc4_ shim
				paramContext.emplace_back(arg.name, arg.wtype);
			}
			for (auto const& d: postInitDecodes)
				paramContext.emplace_back(d.origName, d.origType);
			setFunctionContext(paramContext, postInit.returnType);
		}

		auto postInitBody = awst::makeBlock(method.sourceLocation);

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

		// Decode each remapped biguint arg: `<origName> = ARC4Decode(__arc4_<origName>)`.
		for (auto const& decode: postInitDecodes)
		{
			auto arc4Var = awst::makeVarExpression(decode.arc4Name, decode.arc4Type, method.sourceLocation);

			auto decodeExpr = awst::makeARC4Decode(std::move(arc4Var), decode.origType, method.sourceLocation);

			auto target = awst::makeVarExpression(decode.origName, decode.origType, method.sourceLocation);

			auto assign = awst::makeAssignmentStatement(std::move(target), std::move(decodeExpr), method.sourceLocation);
			postInitBody->body.push_back(std::move(assign));
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

		postInit.body = postInitBody;
		m_postInitMethod = std::move(postInit);
}

} // namespace puyasol::builder
