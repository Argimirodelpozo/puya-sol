#include "builder/ContractBuilder.h"
#include "builder/sol-types/TypeCoercion.h"
#include "Logger.h"

#include <libsolidity/ast/ASTVisitor.h>

namespace puyasol::builder
{

void ContractBuilder::buildPublicStateVariableGetters(
	solidity::frontend::ContractDefinition const& _contract,
	awst::Contract& _contractNode,
	std::string const& _contractName,
	std::set<std::string>& _translatedFunctions)
{
	auto* contract = &_contractNode;
	auto const& contractName = _contractName;
	auto& translatedFunctions = _translatedFunctions;
	for (auto const* base: _contract.annotation().linearizedBaseContracts)
	{
		for (auto const* var: base->stateVariables())
		{
			if (var->visibility() != solidity::frontend::Visibility::Public)
				continue;
			if (translatedFunctions.count(var->name()))
				continue; // explicit getter already exists

			// Get the Solidity-computed getter function type.
			// This gives us parameter types (mapping keys, array indices)
			// and return types (struct field filtering).
			auto getterFuncType = var->functionType(/*_internal=*/false);
			if (!getterFuncType)
				continue;

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

			// Build getter parameters from the Solidity getter function type.
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

			// Determine return type from Solidity getter return types.
			auto const& solReturnTypes = getterFuncType->returnParameterTypes();
			auto const& solReturnNames = getterFuncType->returnParameterNames();
			// Track signed return for sign-extension
			unsigned signedGetterBits = 0;
			if (solReturnTypes.size() == 1)
			{
				getter.returnType = m_typeMapper.map(solReturnTypes[0]);
				// Detect signed integer return ≤64 bits for sign-extension
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
				// Multiple return values (struct fields) — use WTuple.
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
				// No return types — shouldn't happen for getters, skip.
				continue;
			}

			// Build body: return value
			auto body = std::make_shared<awst::Block>();
			body->sourceLocation = loc;

			std::shared_ptr<awst::Expression> readExpr;
			if (var->isConstant())
			{
				// Compile-time constant — return the value directly.
				if (var->value())
					readExpr = m_exprBuilder->build(*var->value());
				if (!readExpr)
					readExpr = StorageMapper::makeDefaultValue(getter.returnType, loc);
				if (readExpr && readExpr->wtype != getter.returnType)
					readExpr = TypeCoercion::implicitNumericCast(
						std::move(readExpr), getter.returnType, loc
					);
				// String literal → bytes[N]: right-pad to N bytes
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
						// Generic ReinterpretCast for other bytes-compatible coercions
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
				// Simple state variable (no mapping/array params) — read from storage.
				auto storageKind = StorageMapper::shouldUseBoxStorage(*var)
					? awst::AppStorageKind::Box
					: awst::AppStorageKind::AppGlobal;

				// For struct types with multiple return values, read the full
				// ARC4Struct and extract/decode each returned field.
				auto const* solStructType = dynamic_cast<solidity::frontend::StructType const*>(var->type());
				if (solStructType && solReturnTypes.size() > 1)
				{
					auto* storedWType = m_typeMapper.map(var->type());
					auto fullStruct = m_storageMapper.createStateRead(
						var->name(), storedWType, storageKind, loc
					);

					auto const* arc4Struct = dynamic_cast<awst::ARC4Struct const*>(storedWType);
					auto tuple = std::make_shared<awst::TupleExpression>();
					tuple->sourceLocation = loc;
					tuple->wtype = getter.returnType;

					for (auto const& member: solStructType->members(nullptr))
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

						auto fieldExpr = std::make_shared<awst::FieldExpression>();
						fieldExpr->sourceLocation = loc;
						fieldExpr->wtype = arc4FieldType ? arc4FieldType : m_typeMapper.map(member.type);
						fieldExpr->base = fullStruct;
						fieldExpr->name = member.name;

						auto* nativeType = m_typeMapper.map(member.type);
						if (arc4FieldType && arc4FieldType != nativeType)
						{
							auto decode = std::make_shared<awst::ARC4Decode>();
							decode->sourceLocation = loc;
							decode->wtype = nativeType;
							decode->value = std::move(fieldExpr);
							tuple->items.push_back(std::move(decode));
						}
						else
							tuple->items.push_back(std::move(fieldExpr));
					}
					readExpr = std::move(tuple);
				}
				else
				{
					// Read with original storage type (not promoted return type)
					auto* readType = signedGetterBits > 0
						? m_typeMapper.map(var->type()) : getter.returnType;

					// Transient state vars: route through TransientStorage
					// (scratch slot TRANSIENT_SLOT packed blob) so the getter
					// sees the same storage as direct named-var reads/writes.
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
				// Array state var `T[]` / `T[N] public array` → getter(uint256 i).
				// The backing store is a single state slot (box for dynamic /
				// oversized static, AppGlobal otherwise) containing the packed
				// ARC4 array; reading element i uses IndexExpression on that
				// slot, NOT a sha256-based mapping key.
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

				auto indexExpr = std::make_shared<awst::IndexExpression>();
				indexExpr->sourceLocation = loc;
				indexExpr->wtype = elemARC4;
				indexExpr->base = std::move(arrayRead);
				indexExpr->index = std::move(idx);

				// Decode ARC4 element back to native type (e.g. arc4.uint256 → biguint)
				auto* nativeElem = m_typeMapper.map(arrType->baseType());
				std::shared_ptr<awst::Expression> result = std::move(indexExpr);

				// Struct element with multi-field getter signature: decompose
				// the ARC4 struct into a tuple of non-mapping, non-dynamic-array
				// fields. Matches Solidity's public-accessor behavior for
				// `Struct[N] public p` where the getter returns the primitive
				// fields flat rather than the struct itself.
				auto const* solStructElem = dynamic_cast<solidity::frontend::StructType const*>(arrType->baseType());
				if (solStructElem && solReturnTypes.size() > 1)
				{
					auto const* arc4Struct = dynamic_cast<awst::ARC4Struct const*>(elemARC4);
					auto tuple = std::make_shared<awst::TupleExpression>();
					tuple->sourceLocation = loc;
					tuple->wtype = getter.returnType;

					for (auto const& member: solStructElem->members(nullptr))
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

						auto fieldExpr = std::make_shared<awst::FieldExpression>();
						fieldExpr->sourceLocation = loc;
						fieldExpr->wtype = arc4FieldType ? arc4FieldType : m_typeMapper.map(member.type);
						fieldExpr->base = result;
						fieldExpr->name = member.name;

						auto* nativeFieldType = m_typeMapper.map(member.type);
						if (arc4FieldType && arc4FieldType != nativeFieldType)
						{
							auto decode = std::make_shared<awst::ARC4Decode>();
							decode->sourceLocation = loc;
							decode->wtype = nativeFieldType;
							decode->value = std::move(fieldExpr);
							tuple->items.push_back(std::move(decode));
						}
						else
							tuple->items.push_back(std::move(fieldExpr));
					}
					readExpr = std::move(tuple);
				}
				else
				{
					if (elemARC4 != nativeElem && elemARC4->name() != nativeElem->name())
					{
						auto decode = std::make_shared<awst::ARC4Decode>();
						decode->sourceLocation = loc;
						decode->wtype = nativeElem;
						decode->value = std::move(result);
						result = std::move(decode);
					}

					readExpr = std::move(result);
				}
			}
			else
			{
				// Mapping/array getter — build box read with key from mapping arguments,
				// then index into the stored value for each nested array dimension.
				// Walk mappings first to determine how many args feed the box key.
				solidity::frontend::Type const* valueType = var->type();
				size_t keyArgCount = 0;
				while (keyArgCount < getter.args.size())
				{
					if (auto const* mt = dynamic_cast<solidity::frontend::MappingType const*>(valueType))
					{
						valueType = mt->valueType();
						keyArgCount++;
					}
					else
						break;
				}

				// The type stored in the box (may be a nested array/struct).
				// Remaining args index into this value after the box read.
				solidity::frontend::Type const* storedValueType = valueType;

				// Walk nested arrays for index args.
				size_t indexArgCount = 0;
				solidity::frontend::Type const* elemType = storedValueType;
				while (keyArgCount + indexArgCount < getter.args.size())
				{
					if (auto const* at = dynamic_cast<solidity::frontend::ArrayType const*>(elemType))
					{
						if (at->isByteArrayOrString())
							break;
						elemType = at->baseType();
						indexArgCount++;
					}
					else
						break;
				}

				// Map the box-stored type (before indexing).
				awst::WType const* storedWType = m_typeMapper.map(storedValueType);
				// The unwound value type used for struct-field decomposition below
				// is the element type after indexing.
				valueType = elemType;

				std::shared_ptr<awst::Expression> storageRead;
				if (keyArgCount == 0)
				{
					// No mapping keys — the state var is a plain multi-dim array.
					// Use a regular state read (box/app-global) of the whole value.
					auto storageKind = StorageMapper::shouldUseBoxStorage(*var)
						? awst::AppStorageKind::Box
						: awst::AppStorageKind::AppGlobal;
					storageRead = m_storageMapper.createStateRead(
						var->name(), storedWType, storageKind, loc);
				}
				else
				{
				// Build the box key from the getter arguments.
				// Each arg is converted to bytes, concatenated, then sha256-hashed.
				auto prefix = awst::makeUtf8BytesConstant(
					var->name(), loc, awst::WType::boxKeyType());

				std::vector<std::shared_ptr<awst::Expression>> keyParts;
				for (size_t i = 0; i < keyArgCount; ++i)
				{
					auto argRef = awst::makeVarExpression(getter.args[i].name, getter.args[i].wtype, loc);

					std::shared_ptr<awst::Expression> keyBytes;
					if (argRef->wtype == awst::WType::uint64Type())
					{
						auto itob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), loc);
						itob->stackArgs.push_back(std::move(argRef));
						keyBytes = std::move(itob);
					}
					else if (argRef->wtype == awst::WType::biguintType())
					{
						// Normalize biguint to exactly 32 bytes before hashing.
						auto reinterpret = awst::makeReinterpretCast(std::move(argRef), awst::WType::bytesType(), loc);

						auto padWidth = awst::makeIntegerConstant("32", loc);

						auto pad = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), loc);
						pad->stackArgs.push_back(std::move(padWidth));

						auto cat = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), loc);
						cat->stackArgs.push_back(std::move(pad));
						cat->stackArgs.push_back(std::move(reinterpret));

