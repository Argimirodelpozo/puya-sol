/// @file SolVariableDeclaration.cpp

#include "builder/ast/stmts/SolVariableDeclaration.h"
#include "builder/ast/exprs/SolIndexAccess.h"
#include "builder/ast/exprs/SolTupleExpression.h"
#include "builder/solc/SolcFacts.h"
#include "builder/eb/CallOperands.h"
#include "Logger.h"
#include "builder/storage/slot/EvmSlotLowering.h"
#include "builder/target/EvmLayoutMode.h"
#include "builder/contract/AWSTBuilder.h" // containsMappingType
#include "builder/eb/MappingPrefix.h"
#include "builder/context/ContractContext.h"
#include "builder/storage/StorageMapper.h"
#include "builder/storage/StoragePlace.hpp"
#include "builder/types/TypeMapper.h"
#include "builder/codec/Arc4Defaults.h"
#include "builder/types/TypeCoercion.h"
#include "builder/types/ConversionPlan.h"
#include "builder/yul/AssemblyBuilder.h"
#include <libsolutil/Assertions.h>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;


SolVariableDeclaration::SolVariableDeclaration(
	BlockContext& _blk,
	VariableDeclarationStatement const& _node,
	awst::SourceLocation _loc)
	: SolStatement(_blk, std::move(_loc)), m_node(_node)
{
}

// ── toAwst binding rungs: each handles one declaration shape and returns
// true when it consumed the declaration (including error early-outs). ──────

bool SolVariableDeclaration::tryCalldataSlicePointerBinding(
	VariableDeclaration const& decl,
	Expression const* initialValue,
	std::vector<std::shared_ptr<awst::Statement>>& result)
{
	// CALLDATA slice binding through a LIVE pointer: `uint[2] calldata t = x[1]`
	// where x's mutable pointer locals exist (an asm block touched x.offset/
	// .length). Bind t's own pointer local `__cd_off_t = __cd_off_x + i*stride`
	// (solc's calldataStride = the element's calldata head size) and mark t
	// live, so a later asm `s := t` reads t's byte offset in __cd_blob —
	// 0x44 + 1*64 = 0x84 in calldata_array_read.
	if (decl.referenceLocation() != VariableDeclaration::Location::CallData || !initialValue)
		return false;
	auto const* idx = SolcFacts::expressionAs<IndexAccess>(initialValue);
	if (!idx || !idx->indexExpression()) return false;
	auto const* baseId = SolcFacts::expressionAs<Identifier>(&idx->baseExpression());
	auto const* baseVd = baseId ? dynamic_cast<VariableDeclaration const*>(
		baseId->annotation().referencedDeclaration) : nullptr;
	auto const* arrT = baseVd ? dynamic_cast<ArrayType const*>(baseVd->type()) : nullptr;
	auto* live = m_blk.fn.scope.liveCalldataPointers();
	if (!arrT || !live) return false;
	auto const baseName = m_blk.scope.awstVarName(*baseVd);
	if (!live->count(baseName)) return false;

	auto loc = m_blk.makeLoc(decl.location());
	auto* word = awst::WType::biguintType();
	auto idxVal = TypeCoercion::coerceScalar(
		CallOperands::evaluate(m_blk.builderCtx(), *idx->indexExpression(), loc), word, loc);
	std::shared_ptr<awst::Expression> length;
	if (arrT->isDynamicallySized()) length = awst::makeVarExpression("__cd_len_" + baseName, word, loc);
	else length = awst::makeIntegerConstant(arrT->length().str(), loc, word);
	m_blk.builderCtx().appendEffectsTo(result);
	result.push_back(awst::makeExpressionStatement(awst::makeAssert(
		awst::makeNumericCompare(idxVal, awst::NumericComparison::Lt, std::move(length), loc),
		loc, "array index out of bounds"), loc));
	auto scaled = awst::makeBigUIntBinOp(std::move(idxVal), awst::BigUIntBinaryOperator::Mult,
		awst::makeIntegerConstant(std::to_string(arrT->calldataStride()), loc, word), loc);
	auto off = awst::makeBigUIntBinOp(awst::makeVarExpression("__cd_off_" + baseName, word, loc),
		awst::BigUIntBinaryOperator::Add, std::move(scaled), loc);
	// Assembly and Solidity must use the same declaration-based local name.
	std::string tName = m_blk.scope.awstVarName(decl);
	result.push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression("__cd_off_" + tName, word, loc), std::move(off), loc));
	live->insert(tName);
	// The pointer is the binding; a materialized array would copy the value.
	return true;
}

