/// @file ModifierChainBuilder.cpp
/// Builds modifier call chains for contract methods and constructors. Each
/// Solidity placeholder becomes a fresh call to the next link in the chain,
/// preserving multiple-placeholder semantics without copying AWST nodes.

#include "builder/contract/ContractBuilder.h"
#include "builder/SolcFacts.h"
#include "builder/sol-ast/EvmSlotLowering.h"
#include "builder/sol-ast/exprs/SolIndexAccess.h"
#include "builder/storage/EvmLayoutMode.h"
#include "awst/StatementWalk.h"
#include "awst/NameGen.h"
#include "builder/sol-ast/stmts/SolBlock.h"
#include "builder/sol-types/TypeCoercion.h"
#include "awst/Termination.hpp"

#include <libsolidity/ast/AST.h>
#include <libsolutil/Common.h>

#include <algorithm>
#include <iterator>

namespace puyasol::builder
{

namespace
{

using solidity::frontend::Conditional;
using solidity::frontend::Expression;
using solidity::frontend::FunctionCall;
using solidity::frontend::FunctionCallKind;
using solidity::frontend::FunctionDefinition;
using solidity::frontend::Identifier;
using solidity::frontend::IndexAccess;
using solidity::frontend::IndexRangeAccess;
using solidity::frontend::MemberAccess;
using solidity::frontend::TupleExpression;
using solidity::frontend::VariableDeclaration;

bool isMemoryReference(VariableDeclaration const& _declaration)
{
	return _declaration.referenceLocation() == VariableDeclaration::Location::Memory
		&& _declaration.type() && !_declaration.type()->isValueType();
}

/// Follow only expressions for which solc preserves memory-reference identity.
/// Calls and constructors intentionally stop the walk: their result is a fresh
/// value for this lowering. Explicit reference conversions and conditional
/// reference selection preserve the selected pointer.
void collectMemoryRoots(
	Expression const& _expression,
	std::map<int64_t, VariableDeclaration const*> const& _parameters,
	std::set<int64_t>& _out)
{
	if (auto const* identifier = dynamic_cast<Identifier const*>(&_expression))
	{
		auto const* declaration = dynamic_cast<VariableDeclaration const*>(
			identifier->annotation().referencedDeclaration);
		if (declaration && _parameters.count(declaration->id()))
			_out.insert(declaration->id());
		return;
	}
	if (auto const* member = dynamic_cast<MemberAccess const*>(&_expression))
		return collectMemoryRoots(member->expression(), _parameters, _out);
	if (auto const* index = dynamic_cast<IndexAccess const*>(&_expression))
		return collectMemoryRoots(index->baseExpression(), _parameters, _out);
	if (auto const* range = dynamic_cast<IndexRangeAccess const*>(&_expression))
		return collectMemoryRoots(range->baseExpression(), _parameters, _out);
	if (auto const* conditional = dynamic_cast<Conditional const*>(&_expression))
	{
		collectMemoryRoots(conditional->trueExpression(), _parameters, _out);
		collectMemoryRoots(conditional->falseExpression(), _parameters, _out);
		return;
	}
	if (auto const* tuple = dynamic_cast<TupleExpression const*>(&_expression))
	{
		for (auto const& component: tuple->components())
			if (component)
				collectMemoryRoots(*component, _parameters, _out);
		return;
	}
	if (auto const* call = dynamic_cast<FunctionCall const*>(&_expression);
		call && call->annotation().kind.set()
		&& *call->annotation().kind == FunctionCallKind::TypeConversion
		&& call->arguments().size() == 1)
		collectMemoryRoots(*call->arguments()[0], _parameters, _out);
}

std::string memoryRootName(
	FunctionDefinition const& _function, VariableDeclaration const& _parameter)
{
	return "__modroot_" + std::to_string(_function.id()) + "_"
		+ std::to_string(_parameter.id());
}

bool canResolveMemoryPointer(
	sol_ast::Context const& _scope, Expression const& _expression)
{
	if (auto const* identifier = dynamic_cast<Identifier const*>(&_expression))
	{
		auto const* declaration = dynamic_cast<VariableDeclaration const*>(
			identifier->annotation().referencedDeclaration);
		return declaration && !_scope.bindings.blobAggregates.get(declaration->id()).empty();
	}
	if (auto const* member = dynamic_cast<MemberAccess const*>(&_expression))
		return canResolveMemoryPointer(_scope, member->expression());
	if (auto const* index = dynamic_cast<IndexAccess const*>(&_expression))
		return canResolveMemoryPointer(_scope, index->baseExpression());
	if (auto const* conditional = dynamic_cast<Conditional const*>(&_expression))
		return canResolveMemoryPointer(_scope, conditional->trueExpression())
			&& canResolveMemoryPointer(_scope, conditional->falseExpression());
	if (auto const* tuple = dynamic_cast<TupleExpression const*>(&_expression))
		return tuple->components().size() == 1 && tuple->components()[0]
			&& canResolveMemoryPointer(_scope, *tuple->components()[0]);
	if (auto const* call = dynamic_cast<FunctionCall const*>(&_expression);
		call && call->annotation().kind.set()
		&& *call->annotation().kind == FunctionCallKind::TypeConversion
		&& call->arguments().size() == 1)
		return canResolveMemoryPointer(_scope, *call->arguments()[0]);
	return false;
}

std::shared_ptr<awst::Expression> resolveMemoryPointer(
	eb::ContractContext& _ctx, sol_ast::Context& _scope,
	Expression const& _expression, awst::SourceLocation const& _loc)
{
	if (!canResolveMemoryPointer(_scope, _expression))
		return nullptr;
	if (auto const* conditional = dynamic_cast<Conditional const*>(&_expression))
	{
		auto condition = _ctx.pinIfWriteBacks(
			_ctx.lower(conditional->condition(), false), _loc);
		auto trueValue = _ctx.lowerOperand([&] {
			return resolveMemoryPointer(
				_ctx, _scope, conditional->trueExpression(), _loc);
		});
		auto falseValue = _ctx.lowerOperand([&] {
			return resolveMemoryPointer(
				_ctx, _scope, conditional->falseExpression(), _loc);
		});
		std::string const name = "__modptr_select_" + std::to_string(
			awst::NameGen::next("ModifierChainBuilder.pointerSelect"));
		auto target = [&] {
			return awst::makeVarExpression(
				name, awst::WType::uint64Type(), _loc);
		};
		auto trueBlock = eb::ContractContext::makeScopedResultBlock(
			std::move(trueValue.effects.pre), target(),
			std::move(trueValue.value), _loc,
			std::move(trueValue.effects.post));
		auto falseBlock = eb::ContractContext::makeScopedResultBlock(
			std::move(falseValue.effects.pre), target(),
			std::move(falseValue.value), _loc,
			std::move(falseValue.effects.post));
		_ctx.preEffects().push_back(awst::makeIfElse(
			std::move(condition), std::move(trueBlock),
			std::move(falseBlock), _loc));
		return target();
	}
	if (auto const* tuple = dynamic_cast<TupleExpression const*>(&_expression))
		return resolveMemoryPointer(
			_ctx, _scope, *tuple->components()[0], _loc);
	if (auto const* call = dynamic_cast<FunctionCall const*>(&_expression);
		call && call->annotation().kind.set()
		&& *call->annotation().kind == FunctionCallKind::TypeConversion)
		return resolveMemoryPointer(
			_ctx, _scope, *call->arguments()[0], _loc);
	return sol_ast::SolIndexAccess::resolveBlobOffset(
		_ctx, _scope, _expression, _loc);
}

struct MemoryBridge
{
	size_t parameterIndex;
	VariableDeclaration const* declaration;
	std::string name;
	std::string valueName;
	awst::WType const* nativeType;
};

template <class MakePrefix>
void insertBeforeReturns(
	std::vector<std::shared_ptr<awst::Statement>>& _statements,
	MakePrefix const& _makePrefix)
{
	for (size_t i = 0; i < _statements.size(); ++i)
	{
		if (auto const* ret = dynamic_cast<awst::ReturnStatement const*>(
				_statements[i].get()))
		{
			auto prefix = _makePrefix(ret->sourceLocation);
			auto const count = prefix.size();
			_statements.insert(
				_statements.begin() + static_cast<std::ptrdiff_t>(i),
				std::make_move_iterator(prefix.begin()),
				std::make_move_iterator(prefix.end()));
			i += count;
			continue;
		}
		awst::forEachChildBlock(*_statements[i], [&](awst::Block& _block, bool) {
			insertBeforeReturns(_block.body, _makePrefix);
		});
	}
}

/// Return-parameter THREADING (mirrors solc IR). A modifier arg or the body may
/// READ/WRITE the named return vars — `mod2(r)`, `m1(x = 2)`, or `r += 1` accumulating
/// across a repeated/looped `_;`. So thread the return params as LEADING in-args through
/// every chain sub and capture them back out at each `_`, letting mutations propagate.
/// These stay native until the outer wrapper return is encoded. Found by the
/// dispatch fuzzer + the chain-as-default experiment.
class ReturnThreading
{
public:
	ReturnThreading(
		solidity::frontend::FunctionDefinition const& _func,
		awst::ContractMethod const& _method,
		std::vector<size_t> const& _writeBackParams,
		std::vector<awst::SubroutineArgument> _extraArgs,
		int _chainId,
		TypeMapper& _typeMapper);

