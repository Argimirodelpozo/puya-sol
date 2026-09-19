#include "builder/contract/ContractBuilder.h"
#include "builder/AwstShorthand.h"
#include "builder/target/XchainAccounts.h"

#include "builder/contract/RouterConditions.h"
#include "builder/contract/SelectorRouter.h"

#include "Logger.h"
#include "builder/context/ProgramAnalysis.h"
#include "builder/context/BuildArtifacts.h"
#include "builder/solc/SolcFacts.h"
#include "builder/codec/EvmAbiDecode.h"
#include "builder/lowering/itxn/ApplicationCall.h"
#include "builder/codec/EvmAbiEncode.h"
#include "builder/codec/EvmValueCodec.h"
#include "builder/codec/SelectorSemantics.h"
#include "builder/solc/OverloadSuffix.h"
#include "builder/types/TypeMapper.h"

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

void emitNonPayableCheck(
	awst::SourceLocation const& loc,
	std::vector<std::shared_ptr<awst::Statement>>& out)
{
	// Own the payment rule at the external boundary, using solc's selected
	// interface function. EVM-profile method bodies deliberately have no guard;
	// ARC4 compatibility routes must not rely on a different selector's guard.
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

/// A word-leaf decode helper `name(__off)`: one `__evm_decw(__off)` fetch
/// (evaluated once), `guard(word)` asserted with `message`, `result(word)`
/// returned.
using WordLeafFn = std::function<std::shared_ptr<awst::Expression>(
	std::shared_ptr<awst::Expression> const&)>;

void synthesizeWordLeaf(
	awst::Contract& contract,
	std::string const& cref,
	awst::SourceLocation const& loc,
	std::string const& name,
	awst::WType const* returnType,
	WordLeafFn const& guard,
	char const* message,
	WordLeafFn const& result)
{
	auto sub = awst::ContractMethod(
		cref, name, returnType,
		{{"__off", awst::WType::uint64Type(), loc}}, loc);
	auto fetch = awst::makeSubroutineCall(
		awst::InstanceMethodTarget{"__evm_decw"},
		awst::WType::bytesType(), loc);
	awst::pushCallArg(fetch->args, awst::makeVarExpression(
		"__off", awst::WType::uint64Type(), loc));
	auto value = awst::makeEvalOnce(std::move(fetch), loc);
	sub.body->body.push_back(awst::makeExpressionStatement(
		awst::makeAssert(guard(value), loc, message), loc));
	sub.body->body.push_back(
		awst::makeReturnStatement(result(value), loc));
	contract.methods.push_back(std::move(sub));
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
		auto sub = awst::ContractMethod(
			cref, "__evm_npy", awst::WType::voidType(), {}, loc);
		emitNonPayableCheck(loc, sub.body->body);
		sub.body->body.push_back(awst::makeReturnStatement(nullptr, loc));
		contract.methods.push_back(std::move(sub));
	}
	{
		auto sub = awst::ContractMethod(
			cref, "__evm_decw", awst::WType::bytesType(),
			{{"__off", awst::WType::uint64Type(), loc}}, loc);
		auto off = [&]() {
			return awst::makeVarExpression(
				"__off", awst::WType::uint64Type(), loc);
		};
		sub.body->body.push_back(awst::makeExpressionStatement(
			awst::makeAssert(
				awst::makeNumericCompare(
					awst::makeUInt64BinOp(off(),
						awst::UInt64BinaryOperator::Add, u64(32, loc), loc),
					awst::NumericComparison::Lte,
					awst::makeLen(awst::makeAppArg(1, loc), loc), loc),
				loc, "EVM ABI decode out of bounds"), loc));
		sub.body->body.push_back(awst::makeReturnStatement(
			awst::makeExtract3(
				awst::makeAppArg(1, loc), off(), u64(32, loc), loc), loc));
		contract.methods.push_back(std::move(sub));
	}
	// __evm_deco(__off) — offset/length small word: decw + high-24-zero
	// assert + narrow. __evm_arga(__off) — address leaf: decw + padding
	// assert. Each dynamic arg repeats the former, each address arg the
	// latter; one body apiece (puya strips whichever ends up uncalled).
	synthesizeWordLeaf(contract, cref, loc, "__evm_deco",
		awst::WType::uint64Type(),
		[&](auto const& value) {
			return awst::makeNumericCompare(
				awst::makeAsBiguint(
					awst::makeExtract(value, 0, 24, loc), loc),
				awst::NumericComparison::Eq,
				awst::makeIntegerConstant("0", loc,
					awst::WType::biguintType()), loc);
		},
		"EVM ABI offset exceeds uint64",
		[&](auto const& value) {
			return awst::makeWord32ToUInt64(value, loc);
		});
	synthesizeWordLeaf(contract, cref, loc, "__evm_arga",
		awst::WType::accountType(),
		[&](auto const& value) {
			return awst::makeBytesComparison(
				awst::makeExtract(value, 0, 12, loc),
				awst::EqualityComparison::Eq, awst::makeBzero(12, loc), loc);
		},
		"invalid EVM ABI address padding",
		[&](auto const& value) {
			return awst::makeAsAccount(value, loc);
		});
}