bool SolVariableDeclaration::trySlotModeStoragePointer(
	VariableDeclaration const& decl,
	Expression const* initialValue,
	std::vector<std::shared_ptr<awst::Statement>>& result)
{
	// --evm-storage-layout: a storage-pointer local IS a biguint slot.
	// Resolve the initializer's slot on the AST (building the aggregate
	// value would be wrong/rejected) and bind `name = slot`.
	auto const& returns = m_blk.typeMapper().analysis().storageReferenceReturns;
	auto const found = returns.find(m_blk.fn.callableId);
	bool const tupleSlotBody = dynamic_cast<awst::WTuple const*>(m_blk.fn.returnType)
		&& found != returns.end() && found->second.slotHandle;
	if ((m_blk.typeMapper().profile().evmStorageLayout
			|| tupleSlotBody
			|| m_blk.typeMapper().analysis().slotHandleDeclarations.contains(decl.id()))
		&& decl.referenceLocation() == VariableDeclaration::Location::Storage)
	{
		auto loc = m_blk.makeLoc(decl.location());
		EvmSlotLowering low(m_blk.builderCtx(), m_blk.scope, loc);
		auto addr = initialValue ? low.resolve(*initialValue) : std::nullopt;
		if (initialValue && !addr)
			return true;   // error already logged
		auto target = awst::makeVarExpression(
			m_blk.scope.awstVarName(decl), awst::WType::biguintType(), loc);
		m_blk.scope.bindings.slotStorageRefs.set(decl.id(), target);
		// pre-statements (bounds asserts, key pins) BEFORE the binding
		for (auto& st: m_blk.builderCtx().takePreEffects())
			result.push_back(std::move(st));
		result.push_back(awst::makeAssignmentStatement(
			std::move(target),
			addr ? addr->slot : awst::makeIntegerConstant("0", loc, awst::WType::biguintType()), loc));
		for (auto& st: m_blk.builderCtx().takePostEffects())
			result.push_back(std::move(st));
		return true;
	}

	return false;
}

/// Lower once using the declared solc type, including for array literals.
std::shared_ptr<awst::Expression> SolVariableDeclaration::buildInitValue(
	VariableDeclaration const& decl,
	Expression const* initialValue,
	awst::WType const*& type)
{
	std::shared_ptr<awst::Expression> value;
	if (initialValue)
	{
		value = m_blk.builderCtx().pinIfWriteBacks(m_blk.builderCtx().lower(*initialValue, false), m_loc);

		value = convertInitValue(decl, std::move(value), initialValue->annotation().type, type);
	}
	else
		value = TypeCoercion::makeDefaultValue(type, m_loc);

	return value;
}

/// Both scalar and destructured initializers use solc's source/destination
/// types, with physical storage handles materialized only for value bindings.
std::shared_ptr<awst::Expression> SolVariableDeclaration::convertInitValue(
	VariableDeclaration const& decl, std::shared_ptr<awst::Expression> value,
	solidity::frontend::Type const* sourceType, awst::WType const* type)
{
	if (auto const* tuple = dynamic_cast<TupleType const*>(sourceType);
		tuple && tuple->components().size() == 1) sourceType = tuple->components().front();
	if (decl.referenceLocation() != VariableDeclaration::Location::Storage)
	{
		value = StorageMapper::makePartialBoxReadWithDefault(
			m_blk.typeMapper(), std::move(value), m_blk.builderCtx().preEffects(), m_loc);
		value = EvmSlotLowering::materializeRefValue(
			m_blk.builderCtx(), m_blk.scope, std::move(value), sourceType, type, m_loc);
	}
	return ConversionPlan{sourceType, decl.type(), type, ConversionPlan::Context::Initialization}.emit(
		std::move(value), m_loc, &m_blk.builderCtx().preEffects());
}