						auto lenCall = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), loc);
						lenCall->stackArgs.push_back(cat);

						auto width32 = awst::makeIntegerConstant("32", loc);

						auto offset = awst::makeIntrinsicCall("-", awst::WType::uint64Type(), loc);
						offset->stackArgs.push_back(std::move(lenCall));
						offset->stackArgs.push_back(std::move(width32));

						auto width32b = awst::makeIntegerConstant("32", loc);

						auto extract = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), loc);
						extract->stackArgs.push_back(std::move(cat));
						extract->stackArgs.push_back(std::move(offset));
						extract->stackArgs.push_back(std::move(width32b));

						keyBytes = std::move(extract);
					}
					else
					{
						// string / bytes / address → ReinterpretCast to bytes
						auto reinterpret = awst::makeReinterpretCast(std::move(argRef), awst::WType::bytesType(), loc);
						keyBytes = std::move(reinterpret);
					}
					keyParts.push_back(std::move(keyBytes));
				}

				// Concatenate key parts
				std::shared_ptr<awst::Expression> compositeKey;
				if (keyParts.size() == 1)
					compositeKey = std::move(keyParts[0]);
				else
				{
					compositeKey = std::move(keyParts[0]);
					for (size_t i = 1; i < keyParts.size(); ++i)
					{
						auto concat = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), loc);
						concat->stackArgs.push_back(std::move(compositeKey));
						concat->stackArgs.push_back(std::move(keyParts[i]));
						compositeKey = std::move(concat);
					}
				}

				// Hash the composite key
				auto hashCall = awst::makeIntrinsicCall("sha256", awst::WType::bytesType(), loc);
				hashCall->stackArgs.push_back(std::move(compositeKey));

				auto boxKey = std::make_shared<awst::BoxPrefixedKeyExpression>();
				boxKey->sourceLocation = loc;
				boxKey->wtype = awst::WType::boxKeyType();
				boxKey->prefix = prefix;
				boxKey->key = std::move(hashCall);

				auto boxExpr = std::make_shared<awst::BoxValueExpression>();
				boxExpr->sourceLocation = loc;
				boxExpr->wtype = storedWType;
				boxExpr->key = std::move(boxKey);

				auto defaultVal = StorageMapper::makeDefaultValue(storedWType, loc);

				auto stateGet = std::make_shared<awst::StateGet>();
				stateGet->sourceLocation = loc;
				stateGet->wtype = storedWType;
				stateGet->field = std::move(boxExpr);
				stateGet->defaultValue = std::move(defaultVal);

				storageRead = std::move(stateGet);
				} // end keyArgCount > 0 branch

				// Apply index accesses for any array dimensions nested inside the box value
				// (e.g. `mapping(K => T[N])` keys by K, then indexes into T[N]).
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

						auto indexExpr = std::make_shared<awst::IndexExpression>();
						indexExpr->sourceLocation = loc;
						indexExpr->wtype = elemARC4;
						indexExpr->base = std::move(indexed);
						indexExpr->index = std::move(idx);
						indexed = std::move(indexExpr);

						walkType = at->baseType();
					}
				}

				// If the stored type is a struct but the getter returns a tuple
				// of selected fields, extract and ARC4-decode each field.
				if (auto const* structType = dynamic_cast<solidity::frontend::StructType const*>(valueType))
				{
					if (solReturnTypes.size() > 1)
					{
						// indexed returns the full ARC4Struct; extract fields.
						std::shared_ptr<awst::Expression> fullStruct = std::move(indexed);
						auto tuple = std::make_shared<awst::TupleExpression>();
						tuple->sourceLocation = loc;
						tuple->wtype = getter.returnType;

						// Get the ARC4Struct type's field types for FieldExpression
						auto const* arc4Struct = dynamic_cast<awst::ARC4Struct const*>(fullStruct->wtype);

						for (auto const& member: structType->members(nullptr))
						{
							if (member.type->category() == solidity::frontend::Type::Category::Mapping)
								continue;
							if (auto const* at = dynamic_cast<solidity::frontend::ArrayType const*>(member.type))
								if (!at->isByteArrayOrString())
									continue;

							// Look up the ARC4 field type from the struct type
							awst::WType const* arc4FieldType = nullptr;
							if (arc4Struct)
								for (auto const& [fname, ftype]: arc4Struct->fields())
									if (fname == member.name)
									{
										arc4FieldType = ftype;
										break;
									}

							auto fieldExpr = std::make_shared<awst::FieldExpression>();
							fieldExpr->sourceLocation = loc;
							fieldExpr->wtype = arc4FieldType ? arc4FieldType : m_typeMapper.map(member.type);
							fieldExpr->base = fullStruct;
							fieldExpr->name = member.name;

							// ARC4Decode to native type if needed
							auto* nativeType = m_typeMapper.map(member.type);
							if (arc4FieldType && arc4FieldType != nativeType)
							{
								auto decode = std::make_shared<awst::ARC4Decode>();
								decode->sourceLocation = loc;
								decode->wtype = nativeType;
								decode->value = std::move(fieldExpr);
								tuple->items.push_back(std::move(decode));
							}
							else
								tuple->items.push_back(std::move(fieldExpr));
						}
						readExpr = std::move(tuple);
					}
					else
					{
						readExpr = std::move(indexed);
					}
				}
				else
				{
					readExpr = std::move(indexed);

					// Decode ARC4 element to native getter return type if needed
					// (e.g. indexing into ARC4StaticArray<uint8,N> gives arc4.uint8 → uint64).
					if (readExpr && readExpr->wtype && readExpr->wtype != getter.returnType)
					{
						auto const* arc4Elem = dynamic_cast<awst::ARC4UIntN const*>(readExpr->wtype);
						if (arc4Elem && (getter.returnType == awst::WType::uint64Type()
							|| getter.returnType == awst::WType::biguintType()))
						{
							auto decode = std::make_shared<awst::ARC4Decode>();
							decode->sourceLocation = loc;
							decode->wtype = getter.returnType;
							decode->value = std::move(readExpr);
							readExpr = std::move(decode);
						}
					}
				}
			}

			// Sign-extend getter return for signed integer types
			if (signedGetterBits > 0 && readExpr)
			{
				readExpr = TypeCoercion::signExtendToUint256(std::move(readExpr), signedGetterBits, loc);
			}

			// ABI v2 validation for getter params (enum keys in mappings)
			bool getterV2 = true;
			{
				auto const& ann = _contract.sourceUnit().annotation();
				if (ann.useABICoderV2.set())
					getterV2 = *ann.useABICoderV2;
			}
			if (getterV2)
			{
				for (size_t pi = 0; pi < solParamTypes.size(); ++pi)
				{
					auto const* pt = solParamTypes[pi];
					// Enum validation
					if (auto const* enumType = dynamic_cast<solidity::frontend::EnumType const*>(pt))
					{
						unsigned memberCount = enumType->numberOfMembers();
						std::string pname = (pi < solParamNames.size() && !solParamNames[pi].empty())
							? solParamNames[pi] : "key" + std::to_string(pi);

						auto pv = awst::makeVarExpression(pname, awst::WType::uint64Type(), loc);

						auto mv = awst::makeIntegerConstant(std::to_string(memberCount - 1), loc);

						auto cmp = awst::makeNumericCompare(pv, awst::NumericComparison::Lte, std::move(mv), loc);

						auto as = awst::makeExpressionStatement(awst::makeAssert(std::move(cmp), loc, "enum validation"), loc);
						body->body.push_back(std::move(as));
					}
				}
			}

			auto ret = awst::makeReturnStatement(std::move(readExpr), loc);
			body->body.push_back(std::move(ret));

			getter.body = body;

			// Remap biguint (uint256) getter parameters to ARC4UIntN(256) so the
			// ABI selector encodes as "uint256" (not "uint512"). Rename the arg to
			// __arc4_<name> and insert a decode statement at the top of the body.
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

					auto decode = std::make_shared<awst::ARC4Decode>();
					decode->sourceLocation = loc;
					decode->wtype = awst::WType::biguintType();
					decode->value = std::move(arc4Var);

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

			// Remap biguint return type to ARC4UIntN(N) so the ABI selector
			// uses Solidity's declared "uintN" signature, not the internal
			// "uint512" marker. Matches the wrap applied to regular method
			// returns; without this, external callers of `c.x()` compute
			// selector sha512_256("x()uint256") but the contract's dispatch
			// emitted sha512_256("x()uint512"), producing a `match` miss.
			//
			// Only applied to UNSIGNED integer returns: signed returns
			// (including signedGetterBits sign-extension) encode as two's
			// complement in biguint and need a different ARC4 path. Wrapping
			// a two's-complement biguint as ARC4UIntN would report overflow.
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
				std::function<void(std::vector<std::shared_ptr<awst::Statement>>&)> wrap;
				wrap = [&](std::vector<std::shared_ptr<awst::Statement>>& stmts) {
					for (auto& stmt : stmts) {
						if (auto* ret = dynamic_cast<awst::ReturnStatement*>(stmt.get())) {
							if (ret->value && ret->value->wtype == awst::WType::biguintType()) {
								auto encode = std::make_shared<awst::ARC4Encode>();
								encode->sourceLocation = ret->value->sourceLocation;
								encode->wtype = arc4RetType;
								encode->value = std::move(ret->value);
								ret->value = std::move(encode);
							}
						} else if (auto* ifElse = dynamic_cast<awst::IfElse*>(stmt.get())) {
							if (ifElse->ifBranch) wrap(ifElse->ifBranch->body);
							if (ifElse->elseBranch) wrap(ifElse->elseBranch->body);
						} else if (auto* block = dynamic_cast<awst::Block*>(stmt.get())) {
							wrap(block->body);
						}
					}
				};
				wrap(getter.body->body);
				getter.returnType = arc4RetType;
			}

			// Non-payable check for public state-variable getters
			// (auto-generated getters are always view, never payable).
			prependNonPayableCheck(getter);

			contract->methods.push_back(std::move(getter));
		}
	}
}

} // namespace puyasol::builder
