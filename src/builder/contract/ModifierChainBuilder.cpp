/// @file ModifierChainBuilder.cpp
/// Builds modifier call chains for contract methods and constructors. Each
/// Solidity placeholder becomes a fresh call to the next link in the chain,
/// preserving multiple-placeholder semantics without copying AWST nodes.

#include "builder/contract/ContractBuilder.h"
#include "builder/sol-ast/EvmSlotLowering.h"
#include "builder/storage/EvmLayoutMode.h"
#include "awst/StatementWalk.h"
#include "awst/NameGen.h"
#include "builder/contract/StateVarWalker.h"
#include "builder/sol-ast/stmts/SolBlock.h"
#include "builder/sol-types/TypeCoercion.h"
#include "awst/Termination.hpp"

#include <libsolidity/ast/ASTVisitor.h>

namespace puyasol::builder
{

void ContractBuilder::buildModifierChain(
	solidity::frontend::FunctionDefinition const& _func,
	awst::ContractMethod& _method,
	std::string const& _contractName,
	std::vector<ParamDecode> const& _paramDecodes,
	std::vector<size_t> const& _writeBackParams
)
{
	int chainId = awst::NameGen::next("ModifierChainBuilder.modChainCounter");

	auto const& modifiers = _func.modifiers();
	if (modifiers.empty())
		return;

	std::string baseName = _method.memberName.empty()
		? _func.name() : _method.memberName;
	std::string cref = m_sourceFile + "." + _contractName;

	// Return-parameter THREADING (mirrors solc IR). A modifier arg or the body may
	// READ/WRITE the named return vars — `mod2(r)`, `m1(x = 2)`, or `r += 1` accumulating
	// across a repeated/looped `_;`. So thread the return params as LEADING in-args through
	// every chain sub and capture them back out at each `_`, letting mutations propagate.
	// These stay native until the outer wrapper return is encoded. Found by the
	// dispatch fuzzer + the chain-as-default experiment.
	struct RetInfo
	{
		std::string name;
		awst::WType const* type;
		bool isWriteBack;
	};
	std::vector<RetInfo> retInfos;
	// Thread the SAME types _method.returnType declares — that is what the body sub
	// returns after native normalization (which promotes signed sub-64 and wide-uint
	// return elements to biguint at the ABI boundary). Re-mapping from the Solidity
	// type instead would give `int64` → uint64, so capturing the body's biguint into
	// a uint64 threading slot fails puya with "Tuple type mismatch". For a tuple the
	// element types come from the WTuple; for a scalar, the whole returnType.
	auto const* retTuple = (_method.returnType
		&& _method.returnType->kind() == awst::WTypeKind::WTuple)
		? static_cast<awst::WTuple const*>(_method.returnType) : nullptr;
	for (size_t ri = 0; ri < _func.returnParameters().size(); ++ri)
	{
		auto const& rp = _func.returnParameters()[ri];
		std::string nm = rp->name().empty()
			? "__ret_" + std::to_string(chainId) + "_" + std::to_string(ri)
			: rp->name();
		awst::WType const* rt =
			(retTuple && ri < retTuple->types().size()) ? retTuple->types()[ri]
			: (!retTuple ? _method.returnType : m_typeMapper.map(rp->type()));
		retInfos.push_back({nm, rt, false});
	}
	for (size_t paramIndex: _writeBackParams)
		if (paramIndex < _method.args.size())
		{
			auto const& arg = _method.args[paramIndex];
			retInfos.push_back({arg.name, arg.wtype, true});
		}
	bool const hasRet = (_method.returnType != awst::WType::voidType());
	// Leading return-param args, prepended to every chain sub's signature.
	std::vector<awst::SubroutineArgument> retArgs;
	for (auto const& r: retInfos)
		if (!r.isWriteBack)
			retArgs.push_back(awst::SubroutineArgument{
				r.name, _method.sourceLocation, r.type});
	// Prepend retArgs to a sub's args (returns a fresh combined vector).
	auto withRetArgs = [&](std::vector<awst::SubroutineArgument> const& _fnArgs) {
		std::vector<awst::SubroutineArgument> out = retArgs;
		out.insert(out.end(), _fnArgs.begin(), _fnArgs.end());
		return out;
	};
	// Push the current return-param vars, then the function params, as call args.
	auto pushThreadedArgs = [&](std::shared_ptr<awst::SubroutineCallExpression> const& _call,
		awst::SourceLocation const& _loc) {
		for (auto const& r: retInfos)
			if (!r.isWriteBack)
				awst::pushCallArg(_call->args, r.name,
					awst::makeVarExpression(r.name, r.type, _loc));
		for (auto const& arg: _method.args)
			awst::pushCallArg(_call->args, arg.name,
				awst::makeVarExpression(arg.name, arg.wtype, _loc));
	};
	// Capture a chain call's return into the return-param var(s): `r = call` (one) or
	// `(r1,…,rN) = call` (many). Appends the assignment to `_dst`.
	auto captureReturn = [&](std::shared_ptr<awst::Block> const& _dst,
		std::shared_ptr<awst::Expression> _call, awst::SourceLocation const& _loc) {
		if (!hasRet) { _dst->body.push_back(awst::makeExpressionStatement(std::move(_call), _loc)); return; }
		if (retInfos.size() == 1)
		{
			auto tgt = awst::makeVarExpression(retInfos[0].name, retInfos[0].type, _loc);
			_dst->body.push_back(awst::makeAssignmentStatement(std::move(tgt), std::move(_call), _loc));
		}
		else
		{
			auto tup = awst::makeTupleExpression(_method.returnType, _loc);
			for (auto const& r: retInfos)
				tup->items.push_back(awst::makeVarExpression(r.name, r.type, _loc));
			_dst->body.push_back(awst::makeAssignmentStatement(std::move(tup), std::move(_call), _loc));
		}
	};
	// Return the threaded return-param(s): `return r` / `return (r1,…,rN)` / bare `return`.
	auto makeThreadedReturn = [&](awst::SourceLocation const& _loc) -> std::shared_ptr<awst::Statement> {
		if (!hasRet) return awst::makeReturnStatement(nullptr, _loc);
		if (retInfos.size() == 1)
			return awst::makeReturnStatement(
				awst::makeVarExpression(retInfos[0].name, retInfos[0].type, _loc), _loc);
		auto tup = awst::makeTupleExpression(_method.returnType, _loc);
		for (auto const& r: retInfos)
			tup->items.push_back(awst::makeVarExpression(r.name, r.type, _loc));
		return awst::makeReturnStatement(std::move(tup), _loc);
	};

	// Every emitted sub receives the still-ARC4-encoded `__arc4_*` params.
	// Materialize independent decode assignments for each body from the compact
	// recipes retained by FunctionBuilder.
	auto prependDecodes = [&](std::shared_ptr<awst::Block> const& _blk) {
		if (_paramDecodes.empty() || !_blk) return;
		auto decodes = makeParamDecodeStatements(_paramDecodes);
		_blk->body.insert(_blk->body.begin(),
			std::make_move_iterator(decodes.begin()),
			std::make_move_iterator(decodes.end()));
	};

	// Innermost subroutine = original function body.
	std::string bodySubName = baseName + "__body_" + std::to_string(chainId);
	{
		awst::ContractMethod bodySub;
		bodySub.sourceLocation = _method.sourceLocation;
		bodySub.cref = cref;
		bodySub.memberName = bodySubName;
		bodySub.returnType = _method.returnType;
		bodySub.args = withRetArgs(_method.args); // return params (in/out) + function params
		bodySub.body = _method.body; // move the original function body here
		prependDecodes(bodySub.body);
		// A named-return body (`{ r += 1; }`) sets the return-param vars but may fall off
		// the end without a `return` — append one that threads them back out.
		if (bodySub.body && !awst::blockAlwaysTerminates(*bodySub.body))
			bodySub.body->body.push_back(makeThreadedReturn(bodySub.sourceLocation));
		bodySub.arc4MethodConfig = std::nullopt; // internal, not ABI-routable
		bodySub.pure = _method.pure;
		m_modifierSubroutines.push_back(std::move(bodySub));
	}

	// Modifier subroutines innermost→outermost, each calling the next.
	std::string nextSubName = bodySubName;

	for (int i = static_cast<int>(modifiers.size()) - 1; i >= 0; --i)
	{
		auto const& modInvocation = modifiers[i];
		auto const* modDef = dynamic_cast<solidity::frontend::ModifierDefinition const*>(
			modInvocation->name().annotation().referencedDeclaration
		);
		if (!modDef)
			continue; // constructor base call — handled elsewhere

		// Resolve virtual override unless this is an explicit A.m base call.
		bool isExplicitBaseModifier = false;
		{
			auto const& path = modInvocation->name().path();
			if (path.size() > 1)
				isExplicitBaseModifier = true;
		}

		auto const* declaringContract = _func.annotation().contract;
		bool const supportsVirtualModifiers = declaringContract
			&& !declaringContract->isLibrary() && !_func.isFree();
		if (m_currentContract && supportsVirtualModifiers
			&& !isExplicitBaseModifier)
		{
			std::string modName = modDef->name();
			solidity::frontend::ModifierDefinition const* resolved = nullptr;
			forEachFunctionModifier(*m_currentContract, [&](auto const* mod)
			{
				if (resolved) return;
				if (mod->name() == modName) resolved = mod;
			});
			if (resolved) modDef = resolved;
		}

		std::string modSubName = baseName + "__mod" + std::to_string(i) + "_" + std::to_string(chainId);

		awst::ContractMethod modSub;
		modSub.sourceLocation = makeLoc(modDef->location());
		modSub.cref = cref;
		modSub.memberName = modSubName;
		modSub.returnType = _method.returnType;
		modSub.args = withRetArgs(_method.args); // return params (in/out) + function params
		modSub.arc4MethodConfig = std::nullopt; // internal
		modSub.pure = _method.pure;

		auto modBody = awst::makeBlock(modSub.sourceLocation);

		// Decode params first so a modifier arg expr (`mArg(a % 5)`) sees native values.
		prependDecodes(modBody);

		auto const* args = modInvocation->arguments();
		auto const& params = modDef->parameters();
		std::vector<int64_t> remappedDeclIds;

		if (args && !args->empty())
		{
			auto modLoc = makeLoc(modInvocation->location());
			for (size_t pi = 0; pi < args->size() && pi < params.size(); ++pi)
			{
				auto const& param = params[pi];
				std::string uniqueName = "__mod_" + param->name() + "_" + std::to_string(awst::NameGen::next("ModifierChainBuilder.modArgCounter"));
				auto* paramType = m_typeMapper.map(param->type());

				// --evm-storage-layout: storage-ref modifier params bind as
				// runtime SLOT-HANDLE vars under the PLAIN param name (what
				// isSlotHandleLocal reads resolve). Building the arg would
				// materialise the aggregate; the alias below is the retired
				// named-cell model. Identifier args resolve purely.
				if (m_typeMapper.profile().evmStorageLayout
					&& param->referenceLocation()
						== solidity::frontend::VariableDeclaration::Location::Storage
					&& dynamic_cast<solidity::frontend::Identifier const*>(
						(*args)[pi].get()))
				{
					sol_ast::EvmSlotLowering low(
						*m_exprBuilder, *m_exprBuilder->currentScope, modLoc);
					if (auto addr = low.resolve(*(*args)[pi]))
					{
						modBody->body.push_back(awst::makeAssignmentStatement(
							awst::makeVarExpression(param->name(),
								awst::WType::biguintType(), modLoc),
							addr->slot, modLoc));
						remappedDeclIds.push_back(param->id());
					}
					continue;
				}

				auto argExpr = m_exprBuilder->buildExpr(*(*args)[pi]);
				if (!argExpr) continue;

				// Solidity memory parameters alias an identifier argument. Remap the
				// modifier declaration directly to that variable so writes before or
				// after `_` remain visible to the wrapped body and its caller.
				if (param->referenceLocation()
						== solidity::frontend::VariableDeclaration::Location::Memory)
				{
					m_exprBuilder->appendEffectsTo(modBody->body);
					if (auto const* variable =
						dynamic_cast<awst::VarExpression const*>(argExpr.get());
						variable && variable->wtype == paramType)
					{
						m_tr->setParamRemap(param->id(), sol_ast::ParamRemap{
							variable->name, paramType});
						remappedDeclIds.push_back(param->id());
						continue;
					}
				}

				// Storage-POINTER modifier param (`modifier m(uint256[] storage a, ...)`
				// / `mapping(...) storage`): alias it to the ARGUMENT's storage location
				// so the modifier body's writes (`a[i] += 1`) mutate the real state var,
				// not a local copy. The aliased target is a contract-global state var or
				// mapping, resolvable from this modifier
				// subroutine. Without it the write was bound to a `__mod_a` LOCAL and
				// silently DROPPED. Found by coverage-guided fuzzing (this storage-ref
				// alias path was 0%-covered in the whole suite).
				if (param->referenceLocation()
					== solidity::frontend::VariableDeclaration::Location::Storage)
				{
					m_exprBuilder->appendEffectsTo(modBody->body);
					sol_ast::StorageAlias alias =
						sol_ast::StorageAlias::classify(std::move(argExpr));
					m_tr->setStorageAlias(param->id(), std::move(alias));
					remappedDeclIds.push_back(param->id());
					continue;
				}

				argExpr = TypeCoercion::implicitNumericCast(std::move(argExpr), paramType, modLoc);
				// `onlyRole(MINTER_ROLE)` binds keccak256(...) — wtype unsized
				// `bytes` — to a `bytes32` param. Bytes are right, label is not,
				// and puya rejects the mismatch outright.
				argExpr = TypeCoercion::relabelUnsizedBytes(
					std::move(argExpr), paramType, modLoc);

				// A modifier ARGUMENT can be a side-effecting expression — a ternary
				// with a checked/negate branch (`mod(a > 0 ? a : -a)`), a checked op —
				// whose SolConditional emits a branch-gating if/else assigning the
				// result to a temp, returning a bare temp-READ. build()/the cast leave
				// those PRE-statements in the context; drain them into the modifier body
				// BEFORE the binding, else `__mod_arg = …(temp)` runs before the if/else
				// assigns the temp (the ternary collapsed to its false branch, reverting
				// every call). Found by coverage-guided fuzzing (modifier lowering cold).
				m_exprBuilder->appendEffectsTo(modBody->body);

				auto target = awst::makeVarExpression(uniqueName, paramType, modLoc);

				auto assignment = awst::makeAssignmentStatement(target, std::move(argExpr), modLoc);
				modBody->body.push_back(std::move(assignment));

				m_tr->setParamRemap(param->id(), sol_ast::ParamRemap{uniqueName, paramType});
				remappedDeclIds.push_back(param->id());
			}
		}

		// At `_`: thread the return-param(s) in, call nextSubName, capture them back out
		// so a repeated/looped `_;` accumulates and a modifier arg's writes propagate.
		auto makePlaceholder = [&, nextSubName,
			loc = modSub.sourceLocation]() {
			auto placeholderBlock = awst::makeBlock(loc);
			auto call = awst::makeSubroutineCall(
				awst::InstanceMethodTarget{nextSubName}, _method.returnType, loc);
			pushThreadedArgs(call, loc);
			captureReturn(placeholderBlock, std::move(call), loc);
			return placeholderBlock;
		};

		setPlaceholderFactory(std::move(makePlaceholder));
		auto translatedBody = buildBlock(modDef->body());
		setPlaceholderFactory({});

		if (translatedBody)
		{
			// A bare `return;` in a modifier body exits it with the current return-params.
			if (hasRet)
			{
				std::function<void(std::vector<std::shared_ptr<awst::Statement>>&)> fixReturns;
				fixReturns = [&](std::vector<std::shared_ptr<awst::Statement>>& stmts)
				{
					for (auto& stmt: stmts)
					{
						if (auto* ret = dynamic_cast<awst::ReturnStatement*>(stmt.get()))
						{
							if (!ret->value)
								stmt = makeThreadedReturn(ret->sourceLocation);
						}
						else
							awst::forEachChildBlock(*stmt, [&](awst::Block& b, bool) {
								fixReturns(b.body);
							});
					}
				};
				fixReturns(translatedBody->body);
			}

			for (auto& stmt: translatedBody->body)
				modBody->body.push_back(std::move(stmt));
		}

		// Modifier falls off the end → return the threaded return-param values.
		modBody->body.push_back(makeThreadedReturn(modSub.sourceLocation));

		for (auto declId: remappedDeclIds)
			m_tr->eraseParamRemap(declId);

		modSub.body = modBody;
		m_modifierSubroutines.push_back(std::move(modSub));
		nextSubName = modSubName;
	}

	// Rewrite _method.body to zero-init the return-param vars, call the outermost modifier
	// (threading the return params in), capture them back, and return them.
	auto entryBody = awst::makeBlock(_method.sourceLocation);

	for (auto const& r: retInfos) // zero-init named return vars before the call
	{
		if (r.isWriteBack)
			continue;
		auto target = awst::makeVarExpression(r.name, r.type, _method.sourceLocation);
		auto zeroVal = StorageMapper::makeDefaultValue(r.type, _method.sourceLocation);
		entryBody->body.push_back(awst::makeAssignmentStatement(
			std::move(target), std::move(zeroVal), _method.sourceLocation));
	}

	auto call = awst::makeSubroutineCall(
		awst::InstanceMethodTarget{nextSubName}, _method.returnType, _method.sourceLocation);
	pushThreadedArgs(call, _method.sourceLocation);

	if (hasRet)
	{
		captureReturn(entryBody, std::move(call), _method.sourceLocation);
		entryBody->body.push_back(makeThreadedReturn(_method.sourceLocation));
	}
	else
		entryBody->body.push_back(
			awst::makeExpressionStatement(std::move(call), _method.sourceLocation));

	_method.body = entryBody;
}

void ContractBuilder::buildConstructorModifierChain(
	solidity::frontend::FunctionDefinition const& _func,
	std::shared_ptr<awst::Block>& _body,
	std::string const& _contractName)
{
	bool hasModifier = false;
	for (auto const& invocation: _func.modifiers())
		if (dynamic_cast<solidity::frontend::ModifierDefinition const*>(
				invocation->name().annotation().referencedDeclaration))
		{
			hasModifier = true;
			break;
		}
	if (!hasModifier)
		return;

	awst::ContractMethod constructor;
	constructor.sourceLocation = makeLoc(_func.location());
	constructor.cref = m_sourceFile + "." + _contractName;
	constructor.memberName = "__ctor_" + std::to_string(_func.id());
	constructor.returnType = awst::WType::voidType();
	constructor.body = std::move(_body);
	for (auto const& parameter: _func.parameters())
	{
		// An unnamed constructor parameter is accepted at the ABI boundary, but
		// neither its body nor a modifier invocation can reference it. Do not
		// invent and thread a dead internal local for it.
		if (parameter->name().empty())
			continue;
		constructor.args.push_back({
			parameter->name(),
			makeLoc(parameter->location()),
			m_typeMapper.map(parameter->type())});
	}

	auto const* savedReturnType = m_functionCtx->returnType;
	bool const savedFrameIsProgram = m_functionCtx->frameIsProgram;
	m_functionCtx->returnType = awst::WType::voidType();
	m_functionCtx->frameIsProgram = true;
	buildModifierChain(_func, constructor, _contractName);
	m_functionCtx->frameIsProgram = savedFrameIsProgram;
	m_functionCtx->returnType = savedReturnType;
	_body = std::move(constructor.body);
}

} // namespace puyasol::builder