void SolVariableDeclaration::bindValue(
	VariableDeclaration const& decl, Expression const* initialValue,
	std::shared_ptr<awst::Expression> value, awst::WType const* type,
	std::vector<std::shared_ptr<awst::Statement>>& result)
{
	if (!value) return; // lowering already reported the error
	if (tryStorageAliasBinding(decl, value, initialValue, result)
		|| tryMemoryAliasBinding(decl, initialValue, type, result)
		|| tryBlobOffsetBinding(decl, initialValue, value, type, result)
		|| tryAsmAggregateInit(decl, initialValue, value, type, result)) return;
	emitDefaultDeclaration(decl,
		awst::makeVarExpression(m_blk.scope.awstVarName(decl), type, m_blk.makeLoc(decl.location())),
		std::move(value), type, initialValue, result);
}

bool SolVariableDeclaration::tryStorageAliasBinding(
	VariableDeclaration const& decl,
	std::shared_ptr<awst::Expression>& value,
	Expression const* initialValue,
	std::vector<std::shared_ptr<awst::Statement>>& result)
{
	// Storage pointer alias
	if (decl.referenceLocation() == VariableDeclaration::Location::Storage && initialValue)
	{
		// A mapping value is its logical holder, never its internal placeholder.
		// Bind it once, including member paths and runtime-selected holders.
		if (!m_blk.builderCtx().typeMapper.profile().evmStorageLayout && decl.type()
			&& decl.type()->category() == solidity::frontend::Type::Category::Mapping)
		{
			auto holder = resolveBuiltStorageHolder(m_blk.builderCtx(), value, m_loc);
			if (!holder.key) throw SizeError("mapping alias requires a resolved storage holder");
			auto const name = m_blk.scope.awstVarName(decl);
			m_blk.scope.bindings.mappingKeyParams.set(decl.id(), name);
			m_blk.builderCtx().appendEffectsTo(result);
			result.push_back(awst::makeAssignmentStatement(
				awst::makeVarExpression(name, awst::WType::bytesType(), m_loc),
				awst::makeAsBytes(std::move(holder.key), m_loc), m_loc));
			return true;
		}
		if (!m_blk.builderCtx().typeMapper.profile().evmStorageLayout
			&& containsMappingType(decl.type())
			&& (dynamic_cast<awst::StateGet const*>(value.get()) || awst::isRawStorageRead(value.get())
				|| dynamic_cast<awst::FieldExpression const*>(value.get())
				|| dynamic_cast<awst::IndexExpression const*>(value.get())
				|| dynamic_cast<awst::ReinterpretCast const*>(value.get())))
		{
			auto holder = resolveBuiltStorageHolder(m_blk.builderCtx(), value, m_loc);
			if (!holder.key) throw SizeError("aggregate alias requires a resolved storage holder");
			value = std::move(holder.value);
		}

		if (StoragePlace::fromRead(value))
		{
			// Raw box/app-state reads need StateGet-with-default so the alias
			// evaluates identically to a direct read; StateGet passes through.
			auto aliasExpr = awst::isRawStorageRead(value.get())
				? StorageMapper::makeStateGetWithDefault(value, value->wtype, m_loc)
				: value;
			m_blk.scope.bindings.storageAliases.set(decl.id(), StorageAlias::stateRead(std::move(aliasExpr)));
			m_blk.builderCtx().appendEffectsTo(result);
			return true;
		}

		// `T storage b = a[i];` / `T storage b = a.field;` — alias the
		// IndexExpression/FieldExpression so push/pop/indexed-write route
		// through the underlying state container's read-modify-write codegen.
		if (dynamic_cast<awst::IndexExpression const*>(value.get()))
		{
			m_blk.scope.bindings.storageAliases.set(decl.id(), StorageAlias::indexedPath(value));
			m_blk.builderCtx().appendEffectsTo(result);
			return true;
		}
		if (dynamic_cast<awst::FieldExpression const*>(value.get()))
		{
			m_blk.scope.bindings.storageAliases.set(decl.id(), StorageAlias::fieldPath(value));
			m_blk.builderCtx().appendEffectsTo(result);
			return true;
		}
		// Ternary init `T storage p = c ? a1 : a2;` — no single compile-time
		// root. Bind a runtime-selected STORAGE KEY instead: pin
		// `c ? key(a1) : key(a2)` into a bytes local AT DECL TIME (mutating
		// c's inputs later must not re-select), and alias p to a state read
		// keyed by that local. Length/index/push/field/element-write all hit
		// the SELECTED underlying root — mutations write through instead of
		// into a materialized copy (formerly a documented known-gap).
		// Families: box roots (dynamic arrays; bytes/string, whose branches
		// are the raw box key under a cast), app-global roots (structs,
		// fixed arrays). Mappings use the holder binding above.
		// Mixed/unrecognized branch shapes (nested
		// ternaries, mixed kinds) keep the value-copy fallback.
		if (auto const* condE = dynamic_cast<awst::ConditionalExpression const*>(value.get()))
		{
			auto truePlace = StoragePlace::fromRead(condE->trueExpr);
			auto falsePlace = StoragePlace::fromRead(condE->falseExpr);
			if (truePlace && falsePlace && truePlace->hasSameShape(*falsePlace))
			{
				std::string keyName = decl.name() + "__selkey" + std::to_string(decl.id());
				auto keySel = awst::makeConditional(condE->condition,
					awst::makeReinterpretCast(
						truePlace->key, awst::WType::bytesType(), m_loc),
					awst::makeReinterpretCast(
						falsePlace->key, awst::WType::bytesType(), m_loc),
					awst::WType::bytesType(), m_loc);
				m_blk.builderCtx().appendEffectsTo(result);
				result.push_back(awst::makeAssignmentStatement(
					awst::makeVarExpression(keyName, awst::WType::bytesType(), m_loc),
					std::move(keySel), m_loc));
				auto keyRead = [&]() {
					return awst::makeVarExpression(
						keyName, awst::WType::bytesType(), m_loc);
				};
				auto aliasExpr = truePlace->makeField(keyRead(), m_loc);
				if (truePlace->kind == StoragePlaceKind::Box)
					aliasExpr = StorageMapper::makeStateGetWithDefault(
						std::move(aliasExpr), truePlace->valueType, m_loc);
				m_blk.scope.bindings.storageAliases.set(decl.id(),
					StorageAlias::stateRead(std::move(aliasExpr)));
				return true;
			}
		}

		// Storage ref from a function call (typically `.slot :=` in assembly).
		// Two patterns: (1) bytes return → mappingKeyParam (SolIndexAccess uses
		// it as box-key prefix, e.g. `Pool.State storage pool = _getPool(id)`);
		// (2) biguint return → slotStorageRef for __storage_read/write.
		if (dynamic_cast<awst::SubroutineCallExpression const*>(value.get())
			|| ((dynamic_cast<awst::TupleItemExpression const*>(value.get())
					|| (SolcFacts::expressionAs<FunctionCall>(initialValue)
						&& dynamic_cast<awst::VarExpression const*>(value.get())))
				&& (value->wtype == awst::WType::biguintType()
					|| value->wtype == awst::WType::uint64Type()
					|| value->wtype == awst::WType::bytesType())))
		{
			bool isMappingPtr = decl.type()
				&& (decl.type()->category() == solidity::frontend::Type::Category::Mapping
					|| builder::containsMappingType(decl.type())
					// Struct getter returning bytes box-key (e.g. `Position.State storage p =
					// self.positions.get(k)`, no nested mappings): bind as mappingKeyParam
					// so `p.field`/`p.method()` resolve against the runtime prefix.
					|| (decl.type()->category() == solidity::frontend::Type::Category::Struct
						&& value->wtype == awst::WType::bytesType()));
			if (isMappingPtr && value->wtype == awst::WType::bytesType())
			{
				auto const name = m_blk.scope.awstVarName(decl);
				m_blk.scope.bindings.mappingKeyParams.set(decl.id(), name);
				// Plain bytes assignment so `m` holds the holder key at runtime;
				// `m = otherMapping` updates which mapping `m` points to.
				auto var = awst::makeVarExpression(name, awst::WType::bytesType(), m_loc);
				auto assign = awst::makeAssignmentStatement(std::move(var), std::move(value), m_loc);
				for (auto& effect: m_blk.builderCtx().takePreEffects())
					result.push_back(std::move(effect));
				result.push_back(std::move(assign));
				for (auto& effect: m_blk.builderCtx().takePostEffects())
					result.push_back(std::move(effect));
				return true;
			}

			// Emit the call as an assignment; slot var wtype must match the return wtype.
			auto* slotWType = value->wtype ? value->wtype : awst::WType::biguintType();
			auto slotVar = awst::makeVarExpression(m_blk.scope.awstVarName(decl), slotWType, m_loc);
			m_blk.scope.bindings.slotStorageRefs.set(decl.id(), slotVar);

			auto assign = awst::makeAssignmentStatement(std::move(slotVar), std::move(value), m_loc);
			for (auto& effect: m_blk.builderCtx().takePreEffects())
				result.push_back(std::move(effect));
			result.push_back(std::move(assign));
			for (auto& effect: m_blk.builderCtx().takePostEffects())
				result.push_back(std::move(effect));
			return true;
		}
	}

	return false;
}