	bool hasRet() const { return m_hasRet; }

	/// Prepend retArgs to a sub's args (returns a fresh combined vector).
	std::vector<awst::SubroutineArgument> withRetArgs(
		std::vector<awst::SubroutineArgument> const& _fnArgs) const
	{
		std::vector<awst::SubroutineArgument> out = m_retArgs;
		out.insert(out.end(), _fnArgs.begin(), _fnArgs.end());
		out.insert(out.end(), m_extraArgs.begin(), m_extraArgs.end());
		return out;
	}

	/// Push the current return-param vars, then the function params, as call args.
	void pushThreadedArgs(
		std::shared_ptr<awst::SubroutineCallExpression> const& _call,
		awst::SourceLocation const& _loc) const
	{
		for (auto const& r: m_retInfos)
			if (!r.isWriteBack)
				awst::pushCallArg(_call->args, r.name,
					awst::makeVarExpression(r.name, r.type, _loc));
		for (auto const& arg: m_method.args)
			awst::pushCallArg(_call->args, arg.name,
				awst::makeVarExpression(arg.name, arg.wtype, _loc));
		for (auto const& arg: m_extraArgs)
			awst::pushCallArg(_call->args, arg.name,
				awst::makeVarExpression(arg.name, arg.wtype, _loc));
	}

