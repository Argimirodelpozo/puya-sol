#include "builder/contract/ContractBuilder.h"
#include "builder/ProgramAnalysis.h"
#include "builder/itxn/InnerCallHandlers.h"
#include "builder/storage/EvmLayoutMode.h"
#include "awst/Termination.hpp"
#include "awst/StatementWalk.h"
#include "awst/Visit.h"
#include "builder/AWSTBuilder.h"
#include "builder/NatSpecTags.h"
#include "builder/assembly/AssemblyBuilder.h"
#include "builder/contract/ParamABIValidator.h"
#include "builder/sol-ast/stmts/SolBlock.h"
#include "builder/sol-ast/StorageRefPointer.h"
#include "builder/itxn/CallResolver.h"
#include "builder/proxies/UupsLowering.h"
#include "builder/sol-types/OverloadSuffix.h"
#include "builder/sol-types/RefParamPassing.h"
#include "builder/sol-types/SolIntType.h"
#include "builder/sol-types/Arc4Defaults.h"
#include "builder/sol-types/TypeCoercion.h"
#include "Logger.h"

#include <libsolidity/ast/ASTVisitor.h>
// yul nodes BY VALUE (the AST aliases are std::variant, which needs
// complete types). Kept out of AssemblyBuilder.h so only the TUs that
// actually instantiate them pay the ~223k lines.
#include <libyul/AST.h>
#include <libyul/Dialect.h>