struct EvmRoute
{
	FunctionType const* function = nullptr;
	size_t methodIndex = 0;
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
		if (!codec::canRoundTripEvmAbi(function->parameterTypes())
			|| !codec::canRoundTripEvmAbi(function->returnParameterTypes()))
		{
			if (!quiet)
				Logger::instance().error(
					"Solidity ABI entry route contains a type unsupported by the "
					"canonical recursive codec: " + function->externalSignature(), loc);
			continue;
		}
		// canRoundTripEvmAbi answers the TYPE question, but emitting an external
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
		routes.push_back({function, static_cast<size_t>(method - contract.methods.data()),
			SolcFacts::externalSelector(*function)});
	}
	return routes;
}

/// Group key for a route's return tail: canonical Solidity return signature
/// + the method's wire return WType identity (createType canonicalizes, so
/// pointer equality is type equality).
std::string evmRetTailKey(EvmRoute const& route, awst::Contract const& contract)
{
	std::string key;
	for (auto const* type: route.function->returnParameterTypes())
		key += type->canonicalName() + ",";
	key += "#";
	key += std::to_string(
		reinterpret_cast<uintptr_t>(contract.methods.at(route.methodIndex).returnType));
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
	std::vector<EvmRoute> const& routes,
	awst::SourceLocation const& loc)
{
	struct TailSpec
	{
		std::vector<Type const*> returnTypes;
		awst::WType const* retW = nullptr;
		int uses = 0;
	};
	std::map<std::string, TailSpec> groups;
	for (auto const& route: routes)
	{
		auto& spec = groups[evmRetTailKey(route, contract)];
		if (spec.uses == 0)
		{
			spec.returnTypes = route.function->returnParameterTypes();
			spec.retW = contract.methods.at(route.methodIndex).returnType;
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
		if (spec.uses < 2 || spec.returnTypes.empty())
			continue;
		if (spec.returnTypes.size() > 1
			&& !dynamic_cast<awst::WTuple const*>(spec.retW))
			continue;   // unexpected wire shape — keep those arms inline

		std::string name = "__evm_ret" + std::to_string(index++);
		auto sub = awst::ContractMethod(
			cref, name, awst::WType::voidType(), {}, loc);
		auto body = sub.body;
		std::vector<std::shared_ptr<awst::Expression>> returnValues;
		if (!spec.returnTypes.empty())
		{
			sub.args.emplace_back("__v", spec.retW, loc);
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
	awst::Contract const& contract,
	EvmRoute const& route,
	std::map<std::string, std::string> const& retTails,
	awst::SourceLocation const& loc,
	std::shared_ptr<awst::Expression> selfPayload = nullptr)
{
	auto const& paramTypes = route.function->parameterTypes();
	auto const& returnTypes = route.function->returnParameterTypes();
	auto body = awst::makeBlock(loc);
	if (!selfPayload && !route.function->isPayable())
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
		auto decoded = selfPayload
			? abi::decodeEvmAbi(typeMapper, awst::makeExtract(selfPayload, 4, 0, loc),
				paramTypes, decodedType, loc, body->body)
			: abi::decodeEvmCalldata(typeMapper, paramTypes, decodedType, loc, body->body);
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
		awst::InstanceMethodTarget{contract.methods.at(route.methodIndex).memberName},
		contract.methods.at(route.methodIndex).returnType, loc);
	for (size_t i = 0; i < values.size(); ++i)
	{
		auto value = std::move(values[i]);
		auto const* expected = i < contract.methods.at(route.methodIndex).args.size()
			? contract.methods.at(route.methodIndex).args[i].wtype : value->wtype;
		if (value->wtype != expected)
			value = codec::valueToArc4(
				typeMapper, paramTypes[i], std::move(value), expected, loc);
		awst::pushCallArg(call->args, std::move(value));
	}

	if (selfPayload)
	{
		auto bytes = ApplicationCall::setTypedReturnData(typeMapper, call,
			returnTypes, true, loc, body->body);
		auto result = awst::makeTupleExpression(typeMapper.createType<awst::WTuple>(
			std::vector<awst::WType const*>{awst::WType::boolType(), awst::WType::bytesType()}), loc);
		result->items = {awst::makeTrue(loc), std::move(bytes)};
		body->body.push_back(awst::makeReturnStatement(std::move(result), loc));
		return body;
	}

	if (returnTypes.empty())
	{
		// Match ARC4's void route: absence of a return record means empty
		// data. An assembly return may already have emitted an explicit
		// payload despite the void signature; never overwrite that record.
		body->body.push_back(awst::makeExpressionStatement(call, loc));
		body->body.push_back(awst::makeReturnStatement(awst::makeTrue(loc), loc));
		return body;
	}

	// Shared tail: `callsub __evm_ret<i>` replaces the inline encode+log
	// epilogue for return shapes used by 2+ arms.
	if (auto tailIt = retTails.find(evmRetTailKey(route, contract));
		tailIt != retTails.end())
	{
		auto tailCall = awst::makeSubroutineCall(
			awst::InstanceMethodTarget{tailIt->second},
			awst::WType::voidType(), loc);
		awst::pushCallArg(tailCall->args, call);
		body->body.push_back(
			awst::makeExpressionStatement(std::move(tailCall), loc));
		body->body.push_back(
			awst::makeReturnStatement(awst::makeTrue(loc), loc));
		return body;
	}

	std::vector<std::shared_ptr<awst::Expression>> returnValues;
	if (returnTypes.size() == 1)
		returnValues.push_back(call);
	else
	{
		auto once = awst::makeEvalOnce(call, loc);
		auto const* tuple = dynamic_cast<awst::WTuple const*>(
			contract.methods.at(route.methodIndex).returnType);
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
	awst::Contract const& contract,
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
			buildEvmArmBody(typeMapper, contract, route, retTails, loc));
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

void ContractBuilder::emitSelfCallDispatch(
	ContractDefinition const& definition, awst::Contract& contract)
{
	if (!m_typeMapper.artifacts().contract().needsSelfCallDispatch) return;
	auto const loc = contract.approvalProgram.sourceLocation;
	auto const* resultType = m_typeMapper.createType<awst::WTuple>(
		std::vector<awst::WType const*>{awst::WType::boolType(), awst::WType::bytesType()});
	auto method = awst::ContractMethod(contract.id, "__puyasol_self_call", resultType,
		{{"__payload", awst::WType::bytesType(), loc}}, loc);
	auto payload = awst::makeVarExpression("__payload", awst::WType::bytesType(), loc);
	auto finish = [&](std::shared_ptr<awst::Block> block, bool success,
		std::shared_ptr<awst::Expression> bytes) {
		auto result = awst::makeTupleExpression(resultType, loc);
		result->items = {awst::makeBoolConstant(success, loc),
			ApplicationCall::finishSelfCall(m_typeMapper, std::move(bytes), loc, block->body)};
		block->body.push_back(awst::makeReturnStatement(std::move(result), loc));
	};
	auto fallback = [&](FunctionDefinition const* function) {
		auto block = awst::makeBlock(loc);
		auto bytes = std::shared_ptr<awst::Expression>(awst::makeBytesConstant({}, loc));
		if (function)
		{
			auto const name = function->isReceive() ? "__receive" : "__fallback";
			auto const* target = findMethod(contract, name);
			if (!target) throw std::logic_error("Missing self-call fallback method");
			auto call = awst::makeSubroutineCall(awst::InstanceMethodTarget{name}, target->returnType, loc);
			if (!function->parameters().empty()) awst::pushCallArg(call->args, payload);
			if (function->returnParameters().empty())
				block->body.push_back(awst::makeExpressionStatement(std::move(call), loc));
			else bytes = std::move(call);
		}
		finish(block, function != nullptr, std::move(bytes));
		return block;
	};
	// solc's dispatcher selects receive only for EMPTY calldata, then fallback.
	auto const* receive = definition.receiveFunction();
	auto const* fallbackFunction = definition.fallbackFunction();
	method.body->body.push_back(awst::makeIfElse(awst::makeNumericCompare(
		awst::makeLen(payload, loc), awst::NumericComparison::Eq, awst::makeZero(loc), loc),
		fallback(receive ? receive : fallbackFunction), nullptr, loc));

	auto routes = collectEvmRoutes(definition, contract, m_overloadedNames, loc,
		m_typeMapper.profile().contractAbi != ContractAbi::Evm);
	auto dispatch = std::make_shared<awst::Switch>();
	dispatch->sourceLocation = loc;
	dispatch->value = awst::makeExtract(payload, 0, 4, loc);
	if (m_typeMapper.profile().contractAbi == ContractAbi::Arc4 && !m_typeMapper.profile().evmSelectors)
		dispatch->value = SelectorSemantics::translateRuntimeSelector(
			dispatch->value, SelectorSemantics::routes(*m_exprBuilder), loc);
	for (auto const& route: routes)
		dispatch->cases.emplace_back(awst::makeBytesConstant(route.selector, loc),
			buildEvmArmBody(m_typeMapper, contract, route, {}, loc, payload));
	auto guarded = awst::makeBlock(loc);
	guarded->body.push_back(std::move(dispatch));
	method.body->body.push_back(awst::makeIfElse(awst::makeNumericCompare(
		awst::makeLen(payload, loc), awst::NumericComparison::Gte, u64(4, loc), loc),
		std::move(guarded), nullptr, loc));
	auto fallbackBody = fallback(fallbackFunction);
	for (auto& statement: fallbackBody->body)
		method.body->body.push_back(std::move(statement));
	contract.methods.push_back(std::move(method));
}

void ContractBuilder::emitEvmEntryDispatch(
	ContractDefinition const& contractDefinition,
	awst::Contract& contract)
{
	auto& approval = contract.approvalProgram;
	if (!approval.body)
		return;
	auto const& loc = approval.sourceLocation;

	// Routes retain indices; helper emission may freely grow methods.
	synthesizeEvmEntryHelpers(contract, loc);
	auto routes = collectEvmRoutes(
		contractDefinition, contract, m_overloadedNames, loc, /*quiet=*/false);
	auto retTails = synthesizeEvmReturnTails(m_typeMapper, contract, routes, loc);
	// The methods remain ordinary callable subroutines, but are no longer
	// advertised to or dispatched by puya's ARC4 router.
	for (auto const& route: routes)
		contract.methods.at(route.methodIndex).arc4MethodConfig.reset();

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
		// Guard on NumAppArgs>0: every residual route is a selector-ful
		// lifecycle method, but puya's router (no bare routes) reads
		// ApplicationArgs[0] UNGUARDED — a zero-arg call (EVM empty calldata,
		// t.call("")) faulted there before the receive/fallback arms below
		// could dispatch it.
		auto hasArgs = awst::makeNumericCompare(
			awst::makeTxn("NumAppArgs", awst::WType::uint64Type(), loc),
			awst::NumericComparison::Gt, u64(0, loc), loc);
		auto routerBlock = awst::makeBlock(loc);
		routerBlock->body.push_back(awst::makeAssignmentStatement(
			did(), awst::makeARC4Router(awst::WType::boolType(), loc), loc));
		auto accepted = awst::makeBlock(loc);
		accepted->body.push_back(awst::makeReturnStatement(awst::makeTrue(loc), loc));
		routerBlock->body.push_back(awst::makeIfElse(
			did(), std::move(accepted), nullptr, loc));
		approval.body->body.push_back(awst::makeIfElse(
			std::move(hasArgs), std::move(routerBlock), nullptr, loc));
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
		emitFallbackCall(*emptyDefinition, emptyTarget->memberName, awst::makeBytesConstant({}, loc), loc, body->body);
		body->body.push_back(awst::makeReturnStatement(awst::makeTrue(loc), loc));
		approval.body->body.push_back(awst::makeIfElse(
			std::move(condition), std::move(body), nullptr, loc));
	}

	emitEvmArmSwitch(m_typeMapper, contract, routes, retTails, approval.body->body, loc);

	// Unmatched non-empty calldata selects fallback(). Reconstruct exactly the
	// Solidity byte stream from the AVM carrier split: selector ++ ABI body.
	if (fallbackMethod)
	{
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
		emitFallbackCall(*fallbackDefinition, fallbackMethod->memberName,
			reconstructCalldata(CalldataTransport::SplitEvm, loc), loc, fallbackBody->body);
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
	synthesizeEvmEntryHelpers(contract, loc);
	auto routes = collectEvmRoutes(
		contractDefinition, contract, m_overloadedNames, loc, /*quiet=*/true,
		&m_typeMapper.analysis());
	auto retTails = synthesizeEvmReturnTails(m_typeMapper, contract, routes, loc);
	emitEvmArmSwitch(m_typeMapper, contract, routes, retTails, approval.body->body, loc);
}

} // namespace puyasol::builder