	/// Capture a chain call's return into the return-param var(s): `r = call` (one) or
	/// `(r1,…,rN) = call` (many). Appends the assignment to `_dst`.
	void captureReturn(
		std::shared_ptr<awst::Block> const& _dst,
		std::shared_ptr<awst::Expression> _call,
		awst::SourceLocation const& _loc) const
	{
		if (!m_hasRet) { _dst->body.push_back(awst::makeExpressionStatement(std::move(_call), _loc)); return; }
		if (m_retInfos.size() == 1)
		{
			auto tgt = awst::makeVarExpression(m_retInfos[0].name, m_retInfos[0].type, _loc);
			_dst->body.push_back(awst::makeAssignmentStatement(std::move(tgt), std::move(_call), _loc));
		}
		else
		{
			auto tup = awst::makeTupleExpression(m_method.returnType, _loc);
			for (auto const& r: m_retInfos)
				tup->items.push_back(awst::makeVarExpression(r.name, r.type, _loc));
			_dst->body.push_back(awst::makeAssignmentStatement(std::move(tup), std::move(_call), _loc));
		}
	}

	/// Return the threaded return-param(s): `return r` / `return (r1,…,rN)` / bare `return`.
	std::shared_ptr<awst::Statement> makeThreadedReturn(awst::SourceLocation const& _loc) const
	{
		if (!m_hasRet) return awst::makeReturnStatement(nullptr, _loc);
		if (m_retInfos.size() == 1)
			return awst::makeReturnStatement(
				awst::makeVarExpression(m_retInfos[0].name, m_retInfos[0].type, _loc), _loc);
		auto tup = awst::makeTupleExpression(m_method.returnType, _loc);
		for (auto const& r: m_retInfos)
			tup->items.push_back(awst::makeVarExpression(r.name, r.type, _loc));
		return awst::makeReturnStatement(std::move(tup), _loc);
	}

	/// A bare `return;` in a modifier body exits it with the current return-params.
	void threadBareReturns(std::vector<std::shared_ptr<awst::Statement>>& _stmts) const
	{
		for (auto& stmt: _stmts)
		{
			if (auto* ret = dynamic_cast<awst::ReturnStatement*>(stmt.get()))
			{
				if (!ret->value)
					stmt = makeThreadedReturn(ret->sourceLocation);
			}
			else
				awst::forEachChildBlock(*stmt, [&](awst::Block& b, bool) {
					threadBareReturns(b.body);
				});
		}
	}