bool SolVariableDeclaration::tryMemoryAliasBinding(
	VariableDeclaration const& decl,
	Expression const* initialValue,
	awst::WType const* type,
	std::vector<std::shared_ptr<awst::Statement>>& result)
{
	// Memory-aggregate alias (handle-model copy-elision): `T memory b = a` where `a` is
	// another memory-aggregate variable → register b→a so b's references resolve to a's
	// local; memory→memory ALIASES (matches EVM) instead of copying. Only a plain
	// memory-aggregate identifier source, and only the small (non-blob) case — >4KB
	// aggregates alias via the blob offset below. Whole-program reassignment facts
	// disqualify either name before translation, including writes in later branches.
	if (initialValue
		&& decl.referenceLocation() == VariableDeclaration::Location::Memory
		&& decl.type() && !builder::memoryUsesBlob(type))
	{
		bool aliasable = decl.type()->category() == solidity::frontend::Type::Category::Struct;
		bool declBytesLike = false;
		if (auto const* at = dynamic_cast<solidity::frontend::ArrayType const*>(decl.type()))
		{
			aliasable = !at->isByteArrayOrString();
			declBytesLike = at->isByteArrayOrString();
		}
		// `bytes memory rb = bytes(strVar)` / `string memory s = string(bVar)`:
		// on EVM the cast is a zero-cost reinterpret of the SAME pointer, so
		// element writes through the new name must hit the original. Peel the
		// type conversion and alias — a copy silently dropped every write
		// (the no-asm Base64 encoder wrote its output into a detached copy).
		solidity::frontend::Expression const* aliasSrc = initialValue;
		bool viaByteCast = false;
		if (auto const* fc = SolcFacts::expressionAs<solidity::frontend::FunctionCall>(initialValue);
			fc && fc->annotation().kind.set()
			&& *fc->annotation().kind
				== solidity::frontend::FunctionCallKind::TypeConversion
			&& fc->arguments().size() == 1 && declBytesLike)
		{
			auto const* innerT = dynamic_cast<solidity::frontend::ArrayType const*>(
				fc->arguments()[0]->annotation().type);
			if (innerT && innerT->isByteArrayOrString()
				&& innerT->dataStoredIn(solidity::frontend::DataLocation::Memory))
			{
				aliasSrc = fc->arguments()[0].get();
				viaByteCast = true;
			}
		}
		auto const* srcId = SolcFacts::expressionAs<solidity::frontend::Identifier>(aliasSrc);
		auto const* srcVd = srcId
			? dynamic_cast<VariableDeclaration const*>(srcId->annotation().referencedDeclaration)
			: nullptr;
		if ((aliasable || viaByteCast) && srcVd
			&& srcVd->referenceLocation() == VariableDeclaration::Location::Memory
			&& m_blk.typeMapper().analysis().reassignedMemoryLocals.count(decl.id()) == 0
			&& m_blk.typeMapper().analysis().reassignedMemoryLocals.count(srcVd->id()) == 0)
		{
			// Keep reference identity, not a destructuring temporary's value.
			// The source is a stable identifier (proven by solc declaration IDs
			// and the reassignment facts above), so resolving it has no effects.
			auto source = m_blk.builderCtx().buildExpr(*srcId);
			if (!source) return false;
			if (source->wtype != type)
				source = awst::makeReinterpretCast(std::move(source), type, m_loc);
			m_blk.scope.bindings.memoryAliases.set(decl.id(), std::move(source));
			m_blk.builderCtx().appendEffectsTo(result);
			return true;
		}
	}

	return false;
}