namespace puyasol::builder
{

using awst::blockAlwaysTerminates;

awst::ContractMethod ContractBuilder::buildClearProgram(
	solidity::frontend::ContractDefinition const& _contract,
	std::string const& _contractName
)
{
	awst::ContractMethod method;
	method.sourceLocation = makeLoc(_contract.location());
	method.returnType = awst::WType::boolType();
	method.cref = m_sourceFile + "." + _contractName;
	method.memberName = "clear_state_program";

	auto body = awst::makeBlock(method.sourceLocation);

	// return true
	auto ret = awst::makeReturnStatement(awst::makeTrue(method.sourceLocation), method.sourceLocation);

	body->body.push_back(ret);
	method.body = body;

	return method;
}

namespace {

std::vector<ReturnWireElem> computeReturnPlan(
	solidity::frontend::FunctionDefinition const& function,
	awst::WType const* returnType,
	TypeMapper& typeMapper)
{
	auto const& returnParams = function.returnParameters();
	auto const* tuple = returnType
		&& returnType->kind() == awst::WTypeKind::WTuple
		? static_cast<awst::WTuple const*>(returnType) : nullptr;
	std::vector<ReturnWireElem> plan;
	for (size_t i = 0; i < returnParams.size(); ++i)
	{
		ReturnWireElem item;
		item.nativeType = tuple
			? (i < tuple->types().size() ? tuple->types()[i] : nullptr)
			: returnType;
		item.wireType = item.nativeType;
		auto integer = SolIntType::fromSolOrEnum(returnParams[i]->type());
		if (item.nativeType == awst::WType::biguintType())
		{
			item.isSigned = integer && integer->isSigned;
			item.bits = integer ? integer->bits : 256u;
			unsigned const wireWidth = item.isSigned ? 256u : item.bits;
			item.wireType = typeMapper.createType<awst::ARC4UIntN>(
				static_cast<int>(wireWidth));
			item.encoded = true;
		}
		else if (integer && !integer->isSigned && integer->bits < 64)
		{
			item.masked = true;
			item.bits = integer->bits;
		}
		else if (item.nativeType
			&& item.nativeType->kind() == awst::WTypeKind::ReferenceArray)
		{
			auto const* arc4 = typeMapper.mapToARC4Type(item.nativeType);
			if (arc4 != item.nativeType)
			{
				item.wireType = arc4;
				item.encoded = true;
			}
		}
		plan.push_back(item);
	}
	return plan;
}

void transformReturnValues(
	std::vector<std::shared_ptr<awst::Statement>>& statements,
	TypeMapper& typeMapper,
	std::vector<ReturnWireElem> const& plan,
	bool asmWrap,
	bool wire)
{
	for (size_t i = 0; i < statements.size(); ++i)
	{
		if (auto* ret = dynamic_cast<awst::ReturnStatement*>(statements[i].get()))
		{
			if (!ret->value) continue;
			auto const loc = ret->value->sourceLocation;
			std::vector<std::shared_ptr<awst::Statement>> prepend;
			ret->value = TypeCoercion::encodeReturnValue(
				typeMapper, std::move(ret->value), plan, loc, prepend,
				asmWrap, wire);
			auto const inserted = prepend.size();
			statements.insert(
				statements.begin() + static_cast<std::ptrdiff_t>(i),
				std::make_move_iterator(prepend.begin()),
				std::make_move_iterator(prepend.end()));
			i += inserted;
			continue;
		}
		awst::forEachChildBlock(*statements[i], [&](awst::Block& block, bool) {
			transformReturnValues(
				block.body, typeMapper, plan, asmWrap, wire);
		});
	}
}

void normalizeNativeReturns(
	awst::ContractMethod& method,
	solidity::frontend::FunctionDefinition const& function,
	TypeMapper& typeMapper,
	std::vector<ReturnWireElem> plan,
	bool asmWrap,
	bool applyWireRules)
{
	if (!method.body || plan.empty()) return;
	if (!applyWireRules)
		for (auto& item: plan)
		{
			item.isSigned = false;
			item.encoded = false;
			item.masked = false;
		}
	transformReturnValues(
		method.body->body, typeMapper, plan, asmWrap, /*wire=*/false);

	// A scalar named return can be assigned with the mapped native type while
	// the ABI method threads a promoted biguint. Keep the assignment itself
	// type-correct; the return transform above handles its final signed form.
	auto const& returns = function.returnParameters();
	if (returns.size() != 1 || returns[0]->name().empty()
		|| !awst::isNumericWType(plan[0].nativeType))
		return;
	std::string const name = returns[0]->name();
	std::function<void(std::vector<std::shared_ptr<awst::Statement>>&)> walk;
	walk = [&](std::vector<std::shared_ptr<awst::Statement>>& body) {
		for (auto& statement: body)
		{
			if (auto* assignment =
				dynamic_cast<awst::AssignmentStatement*>(statement.get()))
				if (auto* target =
					dynamic_cast<awst::VarExpression*>(assignment->target.get());
					target && target->name == name && assignment->value
					&& awst::isNumericWType(assignment->value->wtype)
					&& assignment->value->wtype != plan[0].nativeType)
				{
					auto const loc = assignment->value->sourceLocation;
					assignment->value = TypeCoercion::implicitNumericCast(
						std::move(assignment->value), plan[0].nativeType, loc);
					target->wtype = plan[0].nativeType;
				}
			awst::forEachChildBlock(*statement, [&](awst::Block& block, bool) {
				walk(block.body);
			});
		}
	};
	walk(method.body->body);
}

void promoteReturnType(
	awst::ContractMethod& method,
	std::vector<ReturnWireElem> const& plan,
	TypeMapper& typeMapper)
{
	if (plan.empty()) return;
	if (plan.size() == 1)
		method.returnType = plan[0].wireType;
	else
	{
		std::vector<awst::WType const*> wireTypes;
		for (auto const& item: plan)
			wireTypes.push_back(item.wireType);
		method.returnType = typeMapper.createType<awst::WTuple>(
			std::move(wireTypes));
	}
}

// Value-model reference parameters need an explicit post-call value. Contract
// internal methods only need this for mutated memory structs; host-bound
// library/free functions also mirror the storage write-backs of root
// subroutines. The returned indices are threaded through modifier placeholders.
std::vector<size_t> augmentMethodForReferenceParams(
	awst::ContractMethod& method,
	solidity::frontend::FunctionDefinition const& func,
	TypeMapper& typeMapper,
	solidity::frontend::ContractDefinition const* mostDerived,
	bool asInternalCopy,
	std::set<int64_t> const& asmSlotParamIds)
{
	using namespace solidity::frontend;
	if (!func.isImplemented() || !method.body) return {};

	auto const* scope = func.annotation().contract;
	bool const isFreestanding = func.isFree() || (scope && scope->isLibrary());
	bool const isInternalMethod = !isFreestanding
		&& func.visibility() == Visibility::Internal;
	bool const isHostedFreestanding = asInternalCopy && isFreestanding
		&& func.visibility() != Visibility::Private;
	if (!isInternalMethod && !isHostedFreestanding)
		return {};

	std::vector<size_t> writeBackParams;
	if (isHostedFreestanding
		&& !typeMapper.profile().evmStorageLayout
		&& func.stateMutability() != StateMutability::View
		&& func.stateMutability() != StateMutability::Pure)
		for (size_t pi = 0;
			pi < func.parameters().size() && pi < method.args.size(); ++pi)
		{
			auto const& parameter = func.parameters()[pi];
			if (parameter->referenceLocation()
					!= VariableDeclaration::Location::Storage)
				continue;
			auto const passing = classifyRefParamPassing(
				typeMapper, *parameter,
				asmSlotParamIds.count(parameter->id()) != 0);
			if (passing == RefParamPassing::Value)
				writeBackParams.push_back(pi);
		}

	auto isMemRefType = isMemoryRefWriteBackType;
	auto const& mutations = typeMapper.analysis().parameterMutations(
		isFreestanding ? nullptr : mostDerived, func);

	for (size_t pi = 0; pi < func.parameters().size() && pi < method.args.size(); ++pi)
	{
		auto const& p = func.parameters()[pi];
		if (p->referenceLocation() != VariableDeclaration::Location::Memory) continue;
		if (!p->type() || !isMemRefType(p->type())) continue;
		if (!mutations.mutates(pi)) continue;
		// Root library/free subroutines pass large aggregates by shared blob
		// offset, so their host-bound equivalents must not add a tuple slot.
		if (isFreestanding && memoryUsesBlob(typeMapper.map(p->type()))) continue;
		writeBackParams.push_back(pi);
	}
	if (writeBackParams.empty()) return {};

	auto const& loc = method.sourceLocation;

	// Augment the return type: original values (flattened), then storage and
	// memory reference parameters in the order expected by SolInternalCall.
	std::vector<awst::WType const*> types;
	bool origVoid = (method.returnType == awst::WType::voidType());
	auto const* origTuple = origVoid ? nullptr
		: dynamic_cast<awst::WTuple const*>(method.returnType);
	if (!origVoid)
	{
		if (origTuple) for (auto const* t : origTuple->types()) types.push_back(t);
		else types.push_back(method.returnType);
	}
	for (size_t idx : writeBackParams) types.push_back(method.args[idx].wtype);
	awst::WType const* newRetType =
		types.size() == 1 ? types[0] : typeMapper.createType<awst::WTuple>(std::move(types));
	method.returnType = newRetType;
	bool newIsTuple = (dynamic_cast<awst::WTuple const*>(newRetType) != nullptr);

	// Walk existing returns; append the reference-param vars to match the new shape.
	// forEachReturnStatement covers ALL nesting (if/else, nested blocks,
	// loops, switch) — the old hand-rolled walk recursed only IfElse, so an
	// early `return` inside a for loop kept its unaugmented value and puya
	// rejected valid Solidity with a return-type mismatch.
	awst::forEachReturnStatement(method.body->body, [&](awst::ReturnStatement& ret) {
		if (!newIsTuple)
		{
			if (!ret.value && writeBackParams.size() == 1)
				ret.value = awst::makeVarExpression(
					method.args[writeBackParams[0]].name,
					method.args[writeBackParams[0]].wtype,
					ret.sourceLocation);
		}
		else
		{
			auto tuple = awst::makeTupleExpression(newRetType, ret.sourceLocation);
			if (ret.value)
			{
				if (auto* ot = dynamic_cast<awst::TupleExpression*>(ret.value.get()))
					for (auto& it : ot->items) tuple->items.push_back(it);
				else tuple->items.push_back(ret.value);
			}
			for (size_t idx : writeBackParams)
				tuple->items.push_back(awst::makeVarExpression(
					method.args[idx].name, method.args[idx].wtype, ret.sourceLocation));
			ret.value = std::move(tuple);
		}
	});

	// Fall-through: only void methods reach here un-terminated (buildFunction
	// already synthesized non-void returns). Return the reference params.
	if (!awst::blockAlwaysTerminates(*method.body))
	{
		auto implicit = awst::makeReturnStatement(nullptr, loc);
		if (!newIsTuple && writeBackParams.size() == 1)
			implicit->value = awst::makeVarExpression(
				method.args[writeBackParams[0]].name,
				method.args[writeBackParams[0]].wtype, loc);
		else
		{
			auto tuple = awst::makeTupleExpression(newRetType, loc);
			for (size_t idx : writeBackParams)
				tuple->items.push_back(awst::makeVarExpression(
					method.args[idx].name, method.args[idx].wtype, loc));
			implicit->value = std::move(tuple);
		}
		method.body->body.push_back(std::move(implicit));
	}
	return writeBackParams;
}
} // namespace

namespace
{
/// The ARC-4 parameter remap for an ABI-routed method: rewrite each remappable
/// arg's wtype to its ARC-4 form (except asm bodies) and record the decodes the
/// prologue must emit. Pure extraction from buildFunction — the returned list
/// feeds the decode-rename loop and the entry checks exactly as before.
std::vector<ParamDecode> collectArc4ParamRemaps(
	TypeMapper& _typeMapper,
	solidity::frontend::FunctionDefinition const& _func,
	awst::ContractMethod& method,
	bool funcHasInlineAssembly)
{
std::vector<ParamDecode> paramDecodes;
// Detect inline assembly (at ANY depth — `unchecked { assembly {..} }` counts):
// skip ARC4 param wrapping (would break asm var refs).
// Self-recursive callsubs are rewritten post-translation to wrap biguint args
// in ARC4Encode (see wrap pass below) — self-recursion no longer gates the remap.
if (method.arc4MethodConfig.has_value())
{
	auto const& solParams = _func.parameters();
	for (size_t pi = 0; pi < method.args.size(); ++pi)
	{
		auto& arg = method.args[pi];

		// Remap biguint → ARC4UIntN(N): without this puya uses uint512 (AVM max),
		// breaking ABI selectors. Skipped for asm bodies (would break Yul refs).
		if (arg.wtype == awst::WType::biguintType() && pi < solParams.size())
		{
			auto intInfo = builder::SolIntType::fromSol(solParams[pi]->annotation().type);
			unsigned bits = intInfo ? intInfo->bits : 256;
			auto const* arc4Type = _typeMapper.createType<awst::ARC4UIntN>(static_cast<int>(bits));
			// Signed sub-256 (64<N<256) decodes to N-bit two's complement; sign-extend
			// to 256-bit so downstream ops (compare, negate) see the correct sign.
			// int256 is already canonical; ≤64-bit is uint64-backed (buildABIEntryChecks).
			unsigned signedBits =
				(intInfo && intInfo->isSigned && bits > 64 && bits < 256) ? bits : 0;
			paramDecodes.push_back({pi, arg.name, arg.wtype, arc4Type,
				arg.sourceLocation, signedBits});
			// Asm bodies are built (buildBlock) AFTER this loop; defer the ABI wtype change so the Yul
			// body builds against the native biguint type (set in the decode rename loop below).
			if (!funcHasInlineAssembly)
				arg.wtype = arc4Type;
			continue;
		}

		// Remap aggregate types and profile-sized external fn-ptrs to ARC4.
		// General bytes/bytes[N] params are NOT remapped.
		bool isAggregate = arg.wtype
			&& (arg.wtype->kind() == awst::WTypeKind::ReferenceArray
				|| arg.wtype->kind() == awst::WTypeKind::ARC4StaticArray
				|| arg.wtype->kind() == awst::WTypeKind::ARC4DynamicArray
				|| arg.wtype->kind() == awst::WTypeKind::WTuple);
		if (!isAggregate && pi < solParams.size()) // external fn-ptr bytes[N]
		{
			if (dynamic_cast<solidity::frontend::FunctionType const*>(solParams[pi]->type())
				&& arg.wtype && arg.wtype->kind() == awst::WTypeKind::Bytes)
				isAggregate = true;
		}
		// Skip remap for asm bodies: decode is also suppressed there, so remapping
		// without a decode would leave the body reading ARC4 where it expects native.
		if (!isAggregate || funcHasInlineAssembly)
			continue;

		awst::WType const* arc4Type = _typeMapper.mapToARC4Type(arg.wtype);
		if (arc4Type != arg.wtype)
		{
			paramDecodes.push_back(
				{pi, arg.name, arg.wtype, arc4Type, arg.sourceLocation});
			arg.wtype = arc4Type;
		}
	}
}
	return paramDecodes;
}
} // namespace

namespace
{

// buildFunction phase: rewrite `return stateVar[idx]` to return just the
// uint64 index; call sites reconstitute the location (SolInternalCall).
// Caller guards storageRefPointerReturn.
void rewriteStorageRefReturnIndices(awst::ContractMethod& method)
{
	std::function<void(std::vector<std::shared_ptr<awst::Statement>>&)> rewriteRet;
	rewriteRet = [&](std::vector<std::shared_ptr<awst::Statement>>& stmts)
	{
		for (auto& stmt: stmts)
		{
			if (auto* ret = dynamic_cast<awst::ReturnStatement*>(stmt.get()))
			{
				if (auto* index = dynamic_cast<awst::IndexExpression*>(
						ret->value.get()))
					ret->value = TypeCoercion::implicitNumericCast(
						index->index, awst::WType::uint64Type(),
						ret->value->sourceLocation);
			}
			else
			{
				// The old hand-written list missed loops and switches.
				awst::forEachChildBlock(*stmt, [&](awst::Block& block, bool) {
					rewriteRet(block.body);
				});
			}
		}
	};
	rewriteRet(method.body->body);
}

// Rename remapped wire arguments once. Decode statements themselves are built
// later, once per modifier-chain member (or once for an unmodified method).
void applyParamDecodeNames(
	std::vector<ParamDecode> const& paramDecodes,
	awst::ContractMethod& method)
{
	for (auto const& pd: paramDecodes)
	{
		std::string arc4Name = "__arc4_" + pd.name;
		for (auto& arg: method.args)
		{
			if (arg.name == pd.name)
			{
				arg.name = arc4Name;
				arg.wtype = pd.arc4Type;
				break;
			}
		}
	}
}

// No-modifier-chain buildFunction phase: insert the deferred ARC4 decodes at
// the top of the method body, then wrap self-recursive callsub args whose
// positions were remapped. Caller guards non-empty deferredDecodes.
void insertDeferredDecodes(
	awst::ContractMethod& method,
	solidity::frontend::FunctionDefinition const& _func,
	std::vector<std::shared_ptr<awst::Statement>>& deferredDecodes,
	std::vector<ParamDecode> const& paramDecodes,
	eb::ContractContext& contractCtx)
{
	method.body->body.insert(
		method.body->body.begin(),
		std::make_move_iterator(deferredDecodes.begin()),
		std::make_move_iterator(deferredDecodes.end()));

	// Self-recursive callsub fix-up: after param remap, internal f(...) calls
	// still pass biguint args; wrap each remapped position in ARC4Encode.
	std::string thisName = eb::CallResolver::resolveMethodName(
		contractCtx, _func);
	awst::visitExpressions(*method.body, [&](awst::Expression& expression) {
		auto* call = dynamic_cast<awst::SubroutineCallExpression*>(&expression);
		if (!call)
			return;
		auto const* target = std::get_if<awst::InstanceMethodTarget>(
			&call->target);
		if (!target || target->memberName != thisName)
			return;
		for (auto const& pd: paramDecodes)
		{
			if (pd.argIndex >= call->args.size()) continue;
			auto& arg = call->args[pd.argIndex];
			if (!arg.value || arg.value->wtype == pd.arc4Type)
				continue;
			if (arg.value->wtype != awst::WType::biguintType())
				continue;
			auto loc = arg.value->sourceLocation;
			arg.value = awst::makeARC4Encode(
				std::move(arg.value), pd.arc4Type, loc);
		}
	});
}

} // anonymous namespace

std::vector<std::shared_ptr<awst::Statement>>
ContractBuilder::makeParamDecodeStatements(
	std::vector<ParamDecode> const& _paramDecodes)
{
	std::vector<std::shared_ptr<awst::Statement>> statements;
	statements.reserve(_paramDecodes.size());
	for (auto const& decode: _paramDecodes)
	{
		auto wireValue = awst::makeVarExpression(
			"__arc4_" + decode.name, decode.arc4Type, decode.loc);
		std::shared_ptr<awst::Expression> nativeValue;
		auto const* array = dynamic_cast<awst::ReferenceArray const*>(
			decode.nativeType);
		if (array && !array->arraySize().has_value())
			nativeValue = awst::makeConvertArray(
				std::move(wireValue), decode.nativeType, decode.loc);
		else
		{
			nativeValue = awst::makeARC4Decode(
				std::move(wireValue), decode.nativeType, decode.loc);
			if (decode.signedBits > 0)
				nativeValue = TypeCoercion::signExtendToUint256(
					std::move(nativeValue), decode.signedBits, decode.loc);
		}
		statements.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(
				decode.name, decode.nativeType, decode.loc),
			std::move(nativeValue), decode.loc));
	}
	return statements;
}