	/// The wrapper body: zero-init the return-param vars, call the outermost
	/// modifier (threading the return params in), capture them back, and
	/// return them.
	std::shared_ptr<awst::Block> makeEntryBody(std::string const& _outerSubName) const
	{
		auto const& loc = m_method.sourceLocation;
		auto entryBody = awst::makeBlock(loc);

		for (auto const& r: m_retInfos) // zero-init named return vars before the call
		{
			if (r.isWriteBack)
				continue;
			auto target = awst::makeVarExpression(r.name, r.type, loc);
			auto zeroVal = StorageMapper::makeDefaultValue(r.type, loc);
			entryBody->body.push_back(awst::makeAssignmentStatement(
				std::move(target), std::move(zeroVal), loc));
		}

		auto call = awst::makeSubroutineCall(
			awst::InstanceMethodTarget{_outerSubName}, m_method.returnType, loc);
		pushThreadedArgs(call, loc);

		if (m_hasRet)
		{
			captureReturn(entryBody, std::move(call), loc);
			entryBody->body.push_back(makeThreadedReturn(loc));
		}
		else
			entryBody->body.push_back(
				awst::makeExpressionStatement(std::move(call), loc));
		return entryBody;
	}

private:
	struct RetInfo
	{
		std::string name;
		awst::WType const* type;
		bool isWriteBack;
	};

	awst::ContractMethod const& m_method;
	std::vector<RetInfo> m_retInfos;
	bool m_hasRet = false;
	/// Leading return-param args, prepended to every chain sub's signature.
	std::vector<awst::SubroutineArgument> m_retArgs;
	/// Shared memory-root offsets, appended after the Solidity parameters.
	std::vector<awst::SubroutineArgument> m_extraArgs;
};

ReturnThreading::ReturnThreading(
	solidity::frontend::FunctionDefinition const& _func,
	awst::ContractMethod const& _method,
	std::vector<size_t> const& _writeBackParams,
	std::vector<awst::SubroutineArgument> _extraArgs,
	int _chainId,
	TypeMapper& _typeMapper)
	: m_method(_method), m_extraArgs(std::move(_extraArgs))
{
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
			? "__ret_" + std::to_string(_chainId) + "_" + std::to_string(ri)
			: rp->name();
		awst::WType const* rt =
			(retTuple && ri < retTuple->types().size()) ? retTuple->types()[ri]
			: (!retTuple ? _method.returnType : _typeMapper.map(rp->type()));
		m_retInfos.push_back({nm, rt, false});
	}
	for (size_t paramIndex: _writeBackParams)
		if (paramIndex < _method.args.size())
		{
			auto const& arg = _method.args[paramIndex];
			m_retInfos.push_back({arg.name, arg.wtype, true});
		}
	m_hasRet = (_method.returnType != awst::WType::voidType());
	for (auto const& r: m_retInfos)
		if (!r.isWriteBack)
			m_retArgs.emplace_back(r.name, r.type, _method.sourceLocation);
}

} // namespace

std::vector<VariableDeclaration const*>
ContractBuilder::modifierMemoryRootParams(FunctionDefinition const& _func) const
{
	std::map<int64_t, VariableDeclaration const*> candidates;
	for (auto const& parameter: _func.parameters())
		if (isMemoryReference(*parameter) && !parameter->name().empty())
			candidates.emplace(parameter->id(), parameter.get());
	if (candidates.empty())
		return {};

	std::set<int64_t> used;
	for (auto const& invocation: _func.modifiers())
	{
		auto const* modifier = SolcFacts::resolveModifier(
			*invocation, m_currentContract);
		auto const* arguments = invocation->arguments();
		if (!modifier || !arguments)
			continue;
		auto const& parameters = modifier->parameters();
		for (size_t i = 0; i < arguments->size() && i < parameters.size(); ++i)
			if (isMemoryReference(*parameters[i]))
				collectMemoryRoots(*(*arguments)[i], candidates, used);
	}

	std::vector<VariableDeclaration const*> result;
	result.reserve(used.size());
	for (auto const& parameter: _func.parameters())
		if (used.count(parameter->id()))
			result.push_back(parameter.get());
	return result;
}