bool SolVariableDeclaration::tryBlobOffsetBinding(
	VariableDeclaration const& decl,
	Expression const* initialValue,
	std::shared_ptr<awst::Expression>& value,
	awst::WType const* type,
	std::vector<std::shared_ptr<awst::Statement>>& result)
{
	// `T memory p = blobAggFn(...)`: callee returns the uint64 base offset into
	// the shared blob; register as blob aggregate (no copy/FMP bump).
	// >4KB memory values can only originate this way (AVM can't copy them).
	if (initialValue
		&& decl.referenceLocation() == solidity::frontend::VariableDeclaration::Location::Memory
		&& builder::memoryUsesBlob(type))
	{
		std::string offN = "__blobagg_off_" + std::to_string(decl.id());
		m_blk.builderCtx().appendEffectsTo(result);
		result.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(offN, awst::WType::uint64Type(), m_loc),
			builder::TypeCoercion::coerceScalar(
				std::move(value), awst::WType::uint64Type(), m_loc),
			m_loc));
		m_blk.scope.bindings.blobAggregates.set(decl.id(), offN);
		return true;
	}

	return false;
}

bool SolVariableDeclaration::tryAsmBytesAllocation(
	VariableDeclaration const& decl, Expression const* initialValue,
	std::vector<std::shared_ptr<awst::Statement>>& result)
{
	if (!initialValue || decl.referenceLocation() != VariableDeclaration::Location::Memory
		|| !m_blk.scope.bindings.assemblyAggregates.contains(decl.id())) return false;
	auto const* array = dynamic_cast<ArrayType const*>(decl.type());
	auto const* call = SolcFacts::expressionAs<FunctionCall>(initialValue);
	if (!array || !array->isByteArrayOrString() || !call
		|| !SolcFacts::expressionAs<NewExpression>(&SolcFacts::functionExpression(call->expression()))) return false;

	// Decide the representation before lowering new bytes/string(n): only the
	// length is needed for blob allocation. Lower it once, including write-backs.
	auto& bc = m_blk.builderCtx();
	auto length = TypeCoercion::coerceScalar(
		bc.pinIfWriteBacks(bc.lower(*call->arguments().front(), false), m_loc),
		awst::WType::uint64Type(), m_loc);
	bc.appendEffectsTo(result);
	std::string offset = "__blobagg_off_" + std::to_string(decl.id());
	for (auto& statement: AssemblyBuilder::emitBytesBlobAlloc(
		m_blk.typeMapper(), std::move(length), offset, static_cast<int>(decl.id()), m_loc))
		result.push_back(std::move(statement));
	m_blk.scope.bindings.blobAggregates.set(decl.id(), offset);
	return true;
}