/// buildFunction phase: name/cref, args (with storage/blob wtype overrides), and the companion offset params for offset-convention …
void ContractBuilder::buildMethodSignature(
	awst::ContractMethod& method,
	solidity::frontend::FunctionDefinition const& _func,
	std::string const& _contractName,
	std::string const& _nameOverride,
	std::set<int64_t> const& asmSlotParamIds)
{
	method.sourceLocation = makeLoc(_func.location());
	method.cref = m_sourceFile + "." + _contractName;
	if (!_nameOverride.empty())
	{
		method.memberName = _nameOverride;
	}
	else
	{
		using solidity::frontend::Visibility;
		auto const* symbol = m_functionSymbols.resolve(_func.id());
		if (symbol && (_func.visibility() == Visibility::Internal
				|| _func.visibility() == Visibility::Private))
			method.memberName = *symbol;
		else
		{
			method.memberName = _func.name();
			if (m_overloadedNames.count(_func.name()))
				appendOverloadSuffix(method.memberName, _func);
		}
	}

	// Documentation
	if (_func.documentation())
		method.documentation.description = *_func.documentation()->text();

	// Parameters
	int paramIndex = 0;
	for (auto const& param: _func.parameters())
	{
		awst::SubroutineArgument arg;
		if (param->name().empty())
			arg.name = "_param" + std::to_string(paramIndex);
		else
			arg.name = param->name();
		arg.sourceLocation = makeLoc(param->location());
		// Slot handle / box-key prefix / blob offset / value — the shared
		// travel rule (RefParamPassing.h); call sites classify identically.
		arg.wtype = refParamWType(
			classifyRefParamPassing(
				m_typeMapper, *param, asmSlotParamIds.count(param->id()) != 0),
			m_typeMapper, *param);
		method.args.push_back(std::move(arg));
		paramIndex++;
	}

	// Handle-model dual handle: offset-convention struct-ref params (those that receive an
	// array-element ref `f(arr[i])` somewhere) get a companion uint64 OFFSET param, appended
	// after all regular params. The caller appends matching offset args in the same order; the
	// body's `s.field` writes target the element slice via box_replace(key, offset+fieldOff).
	for (auto const& param: _func.parameters())
		if (!m_typeMapper.profile().evmStorageLayout   // slot handles carry the element position directly
			&& param->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Storage
			&& !param->name().empty()
			&& m_typeMapper.analysis().structRefOffsetParams.count(param->id()))
		{
			awst::SubroutineArgument offArg;
			offArg.name = param->name() + "__off";
			offArg.sourceLocation = makeLoc(param->location());
			offArg.wtype = awst::WType::uint64Type();
			method.args.push_back(std::move(offArg));
		}

}