void ContractBuilder::registerModifierMemoryRootParams(
	FunctionDefinition const& _func)
{
	for (auto const* parameter: modifierMemoryRootParams(_func))
	{
		auto const* type = m_typeMapper.map(parameter->type());
		// Large aggregates already use their declared uint64 BlobOffset calling
		// convention; small values need the chain-local bridge allocated below.
		if (!memoryUsesBlob(type))
			m_functionCtx->scope.bindings.blobAggregates.set(
				parameter->id(), memoryRootName(_func, *parameter));
	}
}

void ContractBuilder::bindModifierArguments(
	solidity::frontend::ModifierInvocation const& _invocation,
	solidity::frontend::ModifierDefinition const& _modifier,
	awst::Block& _modBody,
	std::vector<int64_t>& _remappedDeclIds,
	std::vector<int64_t>& _blobDeclIds)
{
	auto const* args = _invocation.arguments();
	auto const& params = _modifier.parameters();
	if (!args || args->empty())
		return;

	auto modLoc = makeLoc(_invocation.location());
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
				_modBody.body.push_back(awst::makeAssignmentStatement(
					awst::makeVarExpression(param->name(),
						awst::WType::biguintType(), modLoc),
					addr->slot, modLoc));
				_remappedDeclIds.push_back(param->id());
			}
			continue;
		}

		// solc passes memory references as pointer values. Give the modifier
		// parameter its own runtime pointer local: writes through it hit the
		// shared object, while any high-level, tuple, branch/loop, or Yul rebind
		// changes only this local. If the argument is already bridge/blob-backed,
		// preserve its exact pointer (including a runtime conditional selection);
		// otherwise spill the fresh value once.
		if (isMemoryReference(*param))
		{
			std::string const pointerName = uniqueName + "_ptr";
			auto pointer = resolveMemoryPointer(
				*m_exprBuilder, *m_exprBuilder->currentScope,
				*(*args)[pi], modLoc);
			if (pointer)
			{
				m_exprBuilder->appendEffectsTo(_modBody.body);
				_modBody.body.push_back(awst::makeAssignmentStatement(
					awst::makeVarExpression(pointerName,
						awst::WType::uint64Type(), modLoc),
					std::move(pointer), modLoc));
			}
			else
			{
				auto value = m_exprBuilder->buildExpr(*(*args)[pi]);
				if (!value)
					continue;
				value = TypeCoercion::coerceForAssignment(
					std::move(value), paramType, modLoc);
				m_exprBuilder->appendEffectsTo(_modBody.body);
				emitBlobBackValue(
					m_typeMapper, param->type(), paramType,
					std::move(value), pointerName,
					awst::NameGen::next("ModifierChainBuilder.modPointer"),
					modLoc, _modBody.body);
			}
			m_tr->scope.bindings.blobAggregates.set(param->id(), pointerName);
			_blobDeclIds.push_back(param->id());
			continue;
		}

		auto argExpr = m_exprBuilder->buildExpr(*(*args)[pi]);
		if (!argExpr) continue;

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
			m_exprBuilder->appendEffectsTo(_modBody.body);
			sol_ast::StorageAlias alias =
				sol_ast::StorageAlias::classify(std::move(argExpr));
			m_tr->scope.bindings.storageAliases.set(param->id(), std::move(alias));
			_remappedDeclIds.push_back(param->id());
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
		m_exprBuilder->appendEffectsTo(_modBody.body);

		auto target = awst::makeVarExpression(uniqueName, paramType, modLoc);

		auto assignment = awst::makeAssignmentStatement(target, std::move(argExpr), modLoc);
		_modBody.body.push_back(std::move(assignment));

		m_tr->scope.bindings.paramRemaps.set(param->id(), sol_ast::ParamRemap{uniqueName, paramType});
		_remappedDeclIds.push_back(param->id());
	}
}

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
	auto const& cref = m_contractId;

	std::vector<MemoryBridge> memoryBridges;
	std::vector<awst::SubroutineArgument> bridgeArgs;
	for (auto const* parameter: modifierMemoryRootParams(_func))
	{
		auto const* nativeType = m_typeMapper.map(parameter->type());
		if (memoryUsesBlob(nativeType))
			continue; // already a uint64 parameter under the ordinary call plan
		auto found = std::find_if(
			_func.parameters().begin(), _func.parameters().end(),
			[&](auto const& candidate) {
				return candidate->id() == parameter->id();
			});
		if (found == _func.parameters().end())
			continue;
		size_t const index = static_cast<size_t>(
			std::distance(_func.parameters().begin(), found));
		std::string const name = memoryRootName(_func, *parameter);
		memoryBridges.push_back({
			index, parameter, name, m_tr->scope.awstVarName(*parameter), nativeType});
		bridgeArgs.emplace_back(
			name, awst::WType::uint64Type(), makeLoc(parameter->location()));
	}

	ReturnThreading const threading(
		_func, _method, _writeBackParams, std::move(bridgeArgs),
		chainId, m_typeMapper);

	auto writeBackBridgeValues = [&](std::string const& _pointerSuffix,
		awst::SourceLocation const& _loc) {
		std::vector<std::shared_ptr<awst::Statement>> result;
		for (auto const& bridge: memoryBridges)
		{
			if (std::find(_writeBackParams.begin(), _writeBackParams.end(),
					bridge.parameterIndex) == _writeBackParams.end())
				continue;
			auto value = materializeBlobValue(
				m_typeMapper, bridge.declaration->type(), bridge.nativeType,
				bridge.name + _pointerSuffix, _loc, result);
			if (!value || bridge.parameterIndex >= _method.args.size())
				continue;
			auto const& argument = _method.args[bridge.parameterIndex];
			result.push_back(awst::makeAssignmentStatement(
				awst::makeVarExpression(argument.name, argument.wtype, _loc),
				TypeCoercion::coerceForAssignment(
					std::move(value), argument.wtype, _loc), _loc));
		}
		return result;
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
		bodySub.args = threading.withRetArgs(_method.args); // return params (in/out) + function params
		bodySub.body = _method.body; // move the original function body here
		prependDecodes(bodySub.body);
		// The body receives each root pointer by value. Preserve its entry
		// pointer for the existing internal-call write-back convention: member
		// writes change the shared object, but `p = other` only repoints the
		// body's local pointer and must not escape to its caller (solc memory
		// reference semantics).
		std::vector<std::shared_ptr<awst::Statement>> bridgeEntry;
		for (auto const& bridge: memoryBridges)
			if (std::find(_writeBackParams.begin(), _writeBackParams.end(),
					bridge.parameterIndex) != _writeBackParams.end())
				bridgeEntry.push_back(awst::makeAssignmentStatement(
					awst::makeVarExpression(
						bridge.name + "_original", awst::WType::uint64Type(),
						bodySub.sourceLocation),
					awst::makeVarExpression(
						bridge.name, awst::WType::uint64Type(),
						bodySub.sourceLocation),
					bodySub.sourceLocation));
		if (!bridgeEntry.empty())
			bodySub.body->body.insert(
				bodySub.body->body.begin(),
				std::make_move_iterator(bridgeEntry.begin()),
				std::make_move_iterator(bridgeEntry.end()));
		insertBeforeReturns(bodySub.body->body,
			[&](awst::SourceLocation const& loc) {
				return writeBackBridgeValues("_original", loc);
			});
		// A named-return body (`{ r += 1; }`) sets the return-param vars but may fall off
		// the end without a `return` — append one that threads them back out.
		if (bodySub.body && !awst::blockAlwaysTerminates(*bodySub.body))
		{
			for (auto& statement: writeBackBridgeValues(
				"_original", bodySub.sourceLocation))
				bodySub.body->body.push_back(std::move(statement));
			bodySub.body->body.push_back(threading.makeThreadedReturn(bodySub.sourceLocation));
		}
		bodySub.arc4MethodConfig = std::nullopt; // internal, not ABI-routable
		bodySub.pure = _method.pure;
		m_modifierSubroutines.push_back(std::move(bodySub));
	}

	// Modifier subroutines innermost→outermost, each calling the next.
	std::string nextSubName = bodySubName;

	for (int i = static_cast<int>(modifiers.size()) - 1; i >= 0; --i)
	{
		auto const& modInvocation = modifiers[i];
		auto const* modDef =
			SolcFacts::resolveModifier(*modInvocation, m_currentContract);
		if (!modDef)
			continue; // constructor base call — handled elsewhere

		std::string modSubName = baseName + "__mod" + std::to_string(i) + "_" + std::to_string(chainId);

		awst::ContractMethod modSub;
		modSub.sourceLocation = makeLoc(modDef->location());
		modSub.cref = cref;
		modSub.memberName = modSubName;
		modSub.returnType = _method.returnType;
		modSub.args = threading.withRetArgs(_method.args); // return params (in/out) + function params
		modSub.arc4MethodConfig = std::nullopt; // internal
		modSub.pure = _method.pure;

		auto modBody = awst::makeBlock(modSub.sourceLocation);

		// Decode params first so a modifier arg expr (`mArg(a % 5)`) sees native values.
		prependDecodes(modBody);

		std::vector<int64_t> remappedDeclIds;
		std::vector<int64_t> blobDeclIds;
		bindModifierArguments(
			*modInvocation, *modDef, *modBody,
			remappedDeclIds, blobDeclIds);

		// At `_`: thread the return-param(s) in, call nextSubName, capture them back out
		// so a repeated/looped `_;` accumulates and a modifier arg's writes propagate.
		auto makePlaceholder = [&, nextSubName,
			loc = modSub.sourceLocation]() {
			auto placeholderBlock = awst::makeBlock(loc);
			auto call = awst::makeSubroutineCall(
				awst::InstanceMethodTarget{nextSubName}, _method.returnType, loc);
			threading.pushThreadedArgs(call, loc);
			threading.captureReturn(placeholderBlock, std::move(call), loc);
			return placeholderBlock;
		};

		auto translatedBody = buildBlock(modDef->body(), std::move(makePlaceholder));

		if (translatedBody)
		{
			if (threading.hasRet())
				threading.threadBareReturns(translatedBody->body);
			insertBeforeReturns(translatedBody->body,
				[&](awst::SourceLocation const& loc) {
					return writeBackBridgeValues("", loc);
				});

			for (auto& stmt: translatedBody->body)
				modBody->body.push_back(std::move(stmt));
		}

		// Modifier falls off the end → return the threaded return-param values.
		for (auto& statement: writeBackBridgeValues(
			"", modSub.sourceLocation))
			modBody->body.push_back(std::move(statement));
		modBody->body.push_back(threading.makeThreadedReturn(modSub.sourceLocation));

		for (auto declId: remappedDeclIds)
			m_tr->scope.bindings.paramRemaps.erase(declId);
		for (auto declId: blobDeclIds)
			m_tr->scope.bindings.blobAggregates.erase(declId);

		modSub.body = modBody;
		m_modifierSubroutines.push_back(std::move(modSub));
		nextSubName = modSubName;
	}

	// Rewrite _method.body to zero-init the return-param vars, call the outermost modifier
	// (threading the return params in), capture them back, and return them.
	_method.body = threading.makeEntryBody(nextSubName);
	std::vector<std::shared_ptr<awst::Statement>> entryPrefix =
		makeParamDecodeStatements(_paramDecodes);
	for (auto const& bridge: memoryBridges)
		emitBlobBackValue(
			m_typeMapper, bridge.declaration->type(), bridge.nativeType,
			awst::makeVarExpression(
				bridge.valueName, bridge.nativeType,
				_method.sourceLocation),
			bridge.name,
			awst::NameGen::next("ModifierChainBuilder.rootPointer"),
			_method.sourceLocation, entryPrefix);
	if (!entryPrefix.empty())
		_method.body->body.insert(
			_method.body->body.begin(),
			std::make_move_iterator(entryPrefix.begin()),
			std::make_move_iterator(entryPrefix.end()));
	for (auto const& bridge: memoryBridges)
		m_tr->scope.bindings.blobAggregates.erase(bridge.declaration->id());
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
	constructor.cref = m_contractId;
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
		constructor.args.emplace_back(
			m_tr->scope.awstVarName(*parameter),
			m_typeMapper.profile().evmStorageLayout
				&& parameter->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Storage
				? awst::WType::biguintType() : m_typeMapper.map(parameter->type()),
			makeLoc(parameter->location()));
	}

	solidity::ScopedSaveAndRestore returnTypeGuard(
		m_functionCtx->returnType, awst::WType::voidType());
	solidity::ScopedSaveAndRestore frameGuard(m_functionCtx->frameIsProgram, true);
	buildModifierChain(_func, constructor, _contractName);
	_body = std::move(constructor.body);
}

} // namespace puyasol::builder