bool SolVariableDeclaration::tryAsmAggregateInit(
	VariableDeclaration const& decl, Expression const* initialValue,
	std::shared_ptr<awst::Expression>& value, awst::WType const* type,
	std::vector<std::shared_ptr<awst::Statement>>& result)
{
	if (!initialValue || decl.referenceLocation() != VariableDeclaration::Location::Memory
		|| !m_blk.scope.bindings.assemblyAggregates.contains(decl.id())) return false;
	// Calls, casts and new non-bytes arrays all use the recursive EVM-memory
	// writer. Its input was already lowered once; prerequisites precede the spill.
	m_blk.builderCtx().appendEffectsTo(result);
	std::string offset = "__blobagg_off_" + std::to_string(decl.id());
	if (emitBlobBackValue(m_blk.typeMapper(), decl.type(), type, std::move(value),
		offset, static_cast<int>(decl.id()), m_blk.makeLoc(decl.location()), result))
		m_blk.scope.bindings.blobAggregates.set(decl.id(), offset);
	return true;
}

/// Default binding: `target = value`, with the fresh-memory FMP bump for uninitialised `T memory t;` (blob-backed >4KB locals bind …
void SolVariableDeclaration::emitDefaultDeclaration(
	VariableDeclaration const& decl,
	std::shared_ptr<awst::Expression> target,
	std::shared_ptr<awst::Expression> value,
	awst::WType const* type,
	Expression const* initialValue,
	std::vector<std::shared_ptr<awst::Statement>>& result)
{
	auto assign = awst::makeAssignmentStatement(std::move(target), std::move(value), m_loc);

	m_blk.builderCtx().appendEffectsTo(result);

	// `T memory t;` (no initializer): allocate fresh memory and bump mload(0x40)
	// so contracts reading mload(0x40) see the expected advance. Initialised
	// memory locals are pointer copies in EVM, so no bump there.
	if (!initialValue
		&& decl.referenceLocation() == VariableDeclaration::Location::Memory)
	{
		int sz = builder::computeEncodedElementSize(type).fixedBytes<int>().value_or(0);

		// >4096 B: can't hold as a single AVM bytes value. Back with the
		// multi-slot blob; bind local to FMP base offset so `t.field[i]`
		// lowers to blob word ops (SolIndexAccess). Blob is pre-zeroed.
		if (builder::memoryUsesBlob(type)
			|| m_blk.scope.bindings.assemblyAggregates.contains(decl.id()))
		{
			std::string offN = "__blobagg_off_" + std::to_string(decl.id());
			// base = current FMP (uint64) = extractUInt64(load(slot0), 88)
			auto blob = awst::makeLoadSlot(
				m_blk.typeMapper().profile().scratchLayout.memoryFirst(), m_loc);
			auto base = awst::makeExtractUInt64(
				std::move(blob), awst::makeIntegerConstant("88", m_loc), m_loc);
			result.push_back(awst::makeAssignmentStatement(
				awst::makeVarExpression(offN, awst::WType::uint64Type(), m_loc),
				std::move(base), m_loc));
			for (auto& s: builder::AssemblyBuilder::emitFreeMemoryBump(
					m_blk.typeMapper().profile().scratchLayout, sz, m_loc,
					static_cast<int>(decl.id())))
				result.push_back(std::move(s));
			m_blk.scope.bindings.blobAggregates.set(decl.id(), offN);
			return; // skip the normal (oversized) target = bzero(sz) assignment
		}

		if (sz > 0)
			for (auto& s: builder::AssemblyBuilder::emitFreeMemoryBump(
					m_blk.typeMapper().profile().scratchLayout, sz, m_loc,
					static_cast<int>(decl.id())))
				result.push_back(std::move(s));
	}

	result.push_back(assign);
}