/// buildFunction phase: native return type mapping (single or tuple), including
/// the biguint promotion required at signed ABI boundaries.
void ContractBuilder::computeMethodReturnType(
	awst::ContractMethod& method,
	solidity::frontend::FunctionDefinition const& _func,
	bool funcHasInlineAssembly)
{
	auto const& returnParams = _func.returnParameters();

	if (returnParams.empty())
		method.returnType = awst::WType::voidType();
	else if (returnParams.size() == 1)
	{
		method.returnType = m_typeMapper.map(returnParams[0]->type());
		// --evm-storage-layout: ANY storage ref return is a biguint slot.
		if (m_typeMapper.profile().evmStorageLayout
			&& returnParams[0]->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Storage)
			method.returnType = awst::WType::biguintType();
		// .slot assembly storage ref: return biguint (slot number).
		else if (returnParams[0]->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Storage
			&& funcHasInlineAssembly)
			method.returnType = awst::WType::biguintType();
		// Storage-ref pointer (`return _pools[id]`): return uint64 index or bytes box-key.
		// Box-keyed when the holder is a mapping (storageRefReturnIsBytesKeyed),
		// even for plain-struct elements with no nested mappings (V4 Position.State).
		else if (storageRefPointerReturn(&_func))
			method.returnType = storageRefReturnIsBytesKeyed(&_func)
				? awst::WType::bytesType()
				: awst::WType::uint64Type();
		// Signed ≤64-bit returns → biguint for proper 256-bit two's complement ARC4 encoding.
		auto intInfo = builder::SolIntType::fromSolOrEnum(returnParams[0]->type());
		// Biguint promotion only at ABI boundary; private/internal keep uint64
		// so `return IntegerConstant(uint64,…)` matches declared type.
		bool isAbiBoundary = _func.isPartOfExternalInterface();
		if (intInfo && intInfo->isSigned)
		{
			if (intInfo->bits <= 64 && isAbiBoundary)
				method.returnType = awst::WType::biguintType();
		}
	}
	else
	{
		// Multiple returns → tuple
		std::vector<awst::WType const*> types;
		std::vector<std::string> names;
		bool hasNames = false;
		for (size_t ri = 0; ri < returnParams.size(); ++ri)
		{
			auto const& rp = returnParams[ri];
			auto* mappedType = m_typeMapper.map(rp->type());
			if (m_typeMapper.profile().evmStorageLayout
				&& rp->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Storage)
				mappedType = awst::WType::biguintType();
			auto intInfo = builder::SolIntType::fromSolOrEnum(rp->type());
			bool isAbiBoundary = _func.isPartOfExternalInterface();
			if (intInfo)
			{
				if (intInfo->isSigned)
				{
					if (intInfo->bits <= 64 && isAbiBoundary)
						mappedType = awst::WType::biguintType();
				}
			}
			types.push_back(mappedType);
			names.push_back(rp->name());
			if (!rp->name().empty())
				hasNames = true;
		}
		if (hasNames)
		{
			// Suffix "Return" to avoid ARC56 collision across methods.
			std::string tupleName = _func.name() + "Return";
			method.returnType = m_typeMapper.createType<awst::WTuple>(
				std::move(types), std::move(names), std::move(tupleName));
		}
		else
			method.returnType = m_typeMapper.createType<awst::WTuple>(std::move(types));
	}

}

