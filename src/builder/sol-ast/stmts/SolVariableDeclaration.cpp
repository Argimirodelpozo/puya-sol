/// @file SolVariableDeclaration.cpp
/// Migrated from VariableDeclarationBuilder.cpp.

#include "builder/sol-ast/stmts/SolVariableDeclaration.h"
#include "Logger.h"
#include "builder/sol-ast/EvmSlotLowering.h"
#include "builder/storage/EvmLayoutMode.h"
#include "builder/AWSTBuilder.h" // containsMappingType
#include "builder/sol-eb/ContractContext.h"
#include "builder/storage/StorageMapper.h"
#include "builder/storage/StoragePlace.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/Arc4Defaults.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/ConversionPlan.h"
#include "builder/assembly/AssemblyBuilder.h"

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

std::vector<std::shared_ptr<awst::Statement>> SolVariableDeclaration::toAwst()
{
	std::vector<std::shared_ptr<awst::Statement>> result;
	auto const& declarations = m_node.declarations();
	auto const* initialValue = m_node.initialValue();

	if (declarations.size() == 1 && declarations[0])
	{
		auto const& decl = *declarations[0];
		auto* type = m_blk.typeMapper().map(decl.type());

		auto target = awst::makeVarExpression(m_blk.awstVarName(decl), type, m_blk.makeLoc(decl.location()));

		// CALLDATA slice binding through a LIVE pointer: `uint[2] calldata t = x[1]`
		// where x's mutable pointer locals exist (an asm block touched x.offset/
		// .length). Bind t's own pointer local `__cd_off_t = __cd_off_x + i*stride`
		// (solc's calldataStride = the element's calldata head size) and mark t
		// live, so a later asm `s := t` reads t's byte offset in __cd_blob —
		// 0x44 + 1*64 = 0x84 in calldata_array_read. The regular value binding
		// below still runs (non-asm uses of t read the decoded value).
		if (decl.referenceLocation() == VariableDeclaration::Location::CallData && initialValue)
			if (auto const* idx = dynamic_cast<IndexAccess const*>(initialValue))
				if (auto const* baseId = dynamic_cast<Identifier const*>(&idx->baseExpression()))
					if (auto const* baseVd = dynamic_cast<VariableDeclaration const*>(
							baseId->annotation().referencedDeclaration))
						if (auto* live = m_blk.fn.liveCalldataPointers();
							live && live->count(baseVd->name()) && idx->indexExpression())
							if (auto const* arrT = dynamic_cast<solidity::frontend::ArrayType const*>(
									baseVd->type()))
							{
								auto loc = m_blk.makeLoc(decl.location());
								auto idxVal = builder::TypeCoercion::implicitNumericCast(
									m_blk.builderCtx().buildExpr(*idx->indexExpression()),
									awst::WType::biguintType(), loc);
								auto scaled = awst::makeBigUIntBinOp(std::move(idxVal),
									awst::BigUIntBinaryOperator::Mult,
									awst::makeIntegerConstant(
										std::to_string(arrT->calldataStride()), loc,
										awst::WType::biguintType()), loc);
								auto off = awst::makeBigUIntBinOp(
									awst::makeVarExpression("__cd_off_" + baseVd->name(),
										awst::WType::biguintType(), loc),
									awst::BigUIntBinaryOperator::Add, std::move(scaled), loc);
								// Name via awstVarName: assembly resolves the bare local
								// through externalRefAwstName (= awstVarName mangling), so
								// the __cd_off_ local + live-set entry must match it.
								std::string tName = m_blk.awstVarName(decl);
								result.push_back(awst::makeAssignmentStatement(
									awst::makeVarExpression("__cd_off_" + tName,
										awst::WType::biguintType(), loc),
									std::move(off), loc));
								live->insert(tName);
								// The POINTER is the binding for a calldata slice: skip the
								// value copy (a ReferenceArray local from an arc4 element is
								// a type mismatch puya rejects, and EVM semantics are the
								// pointer anyway). A value use of the slice would hit an
								// undefined local — loud, not silently wrong.
								m_blk.builderCtx().appendEffectsTo(result);
								return result;
							}

		// --evm-storage-layout: a storage-pointer local IS a biguint slot.
		// Resolve the initializer's slot on the AST (building the aggregate
		// value would be wrong/rejected) and bind `name = slot`.
		if (m_blk.typeMapper().profile().evmStorageLayout
			&& decl.referenceLocation() == VariableDeclaration::Location::Storage
			&& initialValue)
		{
			auto loc = m_blk.makeLoc(decl.location());
			EvmSlotLowering low(m_blk.builderCtx(), m_blk, loc);
			auto addr = low.resolve(*initialValue);
			if (!addr)
				return result;   // error already logged
			m_blk.setSlotStorageRef(decl.id(), awst::makeVarExpression(
				decl.name(), awst::WType::biguintType(), loc));
			// pre-statements (bounds asserts, key pins) BEFORE the binding
			for (auto& st: m_blk.builderCtx().takePreEffects())
				result.push_back(std::move(st));
			result.push_back(awst::makeAssignmentStatement(
				awst::makeVarExpression(decl.name(), awst::WType::biguintType(), loc),
				addr->slot, loc));
			for (auto& st: m_blk.builderCtx().takePostEffects())
				result.push_back(std::move(st));
			return result;
		}

		std::shared_ptr<awst::Expression> value;
		if (initialValue)
		{
			// Track function pointer assignments via ASTNode::referencedDeclaration
			// (handles Identifier + MemberAccess; super.f safe via findSuperTarget
			// in SolInternalCall::processFromIdent). EXCEPT a FOREIGN contract's
			// external fn (`Other(addr).g`): the static shortcut direct-callsubs
			// the target, which cannot cross apps — those must stay dynamic
			// (12-byte appId++selector, inner-txn path). `this.f` keeps the
			// shortcut (self-calls ARE direct subroutine calls by design).
			if (auto const* declFt = dynamic_cast<FunctionType const*>(decl.type()))
			{
				bool foreignExternal = false;
				if (declFt->kind() == FunctionType::Kind::External)
					if (auto const* ma = dynamic_cast<MemberAccess const*>(initialValue))
					{
						auto const* baseId = dynamic_cast<Identifier const*>(&ma->expression());
						if (!(baseId && baseId->name() == "this"))
							foreignExternal = true;
					}
				if (!foreignExternal)
					if (auto const* funcDef = dynamic_cast<FunctionDefinition const*>(
							ASTNode::referencedDeclaration(*initialValue)))
						m_blk.setFuncPtrTarget(decl.id(), funcDef);
			}

			value = m_blk.builderCtx().buildExpr(*initialValue);

			// --evm-storage-layout: a storage-typed initializer builds to its
			// biguint slot handle; a MEMORY struct local needs the VALUE —
			// materialise it from the slots (storage → memory copy).
			if (m_blk.typeMapper().profile().evmStorageLayout && value
				&& value->wtype == awst::WType::biguintType()
				&& decl.referenceLocation() != VariableDeclaration::Location::Storage)
				if (auto const* ist = dynamic_cast<solidity::frontend::StructType const*>(
						initialValue->annotation().type);
					ist && ist->dataStoredIn(solidity::frontend::DataLocation::Storage))
				{
					auto loc = m_blk.makeLoc(decl.location());
					EvmSlotLowering low(m_blk.builderCtx(), m_blk, loc);
					EvmSlotLowering::Addr a;
					a.slot = std::move(value);
					a.solType = ist;
					a.wtype = m_blk.typeMapper().map(ist);
					value = low.readStructValue(a);
					if (!value)
						return result;
				}

			// Upgrade dynamic array to fixed-size when N is known
			if (auto* newArr = dynamic_cast<awst::NewArray*>(value.get()))
			{
				if (!newArr->values.empty())
				{
					if (type && type->kind() == awst::WTypeKind::ReferenceArray)
					{
						auto const* refArr = dynamic_cast<awst::ReferenceArray const*>(type);
						if (refArr && !refArr->arraySize())
						{
							int n = static_cast<int>(newArr->values.size());
							type = m_blk.typeMapper().createType<awst::ReferenceArray>(
								refArr->elementType(), true, n);
							newArr->wtype = type;
							target->wtype = type;
						}
					}
					// Note: don't upgrade ARC4DynamicArray→ARC4StaticArray here.
					// Subsequent references to the variable use TypeMapper which
					// returns ARC4DynamicArray, causing type mismatches.
				}
			}

			if (initialValue && !dynamic_cast<TupleType const*>(
					initialValue->annotation().type))
				value = builder::ConversionPlan{
					initialValue->annotation().type, decl.type(), type,
					builder::ConversionPlan::Context::Initialization}.emit(
						std::move(value), m_loc);
			else
				value = builder::TypeCoercion::coerceForAssignment(
					std::move(value), type, m_loc);
		}
		else
			value = StorageMapper::makeDefaultValue(type, m_loc);

		// Storage pointer alias
		if (decl.referenceLocation() == VariableDeclaration::Location::Storage && initialValue)
		{
			// `mapping storage m = m1;`: BytesConstant is the holder name;
			// register alias so SolIndexAccess resolves `m[k]` to the state-var prefix.
			if (dynamic_cast<awst::BytesConstant const*>(value.get())
				&& decl.type()
				&& decl.type()->category() == solidity::frontend::Type::Category::Mapping)
			{
				m_blk.setStorageAlias(decl.id(), StorageAlias::mappingHolder(value));
				m_blk.builderCtx().appendEffectsTo(result);
				return result;
			}

			if (dynamic_cast<awst::StateGet const*>(value.get())
				|| awst::isRawStorageRead(value.get()))
			{
				// Raw box/app-state reads need StateGet-with-default so the alias
				// evaluates identically to a direct read; StateGet passes through.
				auto aliasExpr = awst::isRawStorageRead(value.get())
					? StorageMapper::makeStateGetWithDefault(value, value->wtype, m_loc)
					: value;
				m_blk.setStorageAlias(decl.id(), StorageAlias::stateRead(std::move(aliasExpr)));
				m_blk.builderCtx().appendEffectsTo(result);
				return result;
			}

			// `T storage b = a[i];` / `T storage b = a.field;` — alias the
			// IndexExpression/FieldExpression so push/pop/indexed-write route
			// through the underlying state container's read-modify-write codegen.
			if (dynamic_cast<awst::IndexExpression const*>(value.get()))
			{
				m_blk.setStorageAlias(decl.id(), StorageAlias::indexedPath(value));
				m_blk.builderCtx().appendEffectsTo(result);
				return result;
			}
			if (dynamic_cast<awst::FieldExpression const*>(value.get()))
			{
				m_blk.setStorageAlias(decl.id(), StorageAlias::fieldPath(value));
				m_blk.builderCtx().appendEffectsTo(result);
				return result;
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
			// fixed arrays), and mappings (runtime holder name →
			// mappingKeyParam). Mixed/unrecognized branch shapes (nested
			// ternaries, mixed kinds) keep the value-copy fallback.
			if (auto const* condE = dynamic_cast<awst::ConditionalExpression const*>(value.get()))
			{
				// `mapping storage m = c ? m1 : m2`: branches are holder-NAME
				// constants — bind the runtime holder via mappingKeyParam (the
				// same route `m = someMappingFn()` takes).
				if (decl.type()
					&& decl.type()->category() == solidity::frontend::Type::Category::Mapping
					&& dynamic_cast<awst::BytesConstant const*>(condE->trueExpr.get())
					&& dynamic_cast<awst::BytesConstant const*>(condE->falseExpr.get()))
				{
					m_blk.setMappingKeyParam(decl.id(), decl.name());
					auto var = awst::makeVarExpression(
						decl.name(), awst::WType::bytesType(), m_loc);
					result.push_back(awst::makeAssignmentStatement(
						std::move(var), value, m_loc));
					m_blk.builderCtx().appendEffectsTo(result);
					return result;
				}

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
					m_blk.setStorageAlias(decl.id(),
						StorageAlias::stateRead(std::move(aliasExpr)));
					m_blk.builderCtx().appendEffectsTo(result);
					return result;
				}
			}

			// Storage ref from a function call (typically `.slot :=` in assembly).
			// Two patterns: (1) bytes return → mappingKeyParam (SolIndexAccess uses
			// it as box-key prefix, e.g. `Pool.State storage pool = _getPool(id)`);
			// (2) biguint return → slotStorageRef for __storage_read/write.
			if (dynamic_cast<awst::SubroutineCallExpression const*>(value.get()))
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
					m_blk.setMappingKeyParam(decl.id(), decl.name());
					// Plain bytes assignment so `m` holds the holder name at runtime;
					// `m = otherMapping` updates which mapping `m` points to.
					auto var = awst::makeVarExpression(decl.name(), awst::WType::bytesType(), m_loc);
					auto assign = awst::makeAssignmentStatement(std::move(var), std::move(value), m_loc);
					result.push_back(std::move(assign));

					m_blk.builderCtx().appendEffectsTo(result);
					return result;
				}

				m_blk.setSlotStorageRef(decl.id(), value);
				// Emit the call as an assignment; slot var wtype must match the return wtype.
				auto* slotWType = value->wtype ? value->wtype : awst::WType::biguintType();
				auto slotVar = awst::makeVarExpression(decl.name(), slotWType, m_loc);

				auto assign = awst::makeAssignmentStatement(std::move(slotVar), std::move(value), m_loc);
				result.push_back(std::move(assign));

				m_blk.builderCtx().appendEffectsTo(result);
				return result;
			}
		}

		// Memory-aggregate alias (handle-model copy-elision): `T memory b = a` where `a` is
		// another memory-aggregate variable → register b→a so b's references resolve to a's
		// local; memory→memory ALIASES (matches EVM) instead of copying. Only a plain
		// memory-aggregate identifier source, and only the small (non-blob) case — >4KB
		// aggregates alias via the blob offset below. (Reassignment of a/b after the alias
		// would make this unsafe; not yet guarded — relying on zero-reg to surface it.)
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
			if (auto const* fc = dynamic_cast<solidity::frontend::FunctionCall const*>(initialValue);
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
			auto const* srcId = dynamic_cast<solidity::frontend::Identifier const*>(aliasSrc);
			auto const* srcVd = srcId
				? dynamic_cast<VariableDeclaration const*>(srcId->annotation().referencedDeclaration)
				: nullptr;
			if ((aliasable || viaByteCast) && srcVd
				&& srcVd->referenceLocation() == VariableDeclaration::Location::Memory
				&& m_blk.typeMapper().analysis().reassignedMemoryLocals.count(decl.id()) == 0
				&& m_blk.typeMapper().analysis().reassignedMemoryLocals.count(srcVd->id()) == 0)
			{
				if (viaByteCast)
				{
					// re-resolve the PEELED source (value was built from the
					// cast); relabel to the declared wtype so uses type-check.
					auto srcRead = m_blk.builderCtx().buildExpr(*aliasSrc);
					if (srcRead)
					{
						if (srcRead->wtype != type)
							srcRead = awst::makeReinterpretCast(
								std::move(srcRead), type, m_loc);
						m_blk.setMemoryAlias(decl.id(), std::move(srcRead));
						m_blk.builderCtx().appendEffectsTo(result);
						return result;
					}
				}
				else
				{
					m_blk.setMemoryAlias(decl.id(), value); // value = buildExpr(a) = a's (resolved) local read
					m_blk.builderCtx().appendEffectsTo(result);
					return result;
				}
			}
		}

		// `T memory p = blobAggFn(...)`: callee returns the uint64 base offset into
		// the shared blob; register as blob aggregate (no copy/FMP bump).
		// >4KB memory values can only originate this way (AVM can't copy them).
		if (initialValue
			&& decl.referenceLocation() == solidity::frontend::VariableDeclaration::Location::Memory
			&& builder::memoryUsesBlob(type))
		{
			std::string offN = "__blobagg_off_" + std::to_string(decl.id());
			result.push_back(awst::makeAssignmentStatement(
				awst::makeVarExpression(offN, awst::WType::uint64Type(), m_loc),
				builder::TypeCoercion::implicitNumericCast(
					std::move(value), awst::WType::uint64Type(), m_loc),
				m_loc));
			m_blk.setBlobAggregate(decl.id(), offN);
			m_blk.builderCtx().appendEffectsTo(result);
			return result;
		}

		// Memory aggregate used in inline assembly: give it an ARC4-layout blob
		// pointer (not EVM-faithful — ARC4 packs differently; add(m,32) skipping a
		// length word will diverge). `new T[](n)` binds to a fresh FMP region and
		// bumps FMP; non-`new` falls through to the ensureBiguint backstop.
		if (initialValue
			&& decl.referenceLocation() == VariableDeclaration::Location::Memory
			&& m_blk.isAssemblyAggregate(decl.id()))
		{
			FunctionCall const* newCall = nullptr;
			if (auto const* fc = dynamic_cast<FunctionCall const*>(initialValue))
				if (dynamic_cast<NewExpression const*>(&fc->expression()))
					newCall = fc;
			// stage 3 (--evm-memory-layout): ANY initializer — build the VALUE,
			// allocate its EVM-layout region in the blob, store it, bind the
			// offset (shared emitBlobBackValue; also used for param spills).
			if (!newCall && m_blk.typeMapper().profile().evmMemoryLayout)
			{
				auto loc2 = m_blk.makeLoc(decl.location());
				auto v0 = m_blk.builderCtx().buildExpr(*initialValue);
				if (!v0)
					return result;
				m_blk.builderCtx().appendEffectsTo(result);
				std::string offN = "__blobagg_off_" + std::to_string(decl.id());
				if (builder::emitBlobBackValue(m_blk.typeMapper(), decl.type(),
						type, std::move(v0), offN,
						static_cast<int>(decl.id()), loc2, result))
					m_blk.setBlobAggregate(decl.id(), offN);
				return result;
			}
			if (newCall)
			{
				using AB = builder::AssemblyBuilder;
				std::string offN = "__blobagg_off_" + std::to_string(decl.id());
				// Dynamic bytes/string (`new bytes(n)` / `new string(n)`): the OZ
				// Strings.toString buffer idiom. Blob-alloc with a runtime length
				// word so `add(buf,32)` points at the data and a value-read
				// materialises [len][data]. Real arrays: fixed FMP bump, no len word.
				auto const* at = dynamic_cast<ArrayType const*>(decl.type());
				if (at && at->isByteArrayOrString() && !newCall->arguments().empty())
				{
					auto lenU64 = builder::TypeCoercion::implicitNumericCast(
						m_blk.builderCtx().buildExpr(*newCall->arguments()[0]),
						awst::WType::uint64Type(), m_loc);
					for (auto& s: AB::emitBytesBlobAlloc(
							m_blk.typeMapper().profile().scratchLayout,
							std::move(lenU64), offN, static_cast<int>(decl.id()), m_loc))
						result.push_back(std::move(s));
					m_blk.setBlobAggregate(decl.id(), offN);
					m_blk.builderCtx().appendEffectsTo(result);
					return result;
				}
				result.push_back(awst::makeAssignmentStatement(
					awst::makeVarExpression(offN, awst::WType::uint64Type(), m_loc),
					awst::makeExtractUInt64(awst::makeLoadSlot(
						m_blk.typeMapper().profile().scratchLayout.memoryFirst(), m_loc),
						awst::makeIntegerConstant("88", m_loc), m_loc),
					m_loc));
				int sz = builder::computeEncodedElementSize(type);
				if (sz > 0)
					for (auto& s: AB::emitFreeMemoryBump(
							m_blk.typeMapper().profile().scratchLayout, sz, m_loc,
							static_cast<int>(decl.id())))
						result.push_back(std::move(s));
				m_blk.setBlobAggregate(decl.id(), offN);
				m_blk.builderCtx().appendEffectsTo(result);
				return result;
			}
		}

		auto assign = awst::makeAssignmentStatement(std::move(target), std::move(value), m_loc);

		m_blk.builderCtx().appendEffectsTo(result);

		// `T memory t;` (no initializer): allocate fresh memory and bump mload(0x40)
		// so contracts reading mload(0x40) see the expected advance. Initialised
		// memory locals are pointer copies in EVM, so no bump there.
		if (!initialValue
			&& decl.referenceLocation() == VariableDeclaration::Location::Memory)
		{
			int sz = builder::computeEncodedElementSize(type);

			// >4096 B: can't hold as a single AVM bytes value. Back with the
			// multi-slot blob; bind local to FMP base offset so `t.field[i]`
			// lowers to blob word ops (SolIndexAccess). Blob is pre-zeroed.
			if (builder::memoryUsesBlob(type)
				|| m_blk.isAssemblyAggregate(decl.id()))
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
				m_blk.setBlobAggregate(decl.id(), offN);
				return result; // skip the normal (oversized) target = bzero(sz) assignment
			}

			if (sz > 0)
				for (auto& s: builder::AssemblyBuilder::emitFreeMemoryBump(
						m_blk.typeMapper().profile().scratchLayout, sz, m_loc,
						static_cast<int>(decl.id())))
					result.push_back(std::move(s));
		}

		result.push_back(assign);
	}
	else if (declarations.size() > 1 && initialValue)
	{
		// Tuple destructuring `(a, b) = expr;`: RHS must evaluate once.
		// SingleEvaluation is inlined per-consumer in AWST JSON, causing puya
		// to re-emit the call for each TupleItemExpression. Assign RHS to a
		// synthetic temp and extract items from it. (polymarket-experiment 271d85851)
		auto rhsExpr = m_blk.builderCtx().buildExpr(*initialValue);
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
			bool slotHandle = m_blk.typeMapper().profile().evmStorageLayout
				&& decl.referenceLocation()
					== solidity::frontend::VariableDeclaration::Location::Storage;
			if (slotHandle)
			{
				type = awst::WType::biguintType();
				m_blk.setSlotStorageRef(decl.id(), awst::makeVarExpression(
					decl.name(), awst::WType::biguintType(),
					m_blk.makeLoc(decl.location())));
			}

			// Shadow-safe name: `uint a=100; { (uint a,)=f(); } return a;`
			// without mangling, inner `a` overwrites the outer one. Slot
			// handles use the PLAIN name — that is what isSlotHandleLocal
			// reads resolve to (same convention as the single-decl binding).
			auto target = awst::makeVarExpression(
				slotHandle ? decl.name() : m_blk.awstVarName(decl), type,
				m_blk.makeLoc(decl.location()));

			// Extract with the slot's ACTUAL wtype (the RHS element type), then
			// coerce to the declared type. Extracting with the declared type
			// mislabels the slot and skipped all coercion — `(int128 a,) =
			// (int8Val,)` bound the raw uint64-backed 0xFF as +255 instead of
			// sign-extending to -1.
			auto const* slotType = (wtupleType && i < wtupleType->types().size())
				? wtupleType->types()[i] : type;
			auto baseRef = awst::makeVarExpression(tempName, tupleType, m_loc);
			std::shared_ptr<awst::Expression> itemExpr = awst::makeTupleItem(
				std::move(baseRef), static_cast<int>(i), slotType, m_loc);

			itemExpr = builder::TypeCoercion::coerceForAssignment(std::move(itemExpr), type, m_loc);
			if (rhsSolTuple && i < rhsSolTuple->components().size())
				itemExpr = builder::TypeCoercion::signExtendSignedWiden(
					std::move(itemExpr), rhsSolTuple->components()[i], decl.type(), m_loc);

			auto assign = awst::makeAssignmentStatement(std::move(target), std::move(itemExpr), m_loc);
			result.push_back(assign);
		}
	}

	return result;
}

} // namespace puyasol::builder::sol_ast