/// Tuple destructuring `(a, b) = expr;`: RHS must evaluate once — SingleEvaluation is inlined per-consumer in AWST JSON, causing …
void SolVariableDeclaration::buildTupleDestructuring(
	Expression const* initialValue,
	std::vector<std::shared_ptr<awst::Statement>>& result)
{
	auto const& declarations = m_node.declarations();
	std::vector<VariableDeclaration const*> bindings;
	for (auto const& declaration: declarations) bindings.push_back(declaration.get());
	auto& ctx = m_blk.builderCtx();
	auto rhsExpr = ctx.pinIfWriteBacks(ctx.lowerOperand([&] {
		return SolTupleExpression::buildBindingRhs(ctx, *initialValue, bindings);
	}, false), m_loc);
	if (!rhsExpr) return;
	m_blk.builderCtx().appendEffectsTo(result);

	auto const* tupleType = rhsExpr->wtype;
	std::string tempName = "__tuple_destruct_" + std::to_string(m_node.id());
	auto tempTarget = awst::makeVarExpression(tempName, tupleType, m_loc);
	auto tempAssign = awst::makeAssignmentStatement(
		std::move(tempTarget), std::move(rhsExpr), m_loc);
	result.push_back(std::move(tempAssign));

	// Per-element SOURCE Solidity types (RHS tuple / multi-return call),
	// for signed sub-word widening below.
	auto const* rhsSolTuple = dynamic_cast<solidity::frontend::TupleType const*>(
		initialValue->annotation().type);
	auto const* wtupleType = dynamic_cast<awst::WTuple const*>(tupleType);
	solAssert(rhsSolTuple && rhsSolTuple->components().size() == declarations.size()
		&& wtupleType && wtupleType->types().size() == declarations.size(),
		"tuple initializer shape changed during lowering");

	for (size_t i = 0; i < declarations.size(); ++i)
	{
		if (!declarations[i]) continue;
		auto const& decl = *declarations[i];
		auto* type = m_blk.typeMapper().map(decl.type());
		// --evm-storage-layout: a STORAGE-located destructured var is a
		// biguint slot handle (the RHS component already is one) — typing
		// it by the mapped aggregate mislabeled the var (an ARC4Struct
		// wtype puya then failed to even deserialize) and broke every
		// `(, S storage y, ) = g()` read.
		bool slotHandle = (m_blk.typeMapper().profile().evmStorageLayout
				|| wtupleType->types()[i] == awst::WType::biguintType())
			&& decl.referenceLocation()
				== solidity::frontend::VariableDeclaration::Location::Storage;
		if (slotHandle)
		{
			type = awst::WType::biguintType();
			m_blk.scope.bindings.slotStorageRefs.set(decl.id(), awst::makeVarExpression(
				m_blk.scope.awstVarName(decl), type,
				m_blk.makeLoc(decl.location())));
		}

		// Extract with the slot's ACTUAL wtype (the RHS element type), then
		// coerce to the declared type. Extracting with the declared type
		// mislabels the slot and skipped all coercion — `(int128 a,) =
		// (int8Val,)` bound the raw uint64-backed 0xFF as +255 instead of
		// sign-extending to -1.
		auto const* slotType = wtupleType->types()[i];
		auto baseRef = awst::makeVarExpression(tempName, tupleType, m_loc);
		std::shared_ptr<awst::Expression> itemExpr = awst::makeTupleItem(
			std::move(baseRef), static_cast<int>(i), slotType, m_loc);

		auto const* sourceType = rhsSolTuple->components().at(i);
		if (decl.referenceLocation() == VariableDeclaration::Location::Memory
			&& !decl.type()->isValueType() && slotType == awst::WType::uint64Type())
		{
			std::string name = "__blobagg_off_" + std::to_string(decl.id());
			result.push_back(awst::makeAssignmentStatement(
				awst::makeVarExpression(name, slotType, m_loc), std::move(itemExpr), m_loc));
			m_blk.scope.bindings.blobAggregates.set(decl.id(), name);
			continue;
		}
		itemExpr = convertInitValue(decl, std::move(itemExpr), sourceType, type);
		// Literal tuple components retain useful alias provenance. Opaque calls
		// still pass a non-null initializer, without rebuilding the call.
		auto const* source = initialValue;
		if (auto const* tuple = SolcFacts::expressionAs<TupleExpression>(initialValue);
			tuple && !tuple->isInlineArray()) source = tuple->components().at(i).get();
		bindValue(decl, source, std::move(itemExpr), type, result);
	}
}