/// buildFunction phase: seed the assembly-translation function context from the (ARC4-remapped) method args plus sub-64-bit widths …
void ContractBuilder::setupBodyParamContext(
	awst::ContractMethod const& method,
	solidity::frontend::FunctionDefinition const& _func)
{
	// Use ARC4-remapped types from method.args for the assembly translation context.
	{
		std::vector<std::pair<std::string, awst::WType const*>> paramContext;
		std::map<std::string, unsigned> bitWidths;
		std::map<std::string, solidity::frontend::Type const*> paramSolTypes;
		for (auto const& arg: method.args)
			paramContext.emplace_back(arg.name, arg.wtype);
		// Collect sub-64-bit widths from function params and return params
		for (auto const& p: _func.parameters())
		{
			paramSolTypes[p->name()] = p->annotation().type;
			if (auto it = builder::SolIntType::fromSol(p->annotation().type);
				it && it->bits < 64)
				bitWidths[p->name()] = it->bits;
		}
		for (auto const& rp: _func.returnParameters())
		{
			if (auto it = builder::SolIntType::fromSol(rp->annotation().type);
				it && it->bits < 64)
				bitWidths[rp->name()] = it->bits;
		}
		setFunctionContext(paramContext, method.returnType, bitWidths, paramSolTypes);
		for (auto const& rp: _func.returnParameters())
			m_functionCtx->returnSolTypes.push_back(rp->type());
	}


}

/// buildFunction phase: register named returns, mapping-storage-ref params, slot-handle params, and blob-backed memory params on …
void ContractBuilder::registerBodyRefParams(
	solidity::frontend::FunctionDefinition const& _func,
	std::set<int64_t> const& asmSlotParamIds)
{
	auto const& returnParams = _func.returnParameters();
	// Stash named-return decls for buildBlock (registers >4KB memory returns as blob-backed).
	std::vector<solidity::frontend::VariableDeclaration const*> namedReturnDecls;
	for (auto const& rp: returnParams)
		if (!rp->name().empty())
			namedReturnDecls.push_back(rp.get());
	setNamedReturns(namedReturnDecls);

	// Mapping-storage-ref params: stash for buildBlock to register as mapping-key-params
	// so `m[k]` resolves the dynamic box-key prefix from the runtime bytes value of m.
	// Covers both input params (storage m) and named returns (storage r assigned r=m1).
	std::vector<solidity::frontend::VariableDeclaration const*> mappingKeyParamDecls;
	auto isMappingStorageRef = [&](solidity::frontend::VariableDeclaration const* p) {
		return !m_typeMapper.profile().evmStorageLayout   // slot handles replace box-key prefixes
			&& p->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Storage
			&& (isBoxKeyedStorageRef(p->type(), m_typeMapper.analysis())
				|| asmSlotParamIds.count(p->id()))
			&& !p->name().empty();
	};
	for (auto const& p: _func.parameters())
		if (isMappingStorageRef(p.get()))
			mappingKeyParamDecls.push_back(p.get());
	for (auto const& rp: returnParams)
		// Also register box-keyed storage-ref named returns (e.g. V4 Position.State
		// storage): storageRefReturnIsBytesKeyed catches the mapping-holder case
		// that containsMappingType misses for plain-struct elements.
		if (isMappingStorageRef(rp.get())
			|| (rp->referenceLocation()
					== solidity::frontend::VariableDeclaration::Location::Storage
				&& !rp->name().empty() && storageRefReturnIsBytesKeyed(&_func)))
			mappingKeyParamDecls.push_back(rp.get());
	setMappingKeyParams(mappingKeyParamDecls);
	for (auto const& p: _func.parameters())
		if (asmSlotParamIds.count(p->id()) && !p->name().empty()
			&& !m_typeMapper.profile().evmStorageLayout)
			m_functionCtx->boxKeyStructParams[p->name()] =
				m_typeMapper.map(p->type());

	// --evm-storage-layout: storage params + named storage returns are
	// biguint slot handles; register so body access resolves through them.
	std::vector<solidity::frontend::VariableDeclaration const*> slotRefParamDecls;
	if (m_typeMapper.profile().evmStorageLayout)
	{
		for (auto const& p: _func.parameters())
			if (p->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Storage
				&& !p->name().empty())
				slotRefParamDecls.push_back(p.get());
		for (auto const& rp: returnParams)
			if (rp->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Storage
				&& !rp->name().empty())
				slotRefParamDecls.push_back(rp.get());
	}
	setSlotRefParams(slotRefParamDecls);

	// Blob-backed (>4KB) memory params: stash so body's p.field[i] routes to the blob.
	std::vector<solidity::frontend::VariableDeclaration const*> blobAggParamDecls;
	for (auto const& p: _func.parameters())
		if (p->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Memory
			&& !p->name().empty()
			&& memoryUsesBlob(m_typeMapper.map(p->type())))
			blobAggParamDecls.push_back(p.get());
	setBlobAggParams(blobAggParamDecls);

}

