#include "builder/contract/ContractBuilder.h"
#include "awst/NameGen.h"
#include "builder/storage/EvmLayoutMode.h"
#include "builder/sol-ast/EvmSlotLowering.h"
#include "Logger.h"
#include "builder/AWSTBuilder.h"
#include "builder/contract/StateVarWalker.h"
#include "builder/contract/ParamABIValidator.h"
#include "builder/contract/ReturnRewriter.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/SolIntType.h"

#include <libsolidity/ast/ASTVisitor.h>

namespace puyasol::builder
{

namespace
{

solidity::frontend::Type const* unwrapUDVT(solidity::frontend::Type const* t)
{
	if (auto const* udvt = dynamic_cast<solidity::frontend::UserDefinedValueType const*>(t))
		return &udvt->underlyingType();
	return t;
}

// WType for a getter return/tuple element: a signed sub-256 integer becomes a 256-bit
// two's-complement biguint (matching FunctionBuilder's signed-return lowering, so the
// ABI element is uint256-on-wire and reads back signed) — in lockstep with the
// signExtendToUint256 projectStructFields applies to the VALUE. Everything else maps natively.
awst::WType const* getterElementWType(TypeMapper& tm, solidity::frontend::Type const* solType)
{
	if (auto it = builder::SolIntType::fromSol(solType); it && it->isSigned && it->bits < 256)
		return awst::WType::biguintType();
	return tm.map(solType);
}

// Project a Solidity struct value into its public-accessor field list:
// skip mapping members and non-bytes array members (matches solc's getter),
// reading each remaining field off `base` and ARC4-decoding it to its native
// type when the stored ARC4 field type differs. Returns the projected items.
// Shared by the simple-var, array-element, and mapping-value getter paths;
// callers either move the items into a tuple or use them directly.
std::vector<std::shared_ptr<awst::Expression>> projectStructFields(
	TypeMapper& typeMapper,
	solidity::frontend::StructType const* solStruct,
	awst::ARC4Struct const* arc4Struct,
	std::shared_ptr<awst::Expression> const& base,
	awst::SourceLocation const& loc)
{
	std::vector<std::shared_ptr<awst::Expression>> items;
	for (auto const& member: solStruct->members(nullptr))
	{
		if (member.type->category() == solidity::frontend::Type::Category::Mapping)
			continue;
		if (auto const* at = dynamic_cast<solidity::frontend::ArrayType const*>(member.type))
			if (!at->isByteArrayOrString())
				continue;

		awst::WType const* arc4FieldType = nullptr;
		if (arc4Struct)
			for (auto const& [fname, ftype]: arc4Struct->fields())
				if (fname == member.name)
				{
					arc4FieldType = ftype;
					break;
				}

		auto fieldExpr = awst::makeFieldExpression(
			base, member.name,
			arc4FieldType ? arc4FieldType : typeMapper.map(member.type), loc);

		auto* nativeType = typeMapper.map(member.type);
		if (arc4FieldType && arc4FieldType != nativeType)
		{
			std::shared_ptr<awst::Expression> decode =
				awst::makeARC4Decode(std::move(fieldExpr), nativeType, loc);
			// Signed fields → canonical 256-bit two's-complement biguint, matching how
			// FunctionBuilder lowers a signed tuple RETURN (mappedType=biguint +
			// signExtendToUint256, so the ABI element is uint256-on-wire and the client
			// int{N} patch reads it signed). A raw ARC4Decode is the unsigned N-bit value
			// (int128 INT128_MIN → +2^127); a 64-bit-only extension would still leave a
			// sub-64 field (int16) uint64-shaped in the ABI tuple. No-op unsigned / int256.
			// The tuple element WType is set to biguint in lockstep (getterElementWType).
			if (auto const* fieldInt = dynamic_cast<solidity::frontend::IntegerType const*>(
					unwrapUDVT(member.type)))
				if (fieldInt->isSigned() && fieldInt->numBits() < 256)
					decode = TypeCoercion::signExtendToUint256(
						std::move(decode), fieldInt->numBits(), loc);
			items.push_back(std::move(decode));
		}
		else
			items.push_back(std::move(fieldExpr));
	}
	return items;
}

// biguint <-> ARC4UIntN(N) codec for getter ABI remapping. Shared by the
// param-decode (isEncode=false: ARC4UIntN -> biguint) and return-encode
// (isEncode=true: biguint -> ARC4UIntN) blocks.
std::shared_ptr<awst::Expression> arc4UintCodec(
	std::shared_ptr<awst::Expression> value,
	awst::WType const* arc4Type,
	bool isEncode,
	awst::SourceLocation loc)
{
	if (isEncode)
		return awst::makeARC4Encode(std::move(value), arc4Type, std::move(loc));
	return awst::makeARC4Decode(std::move(value), awst::WType::biguintType(), std::move(loc));
}

} // namespace

void ContractBuilder::buildPublicStateVariableGetters(
	solidity::frontend::ContractDefinition const& _contract,
	awst::Contract& _contractNode,
	std::string const& _contractName,
	std::set<std::string>& _translatedFunctions)
{
	auto* contract = &_contractNode;
	auto const& contractName = _contractName;
	auto& translatedFunctions = _translatedFunctions;
	forEachStateVar(_contract, [&](auto const* var)
	{
		{
			if (!var->isPartOfExternalInterface())
				return;
			if (translatedFunctions.count(var->name()))
				return; // explicit getter already exists

			// Getter type: param types (mapping keys, array indices) + return types (struct filtering).
			auto getterFuncType = var->functionType(/*_internal=*/false);
			if (!getterFuncType)
				return;

			translatedFunctions.insert(var->name());

			auto loc = makeLoc(var->location());

			awst::ContractMethod getter;
			getter.sourceLocation = loc;
			getter.cref = m_sourceFile + "." + contractName;
			getter.memberName = var->name();
			getter.pure = var->isConstant();

			awst::ARC4ABIMethodConfig config;
			config.name = var->name();
			config.sourceLocation = loc;
			config.allowedCompletionTypes = {0}; // NoOp
			config.create = 3; // Disallow
			config.readonly = true;
			getter.arc4MethodConfig = config;

			auto const& solParamTypes = getterFuncType->parameterTypes();
			auto const solParamNames = getterFuncType->parameterNames();
			for (size_t i = 0; i < solParamTypes.size(); ++i)
			{
				awst::SubroutineArgument arg;
				std::string paramName = (i < solParamNames.size() && !solParamNames[i].empty())
					? solParamNames[i]
					: "key" + std::to_string(i);
				arg.name = paramName;
				arg.sourceLocation = loc;
				arg.wtype = m_typeMapper.map(solParamTypes[i]);
				getter.args.push_back(std::move(arg));
			}

			auto const& solReturnTypes = getterFuncType->returnParameterTypes();
			auto const& solReturnNames = getterFuncType->returnParameterNames();
			unsigned signedGetterBits = 0; // >0 for signed ≤64-bit returns
			if (solReturnTypes.size() == 1)
			{
				getter.returnType = m_typeMapper.map(solReturnTypes[0]);
				if (auto intInfo = builder::SolIntType::fromSol(solReturnTypes[0]))
				{
					// ANY signed sub-256 return must sign-extend to canonical 256-bit
					// TC for the ABI. ≤64-bit is uint64-backed (override to biguint);
					// 64<bits<256 already maps to biguint, but an ARRAY-ELEMENT / UDVT
					// getter reads the element at its NATURAL width (int72 -1 = 2^72-1),
					// which is NOT canonical — the old `<= 64` gate skipped sign-extension
					// for those, so `int72[] public a; a(i)` returned 2^72-1 for -1.
					// signExtendToUint256 is idempotent, so widening is safe for the
					// already-canonical scalar case too. Found by the corpus-mutation
					// fuzzer (userDefinedValueType/memory_to_storage uint16->int72).
					if (intInfo->isSigned && intInfo->bits < 256)
					{
						getter.returnType = awst::WType::biguintType();
						signedGetterBits = intInfo->bits;
					}
				}
			}
			else if (solReturnTypes.size() > 1)
			{
				std::vector<awst::WType const*> tupleTypes;
				std::vector<std::string> tupleNames;
				for (size_t i = 0; i < solReturnTypes.size(); ++i)
				{
					// Signed sub-256 elements → biguint (256-bit), matching the value
					// projectStructFields produces and an explicit signed tuple return.
					tupleTypes.push_back(getterElementWType(m_typeMapper, solReturnTypes[i]));
					tupleNames.push_back(i < solReturnNames.size() ? solReturnNames[i] : "");
				}
				getter.returnType = m_typeMapper.createType<awst::WTuple>(
					std::move(tupleTypes), std::move(tupleNames)
				);
			}
			else
			{
				return; // no return types — shouldn't happen for getters
			}

			auto body = awst::makeBlock(loc);

			std::shared_ptr<awst::Expression> readExpr;
			// Transient vars are NOT in the storage layout (slot space is
			// persistent storage only) — they keep the TRANSIENT_SLOT blob
			// getter below, same as default mode.
			if (!var->isConstant() && !var->immutable()
				&& var->referenceLocation()
					!= solidity::frontend::VariableDeclaration::Location::Transient
				&& m_typeMapper.profile().evmStorageLayout)
			{
				// --evm-storage-layout: walk the declared type over the getter
				// args (mapping keys / array indices) to the leaf's slot address
				// and read it there — the slot-mode twin of the branches below.
				sol_ast::EvmSlotLowering low(
					*m_exprBuilder, *m_exprBuilder->currentScope, loc);
				auto addr = low.addrForStateVar(*var);
				bool supported = addr.has_value();
				solidity::frontend::Type const* walk = var->type();
				size_t ai = 0;
				while (supported && ai < getter.args.size())
				{
					if (auto const* mt =
							dynamic_cast<solidity::frontend::MappingType const*>(walk))
					{
						auto keyVar = awst::makeVarExpression(
							getter.args[ai].name, getter.args[ai].wtype, loc);
						auto slot = low.mappingEntrySlot(
							addr->slot, std::move(keyVar), mt->keyType());
						walk = mt->valueType();
						sol_ast::EvmSlotLowering::Addr a;
						a.slot = std::move(slot);
						a.size = walk->storageBytes();
						a.solType = walk;
						a.wtype = m_typeMapper.map(walk);
						if (a.wtype == awst::WType::accountType())
							a.size = 32;   // full-slot AVM address
						addr = std::move(a);
						ai++;
						continue;
					}
					if (auto const* at =
							dynamic_cast<solidity::frontend::ArrayType const*>(walk);
						at && !at->isByteArrayOrString())
					{
						auto idxRef = awst::makeVarExpression(
							getter.args[ai].name, getter.args[ai].wtype, loc);
						auto idx = TypeCoercion::implicitNumericCast(
							std::move(idxRef), awst::WType::biguintType(), loc);
						std::shared_ptr<awst::Expression> dataBase;
						std::shared_ptr<awst::Expression> lenExpr;
						if (at->isDynamicallySized())
						{
							lenExpr = sol_ast::EvmSlotLowering::readSlotWord(addr->slot, loc);
							dataBase = sol_ast::EvmSlotLowering::dynDataBase(addr->slot, loc);
						}
						else
						{
							lenExpr = awst::makeIntegerConstant(
								at->length().str(), loc, awst::WType::biguintType());
							dataBase = addr->slot;
						}
						auto idxRef2 = awst::makeVarExpression(
							getter.args[ai].name, getter.args[ai].wtype, loc);
						auto idxCheck = TypeCoercion::implicitNumericCast(
							std::move(idxRef2), awst::WType::biguintType(), loc);
						auto cmp = awst::makeNumericCompare(std::move(idxCheck),
							awst::NumericComparison::Lt, std::move(lenExpr), loc);
						body->body.push_back(awst::makeExpressionStatement(
							awst::makeAssert(std::move(cmp), loc, "array out-of-bounds"),
							loc));
						addr = low.elemAddr(std::move(dataBase), std::move(idx),
							at->baseType());
						walk = at->baseType();
						ai++;
						continue;
					}
					supported = false;
				}
				if (supported)
				{
					if (walk->isValueType())
						readExpr = low.readValue(*addr);
					else if (sol_ast::EvmSlotLowering::isBytesLike(walk))
						readExpr = low.readBytesValue(*addr);
					else if (auto const* st =
							dynamic_cast<solidity::frontend::StructType const*>(walk))
					{
						// project fields flat, skipping mapping/array members
						// (solc's public-accessor rule); string/bytes stay.
						std::vector<std::shared_ptr<awst::Expression>> items;
						for (auto const& m: st->structDefinition().members())
						{
							if (!m)
								continue;
							auto const* mtOfM = m->type();
							if (dynamic_cast<solidity::frontend::StructType const*>(mtOfM))
							{
								// nested-struct member: solc's getter returns a
								// nested tuple — not modelled yet; skip loudly.
								supported = false;
								break;
							}
							if (dynamic_cast<solidity::frontend::MappingType const*>(mtOfM))
								continue;
							if (auto const* ma2 = dynamic_cast<
									solidity::frontend::ArrayType const*>(mtOfM);
								ma2 && !ma2->isByteArrayOrString())
								continue;
							auto const& off = st->storageOffsetsOfMember(m->name());
							sol_ast::EvmSlotLowering::Addr fa;
							fa.slot = off.first == 0 ? addr->slot
								: awst::makeBigUIntBinOp(addr->slot,
									awst::BigUIntBinaryOperator::Add,
									awst::makeIntegerConstant(off.first.str(), loc,
										awst::WType::biguintType()), loc);
							fa.byteOffset = off.second
								? awst::makeIntegerConstant(
									static_cast<uint64_t>(off.second), loc)
								: nullptr;
							fa.size = mtOfM->storageBytes();
							fa.solType = mtOfM;
							fa.wtype = m_typeMapper.map(mtOfM);
							// Full-32 account widening matches the write side
							// ONLY when the field is ALONE in its slot; a
							// PACKED account is stored as its trailing 20
							// bytes, and a widened read swallows the
							// neighbours (returned rescale's low byte inside
							// the address on the Comet/CoW RewardConfig
							// shape). Same aloneness test as
							// EvmSlotLowering::resolve.
							bool aloneInSlot = true;
							for (auto const& m2: st->structDefinition().members())
							{
								if (!m2 || m2->name() == m->name())
									continue;
								auto const& mo2 =
									st->storageOffsetsOfMember(m2->name());
								if (mo2.first == off.first)
								{
									aloneInSlot = false;
									break;
								}
							}
							if (fa.wtype == awst::WType::accountType()
								&& !fa.byteOffset && aloneInSlot)
								fa.size = 32;   // matches the write side's widening
							std::shared_ptr<awst::Expression> item;
							if (sol_ast::EvmSlotLowering::isBytesLike(mtOfM))
								item = low.readBytesValue(fa);
							else
								item = low.readValue(fa);
							if (auto it2 = builder::SolIntType::fromSol(mtOfM);
								item && it2 && it2->isSigned && it2->bits < 256)
								item = TypeCoercion::signExtendToUint256(
									TypeCoercion::implicitNumericCast(std::move(item),
										awst::WType::biguintType(), loc),
									it2->bits, loc);
							items.push_back(std::move(item));
						}
						if (items.size() == 1)
							readExpr = std::move(items[0]);
						else if (!items.empty())
						{
							auto tuple = awst::makeTupleExpression(getter.returnType, loc);
							for (auto& it3: items)
								tuple->items.push_back(std::move(it3));
							readExpr = std::move(tuple);
						}
					}
				}
				// flush anything the lowering queued (index pins etc.)
				for (auto& st2: m_exprBuilder->takePreEffects())
					body->body.push_back(std::move(st2));
				for (auto& st2: m_exprBuilder->takePostEffects())
					body->body.push_back(std::move(st2));
				if (!readExpr)
				{
					Logger::instance().warning(
						"--evm-storage-layout: skipping auto-getter for public "
						"state variable '" + var->name()
						+ "' (shape not yet supported; write an explicit getter)",
						loc);
					return;
				}
			}
			else if (var->isConstant())
			{
				// Compile-time constant: return directly.
				if (var->value())
					readExpr = m_exprBuilder->buildExpr(*var->value());
				if (!readExpr)
					readExpr = StorageMapper::makeDefaultValue(getter.returnType, loc);
				if (readExpr && readExpr->wtype != getter.returnType)
					readExpr = TypeCoercion::implicitNumericCast(
						std::move(readExpr), getter.returnType, loc
					);
				// String literal → bytes[N]: right-pad.
				if (readExpr && readExpr->wtype != getter.returnType)
				{
					auto const* bytesType = dynamic_cast<awst::BytesWType const*>(getter.returnType);
					if (bytesType && bytesType->length().has_value() && *bytesType->length() > 0)
					{
						if (auto padded = TypeCoercion::stringToBytesN(
								readExpr.get(), getter.returnType, *bytesType->length(), loc))
							readExpr = std::move(padded);
					}
					else
					{
						// Generic ReinterpretCast for bytes-compatible coercions.
						bool compat = readExpr->wtype == awst::WType::stringType()
							|| (readExpr->wtype && readExpr->wtype->kind() == awst::WTypeKind::Bytes);
						if (compat)
						{
							auto cast = awst::makeReinterpretCast(std::move(readExpr), getter.returnType, loc);
							readExpr = std::move(cast);
						}
					}
				}
			}
			else if (getter.args.empty())
			{
				// Simple state variable (no keys/indices): read from storage.
				auto storageKind = m_storageMapper.shouldUseBoxStorage(*var)
					? awst::AppStorageKind::Box
					: awst::AppStorageKind::AppGlobal;

				// Struct getter: read the full ARC4Struct, project each field (sign-extending
				// signed sub-word fields). Covers single-field structs too — they were read
				// as a bare scalar and skipped per-field sign-extension.
				auto const* solStructType = dynamic_cast<solidity::frontend::StructType const*>(var->type());
				if (solStructType && solReturnTypes.size() >= 1)
				{
					auto* storedWType = m_typeMapper.map(var->type());
					auto fullStruct = m_storageMapper.createStateRead(
						var->name(), storedWType, storageKind, loc
					);

					auto const* arc4Struct = dynamic_cast<awst::ARC4Struct const*>(storedWType);
					auto items = projectStructFields(m_typeMapper, solStructType, arc4Struct, fullStruct, loc);

					// One returnable field keeps the scalar return type; >1 packs a tuple.
					// Either way each field is sign-extended inside projectStructFields.
					if (items.size() == 1)
						readExpr = std::move(items[0]);
					else
					{
						auto tuple = awst::makeTupleExpression(getter.returnType, loc);
						for (auto& item: items)
							tuple->items.push_back(std::move(item));
						readExpr = std::move(tuple);
					}
				}
				else
				{
					// Use original storage type (not promoted return type).
					auto* readType = signedGetterBits > 0
						? m_typeMapper.map(var->type()) : getter.returnType;

					// Transient vars: route through TRANSIENT_SLOT blob (same as named-var reads).
					if (var->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Transient
						&& m_transientStorage.isTransient(*var))
					{
						readExpr = m_transientStorage.buildRead(var->name(), readType, loc);
					}
					if (!readExpr)
						readExpr = m_storageMapper.createStateRead(
							var->name(), readType, storageKind, loc
						);
				}
			}
			else if (dynamic_cast<solidity::frontend::ArrayType const*>(var->type())
				&& !dynamic_cast<solidity::frontend::ArrayType const*>(var->type())->isByteArrayOrString()
				&& getter.args.size() == 1)
			{
				// Array getter(i): IndexExpression into the packed ARC4 array slot (not sha256 key).
				auto const* arrType = dynamic_cast<solidity::frontend::ArrayType const*>(var->type());
				auto* arrWType = m_typeMapper.map(arrType);
				auto* elemARC4 = m_typeMapper.mapSolTypeToARC4(arrType->baseType());

				auto storageKind = m_storageMapper.shouldUseBoxStorage(*var)
					? awst::AppStorageKind::Box
					: awst::AppStorageKind::AppGlobal;

				auto arrayRead = m_storageMapper.createStateRead(
					var->name(), arrWType, storageKind, loc
				);

				auto idxRef = awst::makeVarExpression(getter.args[0].name, getter.args[0].wtype, loc);
				auto idx = TypeCoercion::implicitNumericCast(
					idxRef, awst::WType::uint64Type(), loc);

				auto indexExpr = awst::makeIndexExpression(std::move(arrayRead), std::move(idx), elemARC4, loc);

				// Decode ARC4 element to native type (e.g. arc4.uint256 → biguint).
				auto* nativeElem = m_typeMapper.map(arrType->baseType());
				std::shared_ptr<awst::Expression> result = std::move(indexExpr);

				// Struct element: decompose ARC4Struct into primitive-fields tuple
				// (Solidity public-accessor skips mappings and non-bytes arrays).
				auto const* solStructElem = dynamic_cast<solidity::frontend::StructType const*>(arrType->baseType());
				if (solStructElem && solReturnTypes.size() > 1)
				{
					auto const* arc4Struct = dynamic_cast<awst::ARC4Struct const*>(elemARC4);
					auto tuple = awst::makeTupleExpression(getter.returnType, loc);

					for (auto& item: projectStructFields(m_typeMapper, solStructElem, arc4Struct, result, loc))
						tuple->items.push_back(std::move(item));
					readExpr = std::move(tuple);
				}
				else
				{
					if (!awst::structurallyEquivalent(elemARC4, nativeElem))
					{
						auto decode = awst::makeARC4Decode(std::move(result), nativeElem, loc);
						result = std::move(decode);
					}

					readExpr = std::move(result);
				}
			}
			else
			{
				// Walk type outer-to-inner: Mapping/array-of-mapping → box key (K…K);
				// array-of-flat-elements → IndexExpression on the value (I…I).
				// Mirrors SolIndexAccess::handleMappingAccess key derivation.
				// arg order: K…K I…I.
				solidity::frontend::Type const* walkType = var->type();
				size_t keyArgCount = 0;
				size_t indexArgCount = 0;
				bool inIndexMode = false;
				solidity::frontend::Type const* storedValueType = walkType;
				// Per-key encoding: array-of-mapping levels use uint64 (itob 8B);
				// mapping levels use declared keyType (biguint → 32B pad).
				std::vector<awst::WType const*> keyArgEncodingType;
				std::vector<uint64_t> keyArgStaticLen; // 0 = dynamic, >0 = static N
				std::vector<bool> keyArgIsArrayLevel;

				while (keyArgCount + indexArgCount < getter.args.size())
				{
					if (auto const* mt = dynamic_cast<solidity::frontend::MappingType const*>(walkType))
					{
						if (inIndexMode) break;
						keyArgEncodingType.push_back(m_typeMapper.map(mt->keyType()));
						keyArgIsArrayLevel.push_back(false);
						keyArgStaticLen.push_back(0);
						keyArgCount++;
						walkType = mt->valueType();
						continue;
					}
					if (auto const* at = dynamic_cast<solidity::frontend::ArrayType const*>(walkType))
					{
						if (at->isByteArrayOrString()) break;
						if (!inIndexMode && containsMappingType(at->baseType()))
						{
							keyArgEncodingType.push_back(awst::WType::uint64Type());
							keyArgIsArrayLevel.push_back(true);
							// Static: arraySize() = N; dynamic: 0 = look up length at bounds-check.
							keyArgStaticLen.push_back(
								at->isDynamicallySized() ? 0 : static_cast<uint64_t>(at->length()));
							keyArgCount++;
							walkType = at->baseType();
							continue;
						}
						if (!inIndexMode)
						{
							inIndexMode = true;
							storedValueType = walkType;
						}
						indexArgCount++;
						walkType = at->baseType();
						continue;
					}
					break;
				}
				if (!inIndexMode)
					storedValueType = walkType;

				awst::WType const* storedWType = m_typeMapper.map(storedValueType);
				solidity::frontend::Type const* valueType = walkType; // deepest type, for struct decomposition

				std::shared_ptr<awst::Expression> storageRead;
				if (keyArgCount == 0)
				{
					// No mapping keys: plain multi-dim array; read the whole value.
					auto storageKind = m_storageMapper.shouldUseBoxStorage(*var)
						? awst::AppStorageKind::Box
						: awst::AppStorageKind::AppGlobal;
					storageRead = m_storageMapper.createStateRead(
						var->name(), storedWType, storageKind, loc);
				}
				else
				{
				// Per-layer hash (mirrors handleMappingAccess writer).
				std::shared_ptr<awst::Expression> currentPrefix = awst::makeUtf8BytesConstant(
					var->name(), loc, awst::WType::boxKeyType());

				for (size_t i = 0; i < keyArgCount; ++i)
				{
					auto argRef = awst::makeVarExpression(getter.args[i].name, getter.args[i].wtype, loc);
					auto const* encType = i < keyArgEncodingType.size() ? keyArgEncodingType[i] : argRef->wtype;
					std::shared_ptr<awst::Expression> encoded = argRef;
					if (encType != argRef->wtype)
						encoded = TypeCoercion::implicitNumericCast(std::move(encoded), encType, loc);

					// Bounds-check array-of-non-flat levels (Panic(0x32) on OOB).
					// Skip mapping levels — they return defaults, not revert.
					bool isArrayLevel = i < keyArgIsArrayLevel.size() && keyArgIsArrayLevel[i];
					if (isArrayLevel)
					{
						uint64_t staticN = i < keyArgStaticLen.size() ? keyArgStaticLen[i] : 0;
						auto argForCheck = awst::makeVarExpression(getter.args[i].name, getter.args[i].wtype, loc);
						auto argU64 = TypeCoercion::implicitNumericCast(std::move(argForCheck), awst::WType::uint64Type(), loc);

						std::shared_ptr<awst::Expression> lengthExpr;
						if (staticN > 0)
						{
							// Static: compile-time length N.
							lengthExpr = awst::makeIntegerConstant(std::to_string(staticN), loc, awst::WType::uint64Type());
						}
						else
						{
							// Dynamic: length in first 2 bytes of box (ARC4 header).
							// Materialise prefix so bounds-check + next-layer hash don't re-emit.
							std::string tempName = "__bounds_prefix_" + std::to_string(awst::NameGen::next("PublicGetterBuilder.s_boundsCounter"));
							auto tempVar = awst::makeVarExpression(tempName, awst::WType::boxKeyType(), loc);
							auto saveStmt = awst::makeAssignmentStatement(tempVar, std::move(currentPrefix), loc);
							body->body.push_back(std::move(saveStmt));
							currentPrefix = tempVar;

							auto boxExpr = awst::makeBoxValueExpression(currentPrefix, awst::WType::bytesType(), loc);
							std::vector<unsigned char> twoZeros{0, 0};
							auto defaultBytes = awst::makeBytesConstant(std::move(twoZeros), loc);
							auto stateGet = awst::makeStateGet(std::move(boxExpr), std::move(defaultBytes), awst::WType::bytesType(), loc);
							lengthExpr = awst::makeExtractUInt16(
								std::move(stateGet), awst::makeZero(loc), loc);
						}

						auto cmp = awst::makeNumericCompare(std::move(argU64), awst::NumericComparison::Lt, std::move(lengthExpr), loc);
						auto assertExpr = awst::makeAssert(std::move(cmp), loc, "array out-of-bounds");
						body->body.push_back(awst::makeExpressionStatement(std::move(assertExpr), loc));
					}

					currentPrefix = awst::makeMappingKeyLayer(
						std::move(encoded), encType, std::move(currentPrefix), loc);
				}

				// makeStateGetWithDefault: avoids StateGet for large/dynamic types (>4KB stack cap).
				auto boxExpr = awst::makeBoxValueExpression(std::move(currentPrefix), storedWType, loc);
				storageRead = StorageMapper::makeStateGetWithDefault(std::move(boxExpr), storedWType, loc);
				} // end keyArgCount > 0 branch

				// Index into any array dims inside the box value (e.g. mapping(K=>T[N]) → index T[N]).
				std::shared_ptr<awst::Expression> indexed = std::move(storageRead);
				{
					solidity::frontend::Type const* walkType = storedValueType;
					for (size_t i = 0; i < indexArgCount; ++i)
					{
						auto const* at = dynamic_cast<solidity::frontend::ArrayType const*>(walkType);
						if (!at)
							break;
						auto* elemARC4 = m_typeMapper.mapSolTypeToARC4(at->baseType());

						auto idxRef = awst::makeVarExpression(
							getter.args[keyArgCount + i].name,
							getter.args[keyArgCount + i].wtype, loc);
						auto idx = TypeCoercion::implicitNumericCast(
							idxRef, awst::WType::uint64Type(), loc);

						auto indexExpr = awst::makeIndexExpression(std::move(indexed), std::move(idx), elemARC4, loc);
						indexed = std::move(indexExpr);

						walkType = at->baseType();
					}
				}

				// Struct: project primitive fields flat (skip mappings/non-bytes arrays).
				if (auto const* structType = dynamic_cast<solidity::frontend::StructType const*>(valueType))
				{
					if (solReturnTypes.size() >= 1)
					{
						std::shared_ptr<awst::Expression> fullStruct = std::move(indexed);
						auto const* arc4Struct = dynamic_cast<awst::ARC4Struct const*>(fullStruct->wtype);

						auto items = projectStructFields(
							m_typeMapper, structType, arc4Struct, fullStruct, loc);

						if (items.size() == 1)
						{
							readExpr = std::move(items[0]);
						}
						else
						{
							auto tuple = awst::makeTupleExpression(getter.returnType, loc);
							for (auto& it : items)
								tuple->items.push_back(std::move(it));
							readExpr = std::move(tuple);
						}
					}
					else
					{
						readExpr = std::move(indexed);
					}
				}
				else
				{
					readExpr = std::move(indexed);

					// Decode ARC4 element to native type (e.g. arc4.uint8 → uint64).
					if (readExpr && readExpr->wtype && readExpr->wtype != getter.returnType)
					{
						auto const* arc4Elem = dynamic_cast<awst::ARC4UIntN const*>(readExpr->wtype);
						if (arc4Elem && (getter.returnType == awst::WType::uint64Type()
							|| getter.returnType == awst::WType::biguintType()))
						{
							auto decode = awst::makeARC4Decode(std::move(readExpr), getter.returnType, loc);
							readExpr = std::move(decode);
						}
					}
				}
			}

			if (signedGetterBits > 0 && readExpr) // sign-extend signed integer return
			{
				readExpr = TypeCoercion::signExtendToUint256(std::move(readExpr), signedGetterBits, loc);
			}

			// ABI param validation for getter key params (sub-64-bit mapping keys).
			bool getterV2 = true;
			{
				auto const& ann = _contract.sourceUnit().annotation();
				if (ann.useABICoderV2.set())
					getterV2 = *ann.useABICoderV2;
			}
			// Reuse buildABIEntryChecks (same as the router) inserted BEFORE key derivation.
			// Sub-64-bit mapping keys (e.g. mapping(uint8=>V)) otherwise alias wrong slots.
			// (Array-index params are uint256 and are unaffected.)
			{
				std::vector<ABIParamDesc> descs;
				descs.reserve(solParamTypes.size());
				for (size_t pi = 0; pi < solParamTypes.size(); ++pi)
				{
					std::string pname = (pi < solParamNames.size() && !solParamNames[pi].empty())
						? solParamNames[pi] : "key" + std::to_string(pi);
					descs.push_back({solParamTypes[pi], std::move(pname), loc});
				}
				// _enumChecksRequireV2=true: an auto-getter does not range-
				// check enum keys under abicoder v1 (matches solc).
				auto checks = buildABIEntryChecks(descs, getterV2, /*_enumChecksRequireV2=*/true);
				body->body.insert(
					body->body.begin(),
					std::make_move_iterator(checks.begin()),
					std::make_move_iterator(checks.end()));
			}

			auto ret = awst::makeReturnStatement(std::move(readExpr), loc);
			body->body.push_back(std::move(ret));

			getter.body = body;

			// Remap biguint getter params to ARC4UIntN at the key's DECLARED
			// width (not a blanket 256): explicit functions publish declared
			// bits for >64-bit params (`probe(uint128)`), and the cross-
			// contract caller derives the selector + arg encoding from the
			// getter's solc FunctionType — a blanket uint256 made every
			// keyed getter call revert on selector mismatch.
			{
				std::vector<std::shared_ptr<awst::Statement>> decodeStmts;
				for (size_t gi = 0; gi < getter.args.size(); ++gi)
				{
					auto& garg = getter.args[gi];
					if (garg.wtype != awst::WType::biguintType())
						continue;
					unsigned bits = 256;
					if (gi < solParamTypes.size())
						if (auto it = builder::SolIntType::fromSol(solParamTypes[gi]))
							bits = it->bits;
					auto const* arc4Type = m_typeMapper.createType<awst::ARC4UIntN>(bits);
					std::string origName = garg.name;
					std::string arc4Name = "__arc4_" + origName;
					garg.wtype = arc4Type;
					garg.name = arc4Name;

					auto arc4Var = awst::makeVarExpression(arc4Name, arc4Type, loc);

					auto decode = arc4UintCodec(std::move(arc4Var), arc4Type, /*isEncode=*/false, loc);

					auto target = awst::makeVarExpression(origName, awst::WType::biguintType(), loc);

					auto assign = awst::makeAssignmentStatement(std::move(target), std::move(decode), loc);
					decodeStmts.push_back(std::move(assign));
				}
				if (!decodeStmts.empty())
					getter.body->body.insert(
						getter.body->body.begin(),
						std::make_move_iterator(decodeStmts.begin()),
						std::make_move_iterator(decodeStmts.end())
					);
			}

			// Remap biguint return to ARC4UIntN: ABI selector "uintN" not "uint512".
			// Without this puya's router publishes `received()uint512` while callers
			// and the arc56 spec compute `received()uint256` — the inner call then
			// falls to the callee's FALLBACK (silent wrong path; empty return log).
			// Unsigned: declared width. Signed: canonical 256-bit TC, so encode at
			// 256 bits always — a sign-extended negative doesn't fit arc4.uintN for
			// N<256 (matches FunctionBuilder's signed-return promotion).
			bool isIntReturn = false;
			unsigned retBits = 256;
			if (getter.returnType == awst::WType::biguintType()
				&& solReturnTypes.size() == 1)
			{
				if (auto intInfo = builder::SolIntType::fromSol(solReturnTypes[0]))
				{
					isIntReturn = true;
					retBits = intInfo->isSigned ? 256 : intInfo->bits;
				}
			}
			if (isIntReturn)
			{
				auto const* arc4RetType = m_typeMapper.createType<awst::ARC4UIntN>(static_cast<int>(retBits));
				forEachReturnStatement(getter.body->body, [&](awst::ReturnStatement& ret) {
					if (ret.value && ret.value->wtype == awst::WType::biguintType()) {
						auto retLoc = ret.value->sourceLocation;
						ret.value = arc4UintCodec(std::move(ret.value), arc4RetType, /*isEncode=*/true, retLoc);
					}
				});
				getter.returnType = arc4RetType;
			}

			prependNonPayableCheck(getter); // getters are always view/non-payable

			contract->methods.push_back(std::move(getter));
		}
	});
}

} // namespace puyasol::builder
