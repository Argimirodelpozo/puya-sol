#include "builder/contract/ContractBuilder.h"
#include "builder/AWSTBuilder.h"
#include "builder/contract/StateVarWalker.h"
#include "builder/contract/ParamABIValidator.h"
#include "builder/contract/ReturnRewriter.h"
#include "builder/sol-types/TypeCoercion.h"

#include <libsolidity/ast/ASTVisitor.h>

namespace puyasol::builder
{

namespace
{

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
			auto decode = awst::makeARC4Decode(std::move(fieldExpr), nativeType, loc);
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
				auto const* solType = solReturnTypes[0];
				if (auto const* udvt = dynamic_cast<solidity::frontend::UserDefinedValueType const*>(solType))
					solType = &udvt->underlyingType();
				if (auto const* intType = dynamic_cast<solidity::frontend::IntegerType const*>(solType))
				{
					if (intType->isSigned() && intType->numBits() <= 64)
					{
						getter.returnType = awst::WType::biguintType();
						signedGetterBits = intType->numBits();
					}
				}
			}
			else if (solReturnTypes.size() > 1)
			{
				std::vector<awst::WType const*> tupleTypes;
				std::vector<std::string> tupleNames;
				for (size_t i = 0; i < solReturnTypes.size(); ++i)
				{
					tupleTypes.push_back(m_typeMapper.map(solReturnTypes[i]));
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
			if (var->isConstant())
			{
				// Compile-time constant: return directly.
				if (var->value())
					readExpr = m_exprBuilder->build(*var->value());
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
				auto storageKind = StorageMapper::shouldUseBoxStorage(*var)
					? awst::AppStorageKind::Box
					: awst::AppStorageKind::AppGlobal;

				// Struct with multiple return values: read full ARC4Struct, project each field.
				auto const* solStructType = dynamic_cast<solidity::frontend::StructType const*>(var->type());
				if (solStructType && solReturnTypes.size() > 1)
				{
					auto* storedWType = m_typeMapper.map(var->type());
					auto fullStruct = m_storageMapper.createStateRead(
						var->name(), storedWType, storageKind, loc
					);

					auto const* arc4Struct = dynamic_cast<awst::ARC4Struct const*>(storedWType);
					auto tuple = awst::makeTupleExpression(getter.returnType, loc);

					for (auto& item: projectStructFields(m_typeMapper, solStructType, arc4Struct, fullStruct, loc))
						tuple->items.push_back(std::move(item));
					readExpr = std::move(tuple);
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

				auto storageKind = StorageMapper::shouldUseBoxStorage(*var)
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
					if (elemARC4 != nativeElem && elemARC4->name() != nativeElem->name())
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
					auto storageKind = StorageMapper::shouldUseBoxStorage(*var)
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
							static int s_boundsCounter = 0;
							std::string tempName = "__bounds_prefix_" + std::to_string(s_boundsCounter++);
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

			// Remap biguint getter params to ARC4UIntN(256): ABI selector "uint256" not "uint512".
			{
				std::vector<std::shared_ptr<awst::Statement>> decodeStmts;
				for (auto& garg: getter.args)
				{
					if (garg.wtype != awst::WType::biguintType())
						continue;
					auto const* arc4Type = m_typeMapper.createType<awst::ARC4UIntN>(256);
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

			// Remap biguint return to ARC4UIntN(N): ABI selector "uintN" not "uint512".
			// Unsigned only: signed two's-complement biguint must NOT be wrapped (overflow).
			bool isUnsignedIntReturn = false;
			unsigned retBits = 256;
			if (getter.returnType == awst::WType::biguintType()
				&& solReturnTypes.size() == 1)
			{
				auto const* retSolType = solReturnTypes[0];
				if (auto const* udvt = dynamic_cast<solidity::frontend::UserDefinedValueType const*>(retSolType))
					retSolType = &udvt->underlyingType();
				if (auto const* intType = dynamic_cast<solidity::frontend::IntegerType const*>(retSolType))
				{
					if (!intType->isSigned())
					{
						isUnsignedIntReturn = true;
						retBits = intType->numBits();
					}
				}
			}
			if (isUnsignedIntReturn)
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