/// buildFunction phase: zero-init named return vars (Solidity implicit init) and bump the free-memory pointer for memory-typed …
void ContractBuilder::prependNamedReturnInits(
	awst::ContractMethod& method,
	solidity::frontend::FunctionDefinition const& _func)
{
	// Zero-init named return vars (Solidity implicit init); bump free-memory pointer
	// for every memory-typed return (EVM allocates at entry; tests probe FMP movement).
	// For a CHAIN-lowered modifier'd function the return params are THREADED in/out as
	// call args (buildModifierChain), so the OUTER method zero-inits them once — doing it
	// again in the body would reset the value on every repeated `_;` (no accumulation).
	// Skip the VALUE zero-inits there (keep the memory FMP bumps).
	bool const chainLowered = !_func.modifiers().empty();
	{
		auto const& retParams = _func.returnParameters();
		std::vector<std::shared_ptr<awst::Statement>> inits;
		for (auto const& rp: retParams)
		{
			if (chainLowered) break;   // value zero-init handled by the chain's outer method
			if (rp->name().empty())
				continue;
			// Box-keyed storage-ref named returns hold a bytes box-key, not a struct — skip zero-init.
			if (rp->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Storage
				&& storageRefReturnIsBytesKeyed(&_func))
				continue;
			// --evm-storage-layout: the named return holds a biguint slot.
			if (m_typeMapper.profile().evmStorageLayout
				&& rp->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Storage)
			{
				inits.push_back(awst::makeAssignmentStatement(
					awst::makeVarExpression(rp->name(), awst::WType::biguintType(),
						method.sourceLocation),
					awst::makeZero(method.sourceLocation, awst::WType::biguintType()),
					method.sourceLocation));
				continue;
			}
			auto* rpType = m_typeMapper.map(rp->type());

			// >4KB memory returns: pre-zeroed in preamble; skip bzero (pointer model).
			if (rp->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Memory
				&& memoryUsesBlob(rpType))
				continue;

			auto target = awst::makeVarExpression(rp->name(), rpType, method.sourceLocation);

			auto zeroVal = StorageMapper::makeDefaultValue(rpType, method.sourceLocation);

			auto assign = awst::makeAssignmentStatement(std::move(target), std::move(zeroVal), method.sourceLocation);
			inits.push_back(std::move(assign));
		}
		for (auto const& rp: retParams)
		{
			if (rp->referenceLocation()
				!= solidity::frontend::VariableDeclaration::Location::Memory)
				continue;
			auto* rpType = m_typeMapper.map(rp->type());
			int sz = computeEncodedElementSize(rpType);
			if (sz <= 0)
				continue;
			// Blob-backed memory return: bind FMP (before bump) to __blobagg_off_<id>
			// to match blob-aggregate registration in ContractBuilder::buildBlock.
			if (memoryUsesBlob(rpType))
			{
				std::string offN = "__blobagg_off_" + std::to_string(rp->id());
				auto blob = awst::makeLoadSlot(
					m_typeMapper.profile().scratchLayout.memoryFirst(),
					method.sourceLocation);
				auto base = awst::makeExtractUInt64(std::move(blob),
					awst::makeIntegerConstant("88", method.sourceLocation), method.sourceLocation);
				inits.push_back(awst::makeAssignmentStatement(
					awst::makeVarExpression(offN, awst::WType::uint64Type(), method.sourceLocation),
					std::move(base), method.sourceLocation));
			}
			for (auto& s: AssemblyBuilder::emitFreeMemoryBump(
					m_typeMapper.profile().scratchLayout, sz,
					method.sourceLocation, static_cast<int>(rp->id())))
				inits.push_back(std::move(s));
		}
		if (!inits.empty())
		{
			method.body->body.insert(
				method.body->body.begin(),
				std::make_move_iterator(inits.begin()),
				std::make_move_iterator(inits.end())
			);
		}
	}

}

/// buildFunction phase: synthesize the implicit return (named vars or default zero) when the body can fall off the end, including …
void ContractBuilder::synthesizeImplicitReturn(
	awst::ContractMethod& method,
	solidity::frontend::FunctionDefinition const& _func,
	bool encodeReturnsAtBuildTime,
	std::vector<ReturnWireElem> const& returnPlan,
	bool funcHasInlineAssembly)
{
	// Synthesize implicit return: named-return vars or default zero.
	if (method.returnType != awst::WType::voidType()
		&& !blockAlwaysTerminates(*method.body))
	{
		auto const& retParams = _func.returnParameters();
		bool hasNamedReturns = false;
		for (auto const& rp: retParams)
			if (!rp->name().empty())
				hasNamedReturns = true;

		auto retStmt = awst::makeReturnStatement(nullptr, method.sourceLocation);

		if (hasNamedReturns)
		{
			if (retParams.size() == 1)
			{
				// A named CALLDATA return whose pointer locals are live (an asm
				// block wrote x.offset/x.length — calldata_assign_from_nowhere)
				// reads through the pointer, not the (zero-init) local.
				if (m_functionCtx->seededCalldataPointers.count(
						retParams[0]->name()))
					retStmt->value = TypeCoercion::calldataPointerValueRead(
						retParams[0]->name(), method.sourceLocation);
				else if (retParams[0]->referenceLocation()
						== solidity::frontend::VariableDeclaration::Location::Memory
					&& m_functionCtx->isAssemblyAggregate(retParams[0]->id())
					&& !memoryUsesBlob(m_typeMapper.map(retParams[0]->type())))
				{
					// Any named memory aggregate exposed to Yul may have been
					// mutated or repointed. Reconstruct its native value from the
					// current offset for the implicit Solidity return, at any shape.
					std::vector<std::shared_ptr<awst::Statement>> reads;
					retStmt->value = materializeBlobValue(
						m_typeMapper, retParams[0]->type(),
						m_typeMapper.map(retParams[0]->type()),
						"__blobagg_off_" + std::to_string(retParams[0]->id()),
						method.sourceLocation, reads);
					for (auto& st: reads)
						method.body->body.push_back(std::move(st));
				}
				else if (m_typeMapper.profile().evmStorageLayout
					&& retParams[0]->referenceLocation()
						== solidity::frontend::VariableDeclaration::Location::Storage)
					retStmt->value = awst::makeVarExpression(
						retParams[0]->name(), awst::WType::biguintType(), method.sourceLocation);
				else
					retStmt->value = awst::makeVarExpression(
						retParams[0]->name(), m_typeMapper.map(retParams[0]->type()), method.sourceLocation);
			}
			else
			{
				auto tuple = awst::makeTupleExpression(nullptr, method.sourceLocation);
				for (auto const& rp: retParams)
				{
					auto* vt = (m_typeMapper.profile().evmStorageLayout
						&& rp->referenceLocation()
							== solidity::frontend::VariableDeclaration::Location::Storage)
						? awst::WType::biguintType()
						: m_typeMapper.map(rp->type());
					if (rp->referenceLocation()
							== solidity::frontend::VariableDeclaration::Location::Memory
						&& m_functionCtx->isAssemblyAggregate(rp->id())
						&& !memoryUsesBlob(vt))
					{
						std::vector<std::shared_ptr<awst::Statement>> reads;
						auto value = materializeBlobValue(
							m_typeMapper, rp->type(), vt,
							"__blobagg_off_" + std::to_string(rp->id()),
							method.sourceLocation, reads);
						for (auto& st: reads)
							method.body->body.push_back(std::move(st));
						tuple->items.push_back(std::move(value));
					}
					else
						tuple->items.push_back(awst::makeVarExpression(
							rp->name(), vt, method.sourceLocation));
				}
				tuple->wtype = method.returnType;
				retStmt->value = std::move(tuple);
			}
		}
		else
		{
			retStmt->value = StorageMapper::makeDefaultValue(method.returnType, method.sourceLocation);
		}

		// Build-time encoding: the synthesized implicit return is the SECOND return
		// construction site (SolReturnStatement is the first, for explicit returns);
		// encode it here too so it matches the wire method.returnType set below. The
		// value is a named var (scalar) or a literal tuple of named vars — never an
		// opaque call, so the spill vector stays empty.
		if (encodeReturnsAtBuildTime && retStmt->value)
		{
			std::vector<std::shared_ptr<awst::Statement>> prepend;
			retStmt->value = TypeCoercion::encodeReturnValue(
				m_typeMapper, std::move(retStmt->value), returnPlan,
				method.sourceLocation, prepend,
				/*asmWrap=*/funcHasInlineAssembly);
			for (auto& s: prepend)
				method.body->body.push_back(std::move(s));
		}

		// Enum range check on implicit named-return.
		if (hasNamedReturns && retParams.size() == 1)
		{
			if (auto const* enumType = dynamic_cast<solidity::frontend::EnumType const*>(retParams[0]->type()))
			{
				unsigned numMembers = enumType->numberOfMembers();
				auto var = awst::makeVarExpression(retParams[0]->name(), awst::WType::uint64Type(), method.sourceLocation);

				auto assertStmt = awst::makeExpressionStatement(
					awst::makeEnumRangeAssert(std::move(var), numMembers, method.sourceLocation),
					method.sourceLocation);
				method.body->body.push_back(std::move(assertStmt));
			}
		}

		method.body->body.push_back(std::move(retStmt));
	}

}

