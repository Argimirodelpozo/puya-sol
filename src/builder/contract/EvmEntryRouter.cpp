#include "builder/contract/ContractBuilder.h"
#include "builder/AwstShorthand.h"
#include "builder/XchainAccounts.h"

#include "builder/contract/RouterConditions.h"

#include "Logger.h"
#include "builder/ProgramAnalysis.h"
#include "builder/SolcFacts.h"
#include "builder/abi/EvmAbiDecode.h"
#include "builder/abi/EvmAbiEncode.h"
#include "builder/codec/EvmValueCodec.h"
#include "builder/sol-types/OverloadSuffix.h"
#include "builder/sol-types/TypeMapper.h"

#include <libsolidity/ast/AST.h>

#include <functional>

namespace puyasol::builder
{
using namespace puyasol::builder::shorthand;
using namespace solidity::frontend;

namespace
{
std::string methodNameFor(
	FunctionType const& function, OverloadedNamesSet const& overloaded)
{
	if (!function.hasDeclaration())
		return {};
	if (auto const* definition =
			dynamic_cast<FunctionDefinition const*>(&function.declaration()))
	{
		std::string name = definition->name();
		if (overloaded.count(name))
			appendOverloadSuffix(name, *definition);
		return name;
	}
	if (auto const* variable =
			dynamic_cast<VariableDeclaration const*>(&function.declaration()))
		return variable->name();
	return {};
}

awst::ContractMethod* findMethod(awst::Contract& contract, std::string const& name)
{
	for (auto& method: contract.methods)
		if (method.memberName == name)
			return &method;
	return nullptr;
}

void emitReturnLog(
	std::shared_ptr<awst::Expression> payload,
	awst::SourceLocation const& loc,
	std::vector<std::shared_ptr<awst::Statement>>& out)
{
	auto log = awst::makeIntrinsicCall("log", awst::WType::voidType(), loc);
	log->stackArgs.push_back(awst::makeConcat(
		awst::makeBytesConstant({0x15, 0x1f, 0x7c, 0x75}, loc),
		std::move(payload), loc));
	out.push_back(awst::makeExpressionStatement(std::move(log), loc));
}

void emitNonPayableCheck(
	awst::SourceLocation const& loc,
	std::vector<std::shared_ptr<awst::Statement>>& out)
{
	// The normal method-body guard is keyed by the ARC4 selector. EVM entry
	// routes have a different selector, so enforce the same payment rule at the
	// adapter while the selected external function is known.
	out.push_back(awst::makeExpressionStatement(
		awst::makeAssert(
			awst::makeNumericCompare(makeMsgValueAmount(loc),
				awst::NumericComparison::Eq, u64(0, loc), loc),
			loc, "not payable"), loc));
}

/// Emit `callsub __evm_npy` — the shared non-payable guard.
void emitNonPayableCall(
	awst::SourceLocation const& loc,
	std::vector<std::shared_ptr<awst::Statement>>& out)
{
	auto call = awst::makeSubroutineCall(
		awst::InstanceMethodTarget{"__evm_npy"}, awst::WType::voidType(), loc);
	out.push_back(awst::makeExpressionStatement(std::move(call), loc));
}

/// Synthesize the shared EVM-entry helper subroutines once per contract:
///   __evm_npy() — the non-payable guard every non-payable arm runs;
///   __evm_decw(__off) — bounds-checked 32-byte word fetch from
///                       ApplicationArgs[1] (the EVM calldata body).
/// Emitting these inline per arm made a 55-method contract spend hundreds of
/// lines repeating them; puya strips whichever helper ends up uncalled.
void synthesizeEvmEntryHelpers(
	awst::Contract& contract, awst::SourceLocation const& loc)
{
	if (contract.methods.empty() || findMethod(contract, "__evm_npy"))
		return;
	std::string cref = contract.methods.front().cref;
	{
		awst::ContractMethod sub;
		sub.sourceLocation = loc;
		sub.cref = cref;
		sub.memberName = "__evm_npy";
		sub.returnType = awst::WType::voidType();
		sub.arc4MethodConfig = std::nullopt;
		auto body = awst::makeBlock(loc);
		emitNonPayableCheck(loc, body->body);
		body->body.push_back(awst::makeReturnStatement(nullptr, loc));
		sub.body = std::move(body);
		contract.methods.push_back(std::move(sub));
	}
	{
		awst::ContractMethod sub;
		sub.sourceLocation = loc;
		sub.cref = cref;
		sub.memberName = "__evm_decw";
		sub.returnType = awst::WType::bytesType();
		sub.arc4MethodConfig = std::nullopt;
		awst::SubroutineArgument offArg;
		offArg.name = "__off";
		offArg.wtype = awst::WType::uint64Type();
		offArg.sourceLocation = loc;
		sub.args.push_back(offArg);
		auto off = [&]() {
			return awst::makeVarExpression(
				"__off", awst::WType::uint64Type(), loc);
		};
		auto body = awst::makeBlock(loc);
		body->body.push_back(awst::makeExpressionStatement(
			awst::makeAssert(
				awst::makeNumericCompare(
					awst::makeUInt64BinOp(off(),
						awst::UInt64BinaryOperator::Add, u64(32, loc), loc),
					awst::NumericComparison::Lte,
					awst::makeLen(awst::makeAppArg(1, loc), loc), loc),
				loc, "EVM ABI decode out of bounds"), loc));
		body->body.push_back(awst::makeReturnStatement(
			awst::makeExtract3(
				awst::makeAppArg(1, loc), off(), u64(32, loc), loc), loc));
		sub.body = std::move(body);
		contract.methods.push_back(std::move(sub));
	}
	// __evm_deco(__off) — offset/length small word: decw + high-24-zero
	// assert + narrow. __evm_arga(__off) — address leaf: decw + padding
	// assert. Each dynamic arg repeats the former, each address arg the
	// latter; one body apiece (puya strips whichever ends up uncalled).
	{
		awst::ContractMethod sub;
		sub.sourceLocation = loc;
		sub.cref = cref;
		sub.memberName = "__evm_deco";
		sub.returnType = awst::WType::uint64Type();
		sub.arc4MethodConfig = std::nullopt;
		awst::SubroutineArgument offArg;
		offArg.name = "__off";
		offArg.wtype = awst::WType::uint64Type();
		offArg.sourceLocation = loc;
		sub.args.push_back(offArg);
		auto body = awst::makeBlock(loc);
		auto fetch = awst::makeSubroutineCall(
			awst::InstanceMethodTarget{"__evm_decw"},
			awst::WType::bytesType(), loc);
		awst::pushCallArg(fetch->args, awst::makeVarExpression(
			"__off", awst::WType::uint64Type(), loc));
		auto value = awst::makeEvalOnce(std::move(fetch), loc);
		body->body.push_back(awst::makeExpressionStatement(
			awst::makeAssert(
				awst::makeNumericCompare(
					awst::makeAsBiguint(
						awst::makeExtract(value, 0, 24, loc), loc),
					awst::NumericComparison::Eq,
					awst::makeIntegerConstant("0", loc,
						awst::WType::biguintType()), loc),
				loc, "EVM ABI offset exceeds uint64"), loc));
		body->body.push_back(awst::makeReturnStatement(
			awst::makeWord32ToUInt64(value, loc), loc));
		sub.body = std::move(body);
		contract.methods.push_back(std::move(sub));
	}
	{
		awst::ContractMethod sub;
		sub.sourceLocation = loc;
		sub.cref = cref;
		sub.memberName = "__evm_arga";
		sub.returnType = awst::WType::accountType();
		sub.arc4MethodConfig = std::nullopt;
		awst::SubroutineArgument offArg;
		offArg.name = "__off";
		offArg.wtype = awst::WType::uint64Type();
		offArg.sourceLocation = loc;
		sub.args.push_back(offArg);
		auto body = awst::makeBlock(loc);
		auto fetch = awst::makeSubroutineCall(
			awst::InstanceMethodTarget{"__evm_decw"},
			awst::WType::bytesType(), loc);
		awst::pushCallArg(fetch->args, awst::makeVarExpression(
			"__off", awst::WType::uint64Type(), loc));
		auto value = awst::makeEvalOnce(std::move(fetch), loc);
		body->body.push_back(awst::makeExpressionStatement(
			awst::makeAssert(
				awst::makeBytesComparison(
					awst::makeExtract(value, 0, 12, loc),
					awst::EqualityComparison::Eq, awst::makeBzero(12, loc), loc),
				loc, "invalid EVM ABI address padding"), loc));
		body->body.push_back(awst::makeReturnStatement(
			awst::makeAsAccount(value, loc), loc));
		sub.body = std::move(body);
		contract.methods.push_back(std::move(sub));
	}
}

struct EvmRoute
{
	FunctionType const* function = nullptr;
	awst::ContractMethod* method = nullptr;
	std::vector<uint8_t> selector;
};

/// Collect one EVM route per external Solidity function. In `quiet` mode a
/// method that cannot carry an EVM route is SKIPPED rather than an error: the
/// ARC-4 profile mounts these routes as a compatibility alias next to its
/// native router, so a non-EVM-able method merely has no alias — its ARC-4
/// route still serves it. The EVM profile has no other transport, so there the
/// same conditions stay hard errors.
std::vector<EvmRoute> collectEvmRoutes(
	ContractDefinition const& contractDefinition,
	awst::Contract& contract,
	OverloadedNamesSet const& overloadedNames,
	awst::SourceLocation const& loc,
	bool quiet,
	ProgramAnalysis const* analysis = nullptr)
{
	std::vector<EvmRoute> routes;
	for (auto const& [_, function]: contractDefinition.interfaceFunctionList(true))
	{
		if (!function)
			continue;
		auto name = methodNameFor(*function, overloadedNames);
		auto* method = findMethod(contract, name);
		if (!method)
		{
			if (!quiet)
				Logger::instance().error(
					"cannot build EVM entry route for Solidity function '"
						+ function->externalSignature() + "': generated method '"
						+ name + "' was not found", loc);
			continue;
		}
		if (!abi::canDecodeEvmAbi(function->parameterTypes())
			|| !abi::canEncodeEvmAbi(function->returnParameterTypes()))
		{
			if (!quiet)
				Logger::instance().error(
					"Solidity ABI entry route contains a type unsupported by the "
					"canonical recursive codec: " + function->externalSignature(), loc);
			continue;
		}
		// canEncodeEvmAbi answers the TYPE question, but emitting an external
		// function pointer additionally needs --evm-selectors (the default
		// profile's compact pointer stores the ARC-4 route, not the Solidity
		// selector, so the codec hard-errors). In quiet/alias mode such a method
		// simply keeps only its ARC-4 route.
		// Alias arms only cover methods whose LOWERING round-trips the EVM
		// transport. A body containing inline assembly does not yet: its
		// `assembly { return(...) }` / blob-pointer conventions are validated
		// for the inline ARC-4 path only, and dispatching one through an arm
		// reverted on the blob memory bound (the chainwide Aave stubs, whose
		// every method leads with the scripted-answer asm return). Skipping
		// keeps such methods ARC-4-only; an EVM-selector caller lands in the
		// fallback — exactly where it landed before the arms existed. Same
		// gate and same rationale as the ARC-4 param remap's asm exclusion.
		if (quiet && analysis && function->hasDeclaration()
			&& analysis->callablesWithInlineAssembly.count(
				function->declaration().id()))
			continue;
		if (quiet)
		{
			std::function<bool(Type const*)> needsEvmSelectors =
				[&](Type const* type) -> bool {
					if (!type) return false;
					if (auto const* fn = dynamic_cast<FunctionType const*>(type))
						return fn->kind() == FunctionType::Kind::External;
					if (auto const* arr = dynamic_cast<ArrayType const*>(type))
						return needsEvmSelectors(arr->baseType());
					if (auto const* st = dynamic_cast<StructType const*>(type))
					{
						for (auto const& member: st->structDefinition().members())
							if (needsEvmSelectors(member->type()))
								return true;
						return false;
					}
					return false;
				};
			bool blocked = false;
			for (auto const* type: function->parameterTypes())
				blocked = blocked || needsEvmSelectors(type);
			for (auto const* type: function->returnParameterTypes())
				blocked = blocked || needsEvmSelectors(type);
			if (blocked)
				continue;
		}
		routes.push_back({function, method,
			SolcFacts::externalSelector(*function)});
	}
	return routes;
}

/// Group key for a route's return tail: canonical Solidity return signature
/// + the method's wire return WType identity (createType canonicalizes, so
/// pointer equality is type equality).
std::string evmRetTailKey(EvmRoute const& route)
{
	std::string key;
	for (auto const* type: route.function->returnParameterTypes())
		key += type->canonicalName() + ",";
	key += "#";
	key += std::to_string(
		reinterpret_cast<uintptr_t>(route.method->returnType));
	return key;
}

/// Synthesize shared per-return-shape encode+log tails (`__evm_ret<i>`) —
/// each arm's EVM-encode + carrier-log epilogue, outlined once per distinct
/// (return signature, wire type) used by 2+ routes. Same economics as
/// __evm_decw; the arm keeps its own `return 1` (program exit). Routes whose
/// key has no tail (singletons, non-tuple multi-returns) keep the inline
/// epilogue.
std::map<std::string, std::string> synthesizeEvmReturnTails(
	TypeMapper& typeMapper,
	awst::Contract& contract,
	std::vector<EvmRoute> const& probeRoutes,
	awst::SourceLocation const& loc)
{
	struct TailSpec
	{
		std::vector<Type const*> returnTypes;
		awst::WType const* retW = nullptr;
		int uses = 0;
	};
	std::map<std::string, TailSpec> groups;
	for (auto const& route: probeRoutes)
	{
		auto& spec = groups[evmRetTailKey(route)];
		if (spec.uses == 0)
		{
			spec.returnTypes = route.function->returnParameterTypes();
			spec.retW = route.method->returnType;
		}
		spec.uses++;
	}

	std::map<std::string, std::string> tailByKey;
	if (contract.methods.empty())
		return tailByKey;
	std::string cref = contract.methods.front().cref;
	int index = 0;
	for (auto& [key, spec]: groups)
	{
		if (spec.uses < 2)
			continue;
		if (spec.returnTypes.size() > 1
			&& !dynamic_cast<awst::WTuple const*>(spec.retW))
			continue;   // unexpected wire shape — keep those arms inline

		std::string name = "__evm_ret" + std::to_string(index++);
		awst::ContractMethod sub;
		sub.sourceLocation = loc;
		sub.cref = cref;
		sub.memberName = name;
		sub.returnType = awst::WType::voidType();
		sub.arc4MethodConfig = std::nullopt;
		auto body = awst::makeBlock(loc);
		std::vector<std::shared_ptr<awst::Expression>> returnValues;
		if (!spec.returnTypes.empty())
		{
			awst::SubroutineArgument arg;
			arg.name = "__v";
			arg.wtype = spec.retW;
			arg.sourceLocation = loc;
			sub.args.push_back(std::move(arg));
			auto v = [&]() {
				return awst::makeVarExpression("__v", spec.retW, loc);
			};
			if (spec.returnTypes.size() == 1)
				returnValues.push_back(v());
			else
			{
				auto const* tuple = dynamic_cast<awst::WTuple const*>(spec.retW);
				for (size_t i = 0; i < spec.returnTypes.size(); ++i)
					returnValues.push_back(awst::makeTupleItem(
						v(), static_cast<int>(i), tuple->types()[i], loc));
			}
		}
		auto encoded = abi::encodeEvmAbi(
			typeMapper, spec.returnTypes, std::move(returnValues), loc,
			body->body);
		emitReturnLog(std::move(encoded), loc, body->body);
		body->body.push_back(awst::makeReturnStatement(nullptr, loc));
		sub.body = std::move(body);
		contract.methods.push_back(std::move(sub));
		tailByKey[key] = name;
	}
	return tailByKey;
}

/// Emit one guarded dispatch arm for an EVM route into `sink`:
///   OnCompletion==NoOp && NumAppArgs==2 && Args[0]==keccak4(signature)
///   -> non-payable check, EVM-decode Args[1], call, EVM-encode + return log.
/// Shared verbatim by the EVM entry profile and the ARC-4 profile's
/// compatibility alias mount.
/// One EVM route's arm BODY (non-payable check, calldata decode, dispatch,
/// return encode + log). The transaction-shape/selector guards live in the
/// shared arm SWITCH below — emitting them per arm made a 55-method contract
/// spend ~half its program on sequential selector compares.
std::shared_ptr<awst::Block> buildEvmArmBody(
	TypeMapper& typeMapper,
	EvmRoute const& route,
	std::map<std::string, std::string> const& retTails,
	awst::SourceLocation const& loc)
{
	auto const& paramTypes = route.function->parameterTypes();
	auto const& returnTypes = route.function->returnParameterTypes();
	auto body = awst::makeBlock(loc);
	if (!route.function->isPayable())
		emitNonPayableCall(loc, body->body);

	std::vector<std::shared_ptr<awst::Expression>> values;
	if (!paramTypes.empty())
	{
		awst::WType const* decodedType = nullptr;
		if (paramTypes.size() == 1)
			decodedType = typeMapper.map(paramTypes[0]);
		else
		{
			std::vector<awst::WType const*> tupleTypes;
			for (auto const* type: paramTypes)
				tupleTypes.push_back(typeMapper.map(type));
			decodedType = typeMapper.createType<awst::WTuple>(
				std::move(tupleTypes));
		}
		auto decoded = abi::decodeEvmAbi(
			typeMapper, awst::makeAppArg(1, loc), paramTypes,
			decodedType, loc, body->body, "__evm_decw", "__evm_deco",
			"__evm_arga");
		if (paramTypes.size() == 1)
			values.push_back(std::move(decoded));
		else
		{
			auto once = awst::makeEvalOnce(std::move(decoded), loc);
			auto const* tuple = dynamic_cast<awst::WTuple const*>(decodedType);
			for (size_t i = 0; i < paramTypes.size(); ++i)
				values.push_back(awst::makeTupleItem(
					once, static_cast<int>(i), tuple->types()[i], loc));
		}
	}

	auto call = awst::makeSubroutineCall(
		awst::InstanceMethodTarget{route.method->memberName},
		route.method->returnType, loc);
	for (size_t i = 0; i < values.size(); ++i)
	{
		auto value = std::move(values[i]);
		auto const* expected = i < route.method->args.size()
			? route.method->args[i].wtype : value->wtype;
		if (value->wtype != expected)
			value = codec::valueToArc4(
				typeMapper, paramTypes[i], std::move(value), expected, loc);
		awst::pushCallArg(call->args, std::move(value));
	}

	// Shared tail: `callsub __evm_ret<i>` replaces the inline encode+log
	// epilogue for return shapes used by 2+ arms.
	if (auto tailIt = retTails.find(evmRetTailKey(route));
		tailIt != retTails.end())
	{
		auto tailCall = awst::makeSubroutineCall(
			awst::InstanceMethodTarget{tailIt->second},
			awst::WType::voidType(), loc);
		if (returnTypes.empty())
		{
			body->body.push_back(awst::makeExpressionStatement(call, loc));
			body->body.push_back(
				awst::makeExpressionStatement(std::move(tailCall), loc));
		}
		else
		{
			awst::pushCallArg(tailCall->args, call);
			body->body.push_back(
				awst::makeExpressionStatement(std::move(tailCall), loc));
		}
		body->body.push_back(
			awst::makeReturnStatement(awst::makeTrue(loc), loc));
		return body;
	}

	std::vector<std::shared_ptr<awst::Expression>> returnValues;
	if (returnTypes.empty())
	{
		body->body.push_back(awst::makeExpressionStatement(call, loc));
	}
	else if (returnTypes.size() == 1)
		returnValues.push_back(call);
	else
	{
		auto once = awst::makeEvalOnce(call, loc);
		auto const* tuple = dynamic_cast<awst::WTuple const*>(
			route.method->returnType);
		for (size_t i = 0; i < returnTypes.size(); ++i)
			returnValues.push_back(awst::makeTupleItem(
				once, static_cast<int>(i), tuple->types()[i], loc));
	}
	auto encoded = abi::encodeEvmAbi(
		typeMapper, returnTypes, std::move(returnValues), loc, body->body);
	emitReturnLog(std::move(encoded), loc, body->body);
	body->body.push_back(awst::makeReturnStatement(awst::makeTrue(loc), loc));
	return body;
}

/// ONE guard + ONE selector match table for every EVM arm:
///   if (NoOp && NumAppArgs == 2) switch (Args[0]) { case sel_i: <arm_i> }
/// puya lowers the constant-case Switch to a pushbytess/match table (the
/// ARC-4 router's own shape). An unmatched selector falls through the switch
/// to whatever dispatch follows, exactly like the old per-arm if-chain.
void emitEvmArmSwitch(
	TypeMapper& typeMapper,
	std::vector<EvmRoute> const& routes,
	std::map<std::string, std::string> const& retTails,
	std::vector<std::shared_ptr<awst::Statement>>& sink,
	awst::SourceLocation const& loc)
{
	if (routes.empty())
		return;
	auto switchNode = std::make_shared<awst::Switch>();
	switchNode->sourceLocation = loc;
	switchNode->value = awst::makeAppArg(0, loc);
	for (auto const& route: routes)
		switchNode->cases.emplace_back(
			awst::makeBytesConstant(route.selector, loc,
				awst::BytesEncoding::Base16, awst::WType::bytesType()),
			buildEvmArmBody(typeMapper, route, retTails, loc));
	auto guarded = awst::makeBlock(loc);
	// xchain account model: ApplicationArgs[2] is an OPTIONAL 20-byte owner
	// claim. Verify it ONCE here — the claimed identity must own THIS sender:
	// sha512_256("Program" || template-with-owner-spliced) == Txn.Sender.
	// msg.sender sites then adopt the claim without re-verifying.
	std::shared_ptr<awst::Expression> argShape = appArgCountIs(2, loc);
	if (auto const& xc = typeMapper.profile().xchainAccounts)
	{
		argShape = awst::makeBoolBinOp(
			std::move(argShape), awst::BinaryBooleanOperator::Or,
			appArgCountIs(3, loc), loc);
		auto verify = awst::makeBlock(loc);
		verify->body.push_back(awst::makeExpressionStatement(
			awst::makeAssert(
				awst::makeNumericCompare(
					awst::makeLen(awst::makeAppArg(2, loc), loc),
					awst::NumericComparison::Eq, u64(20, loc), loc),
				loc, "xchain owner claim must be 20 bytes"),
			loc));
		verify->body.push_back(awst::makeExpressionStatement(
			awst::makeAssert(
				awst::makeBytesComparison(
					awst::makeAsBytes(
						xchain::derivedAccount(*xc, awst::makeAppArg(2, loc), loc),
						loc),
					awst::EqualityComparison::Eq,
					awst::makeAsBytes(
						awst::makeTxn("Sender", awst::WType::accountType(), loc),
						loc),
					loc),
				loc, "xchain owner claim does not match sender"),
			loc));
		guarded->body.push_back(awst::makeIfElse(
			appArgCountIs(3, loc), std::move(verify), nullptr, loc));
	}
	guarded->body.push_back(std::move(switchNode));
	auto condition = awst::makeBoolBinOp(
		isNoOpCall(loc), awst::BinaryBooleanOperator::And,
		std::move(argShape), loc);
	sink.push_back(awst::makeIfElse(
		std::move(condition), std::move(guarded), nullptr, loc));
}
}

void ContractBuilder::emitEvmEntryDispatch(
	ContractDefinition const& contractDefinition,
	awst::Contract& contract)
{
	auto& approval = contract.approvalProgram;
	if (!approval.body)
		return;
	auto const& loc = approval.sourceLocation;

	// Synthesize helpers BEFORE collecting routes: collectEvmRoutes stores
	// ContractMethod pointers, and appending methods afterwards could
	// reallocate the vector under them.
	synthesizeEvmEntryHelpers(contract, loc);
	// Probe pass (quiet) just to group return shapes; the tail subs it
	// appends would invalidate route pointers, so the REAL collect follows.
	std::map<std::string, std::string> retTails;
	{
		auto probe = collectEvmRoutes(
			contractDefinition, contract, m_overloadedNames, loc,
			/*quiet=*/true, nullptr);
		retTails = synthesizeEvmReturnTails(m_typeMapper, contract, probe, loc);
	}
	auto routes = collectEvmRoutes(
		contractDefinition, contract, m_overloadedNames, loc, /*quiet=*/false);
	// The methods remain ordinary callable subroutines, but are no longer
	// advertised to or dispatched by puya's ARC4 router.
	for (auto const& route: routes)
		route.method->arc4MethodConfig.reset();

	// Solidity fallback/receive are owned by this adapter as well. Their full
	// forwarding behavior is added below; suppress accidental ARC4 exposure.
	for (auto& method: contract.methods)
		if (method.memberName == "__fallback" || method.memberName == "__receive")
			method.arc4MethodConfig.reset();
	auto const* fallbackDefinition = contractDefinition.fallbackFunction();
	if (fallbackDefinition && !fallbackDefinition->isImplemented())
		fallbackDefinition = nullptr;
	auto const* receiveDefinition = contractDefinition.receiveFunction();
	if (receiveDefinition && !receiveDefinition->isImplemented())
		receiveDefinition = nullptr;
	auto* fallbackMethod = fallbackDefinition
		? findMethod(contract, "__fallback") : nullptr;
	auto* receiveMethod = receiveDefinition
		? findMethod(contract, "__receive") : nullptr;

	// Compiler-private lifecycle methods (notably __postInit) retain ARC4
	// configs. Give that residual router first refusal without reopening any
	// public Solidity route under ARC4 selectors.
	bool hasResidualArc4Route = false;
	for (auto const& method: contract.methods)
		if (method.arc4MethodConfig)
		{
			hasResidualArc4Route = true;
			break;
		}
	if (hasResidualArc4Route)
	{
		std::string didName = "__evm_entry_arc4_internal";
		auto did = [&]() {
			return awst::makeVarExpression(didName, awst::WType::boolType(), loc);
		};
		approval.body->body.push_back(awst::makeAssignmentStatement(
			did(), awst::makeARC4Router(awst::WType::boolType(), loc), loc));
		auto accepted = awst::makeBlock(loc);
		accepted->body.push_back(awst::makeReturnStatement(awst::makeTrue(loc), loc));
		approval.body->body.push_back(awst::makeIfElse(
			did(), std::move(accepted), nullptr, loc));
	}

	// Empty Solidity calldata selects receive(), or fallback() when receive is
	// absent. Emit the carrier log even for a void handler so low-level callers
	// have one deterministic return record to capture.
	if (receiveMethod || fallbackMethod)
	{
		auto* emptyTarget = receiveMethod ? receiveMethod : fallbackMethod;
		auto const* emptyDefinition = receiveMethod
			? receiveDefinition : fallbackDefinition;
		auto condition = awst::makeBoolBinOp(
			isNoOpCall(loc), awst::BinaryBooleanOperator::And,
			appArgCountIs(0, loc), loc);
		auto body = awst::makeBlock(loc);
		if (emptyDefinition && !emptyDefinition->isPayable())
			emitNonPayableCall(loc, body->body);
		auto call = awst::makeSubroutineCall(
			awst::InstanceMethodTarget{emptyTarget->memberName},
			emptyTarget->returnType, loc);
		if (emptyDefinition && !emptyDefinition->parameters().empty())
			awst::pushCallArg(call->args, awst::makeBytesConstant({}, loc));
		if (emptyTarget->returnType == awst::WType::voidType())
		{
			body->body.push_back(awst::makeExpressionStatement(call, loc));
			emitReturnLog(awst::makeBytesConstant({}, loc), loc, body->body);
		}
		else
			emitReturnLog(call, loc, body->body);
		body->body.push_back(awst::makeReturnStatement(awst::makeTrue(loc), loc));
		approval.body->body.push_back(awst::makeIfElse(
			std::move(condition), std::move(body), nullptr, loc));
	}

	emitEvmArmSwitch(m_typeMapper, routes, retTails, approval.body->body, loc);

	// Unmatched non-empty calldata selects fallback(). Reconstruct exactly the
	// Solidity byte stream from the AVM carrier split: selector ++ ABI body.
	if (fallbackMethod)
	{
		auto hasSelector = awst::makeNumericCompare(
			awst::makeTxn("NumAppArgs", awst::WType::uint64Type(), loc),
			awst::NumericComparison::Gt, u64(0, loc), loc);
		auto hasBody = awst::makeNumericCompare(
			awst::makeTxn("NumAppArgs", awst::WType::uint64Type(), loc),
			awst::NumericComparison::Gt, u64(1, loc), loc);
		auto bodyBytes = awst::makeConditional(
			std::move(hasBody), awst::makeAppArg(1, loc),
			awst::makeBytesConstant({}, loc), awst::WType::bytesType(), loc);
		auto calldata = awst::makeConditional(
			std::move(hasSelector),
			awst::makeConcat(awst::makeAppArg(0, loc), std::move(bodyBytes), loc),
			awst::makeBytesConstant({}, loc), awst::WType::bytesType(), loc);
		auto carrierShape = awst::makeBoolBinOp(
			awst::makeNumericCompare(
				awst::makeTxn("NumAppArgs", awst::WType::uint64Type(), loc),
				awst::NumericComparison::Gt, u64(0, loc), loc),
			awst::BinaryBooleanOperator::And,
			awst::makeNumericCompare(
				awst::makeTxn("NumAppArgs", awst::WType::uint64Type(), loc),
				awst::NumericComparison::Lte, u64(2, loc), loc), loc);
		auto condition = awst::makeBoolBinOp(
			isNoOpCall(loc), awst::BinaryBooleanOperator::And,
			std::move(carrierShape), loc);
		auto fallbackBody = awst::makeBlock(loc);
		if (fallbackDefinition && !fallbackDefinition->isPayable())
			emitNonPayableCall(loc, fallbackBody->body);
		auto call = awst::makeSubroutineCall(
			awst::InstanceMethodTarget{fallbackMethod->memberName},
			fallbackMethod->returnType, loc);
		if (fallbackDefinition && !fallbackDefinition->parameters().empty())
			awst::pushCallArg(call->args, std::move(calldata));
		if (fallbackMethod->returnType == awst::WType::voidType())
		{
			fallbackBody->body.push_back(
				awst::makeExpressionStatement(call, loc));
			emitReturnLog(awst::makeBytesConstant({}, loc), loc,
				fallbackBody->body);
		}
		else
			emitReturnLog(call, loc, fallbackBody->body);
		fallbackBody->body.push_back(
			awst::makeReturnStatement(awst::makeTrue(loc), loc));
		approval.body->body.push_back(awst::makeIfElse(
			std::move(condition), std::move(fallbackBody), nullptr, loc));
	}

	// No matching Solidity selector and no compiler-private ARC4 route.
	approval.body->body.push_back(
		awst::makeReturnStatement(awst::makeFalse(loc), loc));
}


void ContractBuilder::emitEvmCompatRoutes(
	solidity::frontend::ContractDefinition const& contractDefinition,
	awst::Contract& contract)
{
	auto& approval = contract.approvalProgram;
	if (!approval.body)
		return;
	auto const& loc = approval.sourceLocation;

	// ARC-4 profile: the abi.* builtins emit canonical EVM calldata in every
	// profile, so `target.call(abi.encodeWithSignature(...))` reaches an
	// ARC-4-routed callee carrying a keccak selector — which the native router
	// errs on. Mount the same EVM route arms the --contract-abi evm profile
	// uses AS AN ALIAS, ahead of the untouched ARC-4 router: every arm is
	// guarded on NumAppArgs==2 (the [selector, body] carrier the low-level
	// call lowering emits), so native ARC-4 traffic is dispatched exactly as
	// before. arc4MethodConfigs are NOT reset — ARC-4 stays the primary
	// transport; methods whose types cannot round-trip the EVM codec simply
	// have no alias (quiet mode) and keep their ARC-4 route.
	// Helpers go in BEFORE route collection (pointer stability, see
	// emitEvmEntryDispatch); puya strips them when no arm ends up calling.
	synthesizeEvmEntryHelpers(contract, loc);
	std::map<std::string, std::string> retTails;
	{
		auto probe = collectEvmRoutes(
			contractDefinition, contract, m_overloadedNames, loc,
			/*quiet=*/true, &m_typeMapper.analysis());
		retTails = synthesizeEvmReturnTails(m_typeMapper, contract, probe, loc);
	}
	auto routes = collectEvmRoutes(
		contractDefinition, contract, m_overloadedNames, loc, /*quiet=*/true,
		&m_typeMapper.analysis());
	emitEvmArmSwitch(m_typeMapper, routes, retTails, approval.body->body, loc);
}

} // namespace puyasol::builder