std::vector<std::shared_ptr<awst::Statement>> SolVariableDeclaration::toAwst()
{
	std::vector<std::shared_ptr<awst::Statement>> result;
	auto const& declarations = m_node.declarations();
	auto const* initialValue = m_node.initialValue();
	if (initialValue) initialValue = &SolcFacts::unparenthesized(*initialValue);

	if (declarations.size() == 1 && declarations[0])
	{
		auto const& decl = *declarations[0];
		auto* type = m_blk.typeMapper().map(decl.type());

		if (tryCalldataSlicePointerBinding(decl, initialValue, result))
			return result;

		if (trySlotModeStoragePointer(decl, initialValue, result))
			return result;

		if (tryAsmBytesAllocation(decl, initialValue, result))
			return result;
		if (initialValue && decl.referenceLocation() == VariableDeclaration::Location::Memory)
			if (auto reference = SolIndexAccess::resolveBlobReference(
				m_blk.builderCtx(), m_blk.scope, *initialValue, m_loc))
			{
				auto& ctx = m_blk.builderCtx();
				auto offset = ctx.emitSequencedOperand(std::move(reference->effects),
					std::move(reference->value), true, m_loc);
				ctx.appendEffectsTo(result);
				std::string name = "__blobagg_off_" + std::to_string(decl.id());
				result.push_back(awst::makeAssignmentStatement(
					awst::makeVarExpression(name, awst::WType::uint64Type(), m_loc), std::move(offset), m_loc));
				m_blk.scope.bindings.blobAggregates.set(decl.id(), name);
				return result;
			}
		auto value = buildInitValue(decl, initialValue, type);
		bindValue(decl, initialValue, std::move(value), type, result);
	}
	else if (declarations.size() > 1 && initialValue)
		buildTupleDestructuring(initialValue, result);

	return result;
}

} // namespace puyasol::builder::sol_ast