/// buildFunction phase: sub-64-bit / bool / enum param guards at the ABI entry.
void ContractBuilder::prependAbiEntryChecks(
	awst::ContractMethod& method,
	solidity::frontend::FunctionDefinition const& _func)
{
	// Sub-64-bit / bool / enum params: AVM uint64 doesn't auto-clean like EVM; guard explicitly.
	{
		bool useV2 = true; // default in 0.8+
		if (m_currentContract)
		{
			auto const& ann = m_currentContract->sourceUnit().annotation();
			if (ann.useABICoderV2.set())
				useV2 = *ann.useABICoderV2;
		}
		auto entryChecks = buildABIEntryChecks(
			_func, m_typeMapper, useV2, m_sourceFile);
		if (!entryChecks.empty())
		{
			method.body->body.insert(
				method.body->body.begin(),
				std::make_move_iterator(entryChecks.begin()),
				std::make_move_iterator(entryChecks.end())
			);
		}
	}

}

/// buildFunction phase: prepend the ensure_budget call when configured.
void ContractBuilder::prependEnsureBudget(
	awst::ContractMethod& method,
	solidity::frontend::FunctionDefinition const& _func)
{
	// ensure_budget: per-function map first, then global opup budget.
	uint64_t budgetForFunc = 0;
	if (auto it = m_ensureBudget.find(_func.name()); it != m_ensureBudget.end())
		budgetForFunc = it->second;
	else if (m_opupBudget > 0 && method.arc4MethodConfig.has_value())
		budgetForFunc = m_opupBudget;

	if (budgetForFunc > 0)
	{
		auto budgetVal = awst::makeIntegerConstant(budgetForFunc, method.sourceLocation);

		auto feeSource = awst::makeZero(method.sourceLocation);

		auto call = awst::makePuyaLibCall("ensure_budget",
			{
				awst::CallArg{std::string("required_budget"), budgetVal},
				awst::CallArg{std::string("fee_source"), feeSource}
			},
			awst::WType::voidType(), method.sourceLocation);

		auto stmt = awst::makeExpressionStatement(std::move(call), method.sourceLocation);

		method.body->body.insert(method.body->body.begin(), std::move(stmt));
	}

}

/// buildFunction phase: non-payable group assert (skipped for internal/ private and receive()).
void ContractBuilder::maybePrependNonPayable(
	awst::ContractMethod& method,
	solidity::frontend::FunctionDefinition const& _func)
{
	// Non-payable check: assert no preceding PaymentTxn has non-zero amount.
	// Skipped for internal/private and receive() (implicitly payable).
	if (!_func.isPayable() && !_func.isReceive())
	{
		// Selector lets the guard tell router dispatch from an internal
		// callsub — see prependNonPayableCheck.
		// Only pay for the selector-gated guard where it is needed: a
		// method reachable by internal callsub. Blanket use cost ~6
		// opcodes on every method and blew the 8 KB cap.
		std::string sel;
		if (m_currentContract
			&& m_typeMapper.analysis().isCalledInternally(
				m_currentContract->id(), _func.id()))
		{
			try { sel = eb::InnerCallHandlers::buildMethodSelector(*m_exprBuilder, &_func); }
			catch (...) { sel.clear(); }
		}
		prependNonPayableCheck(method, sel);
	}
}

awst::ContractMethod ContractBuilder::buildFunction(
	solidity::frontend::FunctionDefinition const& _func,
	std::string const& _contractName,
	std::string const& _nameOverride,
	bool _asInternalCopy
)
{
	awst::ContractMethod method;
	bool const funcHasInlineAssembly =
		m_typeMapper.analysis().callablesWithInlineAssembly.count(_func.id()) != 0;
	std::set<int64_t> asmSlotParamIds;
	for (auto const& param: _func.parameters())
		if (m_typeMapper.analysis().asmSlotReferenceDeclarations.count(param->id()))
			asmSlotParamIds.insert(param->id());

	buildMethodSignature(method, _func, _contractName, _nameOverride, asmSlotParamIds);

	computeMethodReturnType(method, _func, funcHasInlineAssembly);

	// Solidity `pure` must NOT map to puya `pure`. They are different contracts:
	// Solidity's means "reads/writes no state" — the function can still REVERT
	// (slice bounds, asserts, division by zero). puya's licenses call
	// ELIMINATION when the result is unused (ir/optimize/dead_code_elimination
	// removes such calls), i.e. it means "total, safe to delete". Marking a
	// Solidity-pure function puya-pure silently deleted its calls once it had
	// two callsites (one callsite gets inlined instead, which kept the body):
	// `f(x, s, e) external pure { x[s:e]; }` stopped bounds-checking entirely.
	// Mapping to false costs only minor backend optimizations
	// (repeated_loads_elimination, eliminate_box_asserts consult the flag);
	// recovering those needs a revert-freedom analysis, not a mutability bit.
	method.pure = false;

	// ARC4 method config for public/external functions. Suppressed for
	// internal copies (super/Base.f() impls): every ABI-entry behavior below
	// (entry checks, param remap, wire-return encoding, budget, not-payable
	// assert) gates on this config.
	if (!_asInternalCopy)
		method.arc4MethodConfig = buildARC4Config(_func, method.sourceLocation);

	// uros: chunk-assigned methods must not be inlined (an inlined copy defeats
	// the split; the uros backend needs to stub it in non-owning chunks).
	if (method.arc4MethodConfig.has_value())
		if (auto* abiCfg = std::get_if<awst::ARC4ABIMethodConfig>(&*method.arc4MethodConfig))
			if (!abiCfg->chunk.empty())
				method.inlineOpt = false;

	// ARC4 methods: remap param types to ARC4; stash decode ops for deferred
	// insertion (collectArc4ParamRemaps above).
	auto paramDecodes = collectArc4ParamRemaps(
		m_typeMapper, _func, method, funcHasInlineAssembly);

	if (_func.isImplemented())
	{
		setupBodyParamContext(method, _func);

		// ABI return encoding belongs at construction time. Plain methods encode
		// each source return immediately; modifier methods first normalize native
		// values for chain threading, then encode only the outer wrapper return.
		std::vector<ReturnWireElem> returnPlan =
			computeReturnPlan(_func, method.returnType, m_typeMapper);
		bool anyWork = false;
		for (auto const& p: returnPlan)
			if (p.encoded || p.masked) { anyWork = true; break; }
		bool const encodeReturnsAtBuildTime =
			method.arc4MethodConfig.has_value()
			&& _func.modifiers().empty()
			&& anyWork;
		if (encodeReturnsAtBuildTime)
			// Asm bodies are unchecked (Yul wraps mod 2^256): the encoder wraps
			// `value % 2^N` before encoding for these (Pass 2/3 encodeRet).
			setReturnWirePlan(returnPlan, /*asmWrap=*/funcHasInlineAssembly);

		registerBodyRefParams(_func, asmSlotParamIds);

		m_functionCtx->inConstructor = _func.isConstructor();
		m_functionCtx->callableId = _func.id();
		m_functionCtx->frameIsProgram =
			_func.visibility() == solidity::frontend::Visibility::Internal
			|| _func.visibility() == solidity::frontend::Visibility::Private
			// fallback/receive are ONLY router-dispatched (Solidity has no
			// `this.fallback()` form), so an asm `return(o,s)` inside them ends
			// the whole call. Without the halt the router's post-call void
			// carrier lands AFTER the fallback's own answer log and, as the
			// LAST log, clobbers it — callers decoded an EMPTY answer from the
			// tape-stub fallback (pm_negriskadapter balanceOf probes).
			|| _func.isFallback() || _func.isReceive();
		// UUPS recognized-idiom folds (proxy.md §3): the OZ UUPSUpgradeable
		// context checks pass and the in-contract upgrade path traps; the
		// real bodies would drag delegatecall + escaped-1967-slot storage
		// into the demand graph.
		if (auto fold = proxies::UupsLowering::classify(_func);
			fold != proxies::UupsFold::None)
			method.body = proxies::UupsLowering::foldedBody(
				fold, method.sourceLocation);
		else
			method.body = buildBlock(_func.body());
		m_functionCtx->inConstructor = false;
		m_functionCtx->callableId = 0;
		m_functionCtx->frameIsProgram = false;

		prependNamedReturnInits(method, _func);

		if (storageRefPointerReturn(&_func))
			rewriteStorageRefReturnIndices(method);

		synthesizeImplicitReturn(
			method, _func, encodeReturnsAtBuildTime, returnPlan, funcHasInlineAssembly);

		// Plain-method return sites are already encoded. Modifier chains must keep
		// native values until every before/after-placeholder action has run.
		if (encodeReturnsAtBuildTime)
			promoteReturnType(method, returnPlan, m_typeMapper);
		else
			normalizeNativeReturns(
				method, _func, m_typeMapper, returnPlan,
				funcHasInlineAssembly,
				method.arc4MethodConfig.has_value()
					&& !_func.modifiers().empty());

		applyParamDecodeNames(paramDecodes, method);

		prependAbiEntryChecks(method, _func);

		// Reference write-backs are part of the callable's real return shape.
		// Establish that shape before modifier-chain construction so every `_`
		// captures and forwards the updated parameter values.
		auto writeBackParams = augmentMethodForReferenceParams(
			method, _func, m_typeMapper, m_currentContract,
			_asInternalCopy, asmSlotParamIds);

		// Transient blob init is in the approval-program preamble (transient slot);
		// per-method init would reset it mid-dispatch, clobbering earlier writes.

		// Modifiers → a per-modifier SUBROUTINE CHAIN (mirrors solc's IR modifier lowering,
		// `IRGenerator::generateModifier`): `__mod{i}_N` + `__body_N` subs, each threading the
		// return params in/out and passing the still-ARC4-encoded `__arc4_*` params along. This
		// avoids textual expansion entirely: each `_` builds a fresh call to the
		// next member. Any member that uses an ABI parameter receives freshly
		// materialized native decode assignments from the recipes above.
		std::vector<std::shared_ptr<awst::Statement>> deferredDecodes;
		if (!_func.modifiers().empty())
		{
			buildModifierChain(
				_func, method, _contractName, paramDecodes,
				writeBackParams);
			if (method.arc4MethodConfig.has_value() && anyWork)
			{
				// Native normalization already handled signed extension, masking,
				// and assembly wrap. The dispatch boundary only needs ARC4 encoding.
				auto dispatchPlan = returnPlan;
				for (auto& item: dispatchPlan)
				{
					item.isSigned = false;
					item.masked = false;
				}
				transformReturnValues(
					method.body->body, m_typeMapper, dispatchPlan,
					/*asmWrap=*/false, /*wire=*/true);
				promoteReturnType(method, returnPlan, m_typeMapper);
			}
		}
		else
			deferredDecodes = makeParamDecodeStatements(paramDecodes);

		if (!deferredDecodes.empty())
			insertDeferredDecodes(
				method, _func, deferredDecodes, paramDecodes, m_tr->contractCtx);

		prependEnsureBudget(method, _func);

		maybePrependNonPayable(method, _func);
	}
	else
	{
		// Abstract — empty body.
		Logger::instance().debug("function '" + method.memberName + "' has no implementation", method.sourceLocation);
		method.body = awst::makeBlock(method.sourceLocation);
	}

	return method;
}


std::optional<awst::ARC4MethodConfig> ContractBuilder::buildARC4Config(
	solidity::frontend::FunctionDefinition const& _func,
	awst::SourceLocation const& _loc
)
{
	using namespace solidity::frontend;

	auto vis = _func.visibility();

	if (vis == Visibility::Private || vis == Visibility::Internal)
		return std::nullopt;

	awst::ARC4ABIMethodConfig config;
	config.sourceLocation = _loc;
	// Both fallback and receive have empty Solidity names; distinguish for routing.
	if (_func.isFallback())
		config.name = "__fallback";
	else if (_func.isReceive())
		config.name = "__receive";
	else
		config.name = _func.name();
	config.allowedCompletionTypes = {0}; // NoOp
	config.create = 3; // Disallow

	if (_func.stateMutability() == StateMutability::View ||
		_func.stateMutability() == StateMutability::Pure)
	{
		config.readonly = true;
	}

	// uros chunk: @custom:uros-chunk <name>
	if (_func.documentation())
		config.chunk = natSpecTagValue(*_func.documentation()->text(), "custom:uros-chunk");

	return awst::ARC4MethodConfig(config);
}


} // namespace puyasol::builder
