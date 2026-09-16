#include "builder/contract/ContractBuilder.h"
#include "builder/storage/named/StoragePathWalker.h"
#include "awst/NameGen.h"
#include "awst/TupleValue.h"
#include "builder/target/EvmLayoutMode.h"
#include "builder/storage/slot/EvmSlotLowering.h"
#include "Logger.h"
#include "builder/contract/AWSTBuilder.h"
#include "builder/storage/StateVarWalker.h"
#include "builder/contract/ParamABIValidator.h"
#include "builder/types/TypeCoercion.h"
#include "builder/types/SolIntType.h"
#include "builder/codec/EvmValueCodec.h"

#include <libsolidity/ast/ASTVisitor.h>

namespace puyasol::builder
{

namespace
{

using Expr = std::shared_ptr<awst::Expression>;

Expr packGetterValues(TypeMapper& types, std::vector<Expr> values,
	awst::SourceLocation const& loc,
	std::optional<std::vector<std::string>> names = std::nullopt)
{
	if (values.size() == 1) return std::move(values.front());
	std::vector<awst::WType const*> fields;
	for (auto const& value: values) fields.push_back(value->wtype);
	auto tuple = awst::makeTupleExpression(
		types.createType<awst::WTuple>(std::move(fields), std::move(names)), loc);
	tuple->items = std::move(values);
	return tuple;
}

/// Physical readers return native values; this one boundary owns cleanup,
/// signed extension, and wire encoding for every getter shape.
Expr finishGetterRead(TypeMapper& types, solidity::frontend::FunctionType const& getter,
	Expr value, std::vector<std::shared_ptr<awst::Statement>>& pre,
	awst::SourceLocation const& loc)
{
	auto const& returns = getter.returnParameterTypes();
	auto values = returns.size() == 1 ? std::vector<Expr>{std::move(value)}
		: awst::tupleItems(std::move(value), loc, &pre);
	for (size_t i = 0; i < returns.size(); ++i)
	{
		auto plan = planReturnElement(types, returns[i], abiReturnNativeType(types, returns[i]));
		values[i] = TypeCoercion::encodeReturnElement(
			codec::valueFromArc4(types, returns[i], std::move(values[i]), loc), plan, loc);
	}
	return packGetterValues(types, std::move(values), loc, getter.returnParameterNames());
}

/// solc's getter FunctionType owns the projection, including one-field
/// structs and hidden mapping/array members. The physical stored struct can
/// still contain placeholders for members that are absent from this interface.
Expr projectStructFields(
	TypeMapper& types, solidity::frontend::FunctionType const& getter,
	awst::ARC4Struct const* stored, std::shared_ptr<awst::Expression> const& base,
	std::vector<std::shared_ptr<awst::Statement>>& pre, awst::SourceLocation const& loc)
{
	std::vector<std::shared_ptr<awst::Expression>> items;
	auto const& names = getter.returnParameterNames();
	auto const& returns = getter.returnParameterTypes();
	for (size_t i = 0; i < returns.size(); ++i)
	{
		auto const* fieldType = types.map(returns[i]);
		if (stored)
			for (auto const& [name, type]: stored->fields())
				if (name == names[i]) { fieldType = type; break; }
		auto field = StorageMapper::makePartialBoxReadWithDefault(types,
			awst::makeFieldExpression(base, names[i], fieldType, loc), pre, loc);
		items.push_back(std::move(field));
	}
	return packGetterValues(types, std::move(items), loc);
}

// ── buildPublicStateVariableGetters branch builders ─────────────────────
// Each builds the getter's readExpr for one variable shape; pure extractions
// from the per-var lambda (same emission order).

/// Slot-mode getter: walk the declared type over the getter args (mapping keys / array indices) to the leaf's slot address and read …
std::shared_ptr<awst::Expression> buildSlotModeGetterRead(
	eb::ContractContext& exprBuilder,
	TypeMapper& tm,
	solidity::frontend::VariableDeclaration const& _var,
	awst::ContractMethod& getter,
	awst::Block& body,
	awst::SourceLocation const& loc)
{
	auto const* var = &_var;
	std::shared_ptr<awst::Expression> readExpr;
	// --evm-storage-layout: walk the declared type over the getter
	// args (mapping keys / array indices) to the leaf's slot address
	// and read it there — the slot-mode twin of the branches below.
	sol_ast::EvmSlotLowering low(
		exprBuilder, *exprBuilder.currentScope, loc);
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
			a.wtype = tm.map(walk);
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
			auto idx = TypeCoercion::coerceScalar(
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
			auto idxCheck = TypeCoercion::coerceScalar(
				std::move(idxRef2), awst::WType::biguintType(), loc);
			auto cmp = awst::makeNumericCompare(std::move(idxCheck),
				awst::NumericComparison::Lt, std::move(lenExpr), loc);
			body.body.push_back(awst::makeExpressionStatement(
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
			auto const* function = var->functionType(false);
			auto const& names = function->returnParameterNames();
			auto const& returns = function->returnParameterTypes();
			std::vector<std::shared_ptr<awst::Expression>> items;
			for (size_t i = 0; i < returns.size(); ++i)
			{
				auto const* storedType = st->memberType(names[i]);
				auto field = low.memberAddr(addr->slot, st, names[i], storedType);
				items.push_back(low.readAny(field, storedType));
			}
			readExpr = packGetterValues(tm, std::move(items), loc);
		}
	}
	// flush anything the lowering queued (index pins etc.)
	for (auto& st2: exprBuilder.takePreEffects())
		body.body.push_back(std::move(st2));
	for (auto& st2: exprBuilder.takePostEffects())
		body.body.push_back(std::move(st2));
	if (!readExpr)
	{
		Logger::instance().warning(
			"--evm-storage-layout: skipping auto-getter for public "
			"state variable '" + var->name()
			+ "' (shape not yet supported; write an explicit getter)",
			loc);
		return nullptr;
	}
	return readExpr;
}

/// Compile-time constant getter: return the initializer value directly.
std::shared_ptr<awst::Expression> buildConstantGetterRead(
	eb::ContractContext& exprBuilder,
	solidity::frontend::VariableDeclaration const& _var,
	awst::WType const* returnType,
	awst::SourceLocation const& loc)
{
	auto const* var = &_var;
	std::shared_ptr<awst::Expression> readExpr;
	// Compile-time constant: return directly.
	if (var->value())
		readExpr = exprBuilder.buildExpr(*var->value());
	if (!readExpr)
		readExpr = TypeCoercion::makeDefaultValue(returnType, loc);
	return TypeCoercion::coerceScalar(std::move(readExpr), returnType, loc);
}

/// Simple state variable (no keys/indices): read from storage and project struct fields.
std::shared_ptr<awst::Expression> buildSimpleGetterRead(
	TypeMapper& tm,
	StorageMapper& sm,
	TransientStorage& ts,
	solidity::frontend::VariableDeclaration const& _var,
	awst::ContractMethod const& getter,
	size_t returnTypeCount,
	awst::Block& body,
	awst::SourceLocation const& loc)
{
	auto const* var = &_var;
	std::shared_ptr<awst::Expression> readExpr;
	// Simple state variable (no keys/indices): read from storage.
	auto binding = sm.physicalBindingFor(*var);

	// Single-field structs still need projection: their other stored members
	// can be absent from solc's getter interface.
	auto const* solStructType = dynamic_cast<solidity::frontend::StructType const*>(var->type());
	if (solStructType && returnTypeCount >= 1)
	{
		auto* storedWType = binding.wtype;
		auto fullStruct = sm.createStateRead(binding, loc);

		auto const* arc4Struct = dynamic_cast<awst::ARC4Struct const*>(storedWType);
		readExpr = projectStructFields(tm, *var->functionType(false),
			arc4Struct, fullStruct, body.body, loc);
	}
	else
	{
		// Use original storage type (not promoted return type).
		auto* readType = tm.map(var->type());

		// Transient vars: route through transient blob (same as named-var reads).
		if (var->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Transient
			&& ts.isTransient(*var))
		{
			readExpr = ts.buildRead(*var, loc);
		}
		if (!readExpr)
			readExpr = sm.createStateRead(
				binding.key, readType, binding.kind, loc
			);
	}
	return readExpr;
}

/// Flat array getter(i): IndexExpression into the packed ARC4 array slot (not a sha256 key); struct elements decompose into the …
std::shared_ptr<awst::Expression> buildFlatArrayGetterRead(
	TypeMapper& tm,
	StorageMapper& sm,
	solidity::frontend::VariableDeclaration const& _var,
	awst::ContractMethod const& getter,
	size_t returnTypeCount,
	awst::Block& body,
	awst::SourceLocation const& loc)
{
	auto const* var = &_var;
	std::shared_ptr<awst::Expression> readExpr;
	// Array getter(i): IndexExpression into the packed ARC4 array slot (not sha256 key).
	auto const* arrType = dynamic_cast<solidity::frontend::ArrayType const*>(var->type());
	auto binding = sm.physicalBindingFor(*var);
	auto* arrWType = binding.wtype;
	// Decode elements with the STORED element type: a mapping-carrying struct
	// element is encoded with byte[] placeholders for its mapping members (a
	// dynamic tuple), which the plain ABI mapping of the struct does not have.
	auto* elemARC4 = tm.mapSolTypeToARC4(arrType->baseType());
	if (auto const* storedDyn = dynamic_cast<awst::ARC4DynamicArray const*>(arrWType))
		elemARC4 = storedDyn->elementType();
	else if (auto const* storedStatic = dynamic_cast<awst::ARC4StaticArray const*>(arrWType))
		elemARC4 = storedStatic->elementType();

	auto arrayRead = sm.createStateRead(binding, loc);

	auto idxRef = awst::makeVarExpression(getter.args[0].name, getter.args[0].wtype, loc);
	auto idx = TypeCoercion::checkedIndexToUint64(body.body, std::move(idxRef), loc);

	// Solidity's generated getter indexes the array, so an out-of-range read is
	// Panic(0x32). Without this the element decode just reads whatever bytes sit
	// at that offset and answers (Privacy Pools' associationSets(uint256)
	// returned values where the EVM reverted); it only appeared to revert when
	// the offset happened to fall outside the backing box.
	std::shared_ptr<awst::Expression> arrLength;
	if (arrType->isDynamicallySized())
	{
		// Dynamic lengths consume the value; fixed lengths are solc facts and
		// must not materialize a possibly oversized backing box.
		std::string arrTmp = "__getter_arr_"
			+ std::to_string(awst::NameGen::next("PublicGetterBuilder.flatArr"));
		auto arrTmpVar = awst::makeVarExpression(arrTmp, arrWType, loc);
		body.body.push_back(
			awst::makeAssignmentStatement(arrTmpVar, std::move(arrayRead), loc));
		arrayRead = awst::makeVarExpression(arrTmp, arrWType, loc);
		arrLength = awst::makeArrayLength(
			arrayRead, awst::WType::uint64Type(), loc);
	}
	else
		arrLength = awst::makeIntegerConstant(
			arrType->length().str(), loc, awst::WType::uint64Type());
	body.body.push_back(awst::makeExpressionStatement(
		awst::makeAssert(
			awst::makeNumericCompare(idx,
				awst::NumericComparison::Lt, std::move(arrLength), loc),
			loc, "array out-of-bounds"),
		loc));

	// Decode ARC4 element to native type (e.g. arc4.uint256 → biguint).
	auto* nativeElem = tm.map(arrType->baseType());
	std::shared_ptr<awst::Expression> result;
	if (StorageMapper::isMultiBoxArray(arrWType))
	{
		auto page = StorageMapper::arrayPageForIndex(binding.key, arrWType, idx, body.body, loc);
		result = StorageMapper::makeBoxWindowRead(tm, page.key, page.offset, page.elementType, loc);
	}
	else
		result = awst::makeIndexExpression(std::move(arrayRead), std::move(idx), elemARC4, loc);

	// Struct element: decompose ARC4Struct into primitive-fields tuple
	// (Solidity public-accessor skips mappings and non-bytes arrays).
	auto const* solStructElem = dynamic_cast<solidity::frontend::StructType const*>(arrType->baseType());
	if (solStructElem && returnTypeCount >= 1)
	{
		// A struct whose accessor exposes ONE field (the others are mappings or
		// arrays) is still projected: decoding the whole placeholder-bearing
		// element as that scalar returned nothing (`Pool[] public pools`).
		auto const* arc4Struct = dynamic_cast<awst::ARC4Struct const*>(elemARC4);
		readExpr = projectStructFields(tm, *var->functionType(false),
			arc4Struct, result, body.body, loc);
	}
	else
	{
		result = StorageMapper::makePartialBoxReadWithDefault(tm, std::move(result), body.body, loc);
		if (!awst::structurallyEquivalent(elemARC4, nativeElem))
		{
			auto decode = awst::makeARC4Decode(std::move(result), nativeElem, loc);
			result = std::move(decode);
		}

		readExpr = std::move(result);
	}
	return readExpr;
}

/// Keyed getter (mapping / array-of-mapping / mapping-of-array): classify the args outer-to-inner (K…K I…I, mirroring …
std::shared_ptr<awst::Expression> buildKeyedGetterRead(
	TypeMapper& tm,
	StorageMapper& sm,
	solidity::frontend::VariableDeclaration const& _var,
	awst::ContractMethod const& getter,
	size_t returnTypeCount,
	awst::Block& body,
	awst::SourceLocation const& loc)
{
	auto const* var = &_var;
	std::shared_ptr<awst::Expression> readExpr;
	auto binding = sm.physicalBindingFor(*var);
	// Walk type outer-to-inner: Mapping/array-of-mapping → box key (K…K);
	// array-of-flat-elements → IndexExpression on the value (I…I).
	// Mirrors SolIndexAccess::handleMappingAccess key derivation.
	// arg order: K…K I…I.
	solidity::frontend::Type const* walkType = var->type();
	size_t keyArgCount = 0;
	size_t indexArgCount = 0;
	bool inIndexMode = false;
	solidity::frontend::Type const* storedValueType = walkType;
	auto indexedPathReachesMapping = [](
		solidity::frontend::Type const* type, size_t remainingArgs) {
		for (size_t i = 0; i < remainingArgs && type; ++i)
		{
			if (dynamic_cast<solidity::frontend::MappingType const*>(type))
				return true;
			auto const* array = dynamic_cast<
				solidity::frontend::ArrayType const*>(type);
			if (!array || array->isByteArrayOrString())
				return false;
			type = array->baseType();
		}
		return false;
	};

	while (keyArgCount + indexArgCount < getter.args.size())
	{
		if (auto const* mt = dynamic_cast<solidity::frontend::MappingType const*>(walkType))
		{
			if (inIndexMode) break;
			keyArgCount++;
			walkType = mt->valueType();
			continue;
		}
		if (auto const* at = dynamic_cast<solidity::frontend::ArrayType const*>(walkType))
		{
			if (at->isByteArrayOrString()) break;
			auto const consumed = keyArgCount + indexArgCount + 1;
			auto const remaining = getter.args.size() - consumed;
			if (!inIndexMode && indexedPathReachesMapping(at->baseType(), remaining))
			{
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

	awst::WType const* storedWType = tm.map(storedValueType);
	solidity::frontend::Type const* valueType = walkType; // deepest type, for struct decomposition

	auto argRef = [&](size_t i) {
		return awst::makeVarExpression(getter.args[i].name, getter.args[i].wtype, loc);
	};
	std::shared_ptr<awst::Expression> storageRead;
	if (keyArgCount == 0)
	{
		// No mapping keys: plain multi-dim array; read the whole value.
		storageRead = sm.createStateRead(
			binding.key, storedWType, binding.kind, loc);
	}
	else
	{
		// Per-layer hash (mirrors handleMappingAccess). Array-of-mapping levels
		// are bounds-checked (Panic(0x32)); mapping levels return defaults.
		StorageHolder holder{
			awst::makeUtf8BytesConstant(binding.key, loc, awst::WType::boxKeyType()), nullptr};
		if (auto const* rootArray = dynamic_cast<
				solidity::frontend::ArrayType const*>(var->type()))
			holder.value = sm.createStateRead(
				binding.key, tm.map(rootArray), binding.kind, loc);
		StoragePathWalker keys(tm, var->type(), loc, StoragePathWalker::ValueTracking::NestedArrays);
		for (size_t i = 0; i < keyArgCount; ++i)
			holder = keys.step(std::move(holder), argRef(i), body.body);

		// makeStateGetWithDefault: avoids StateGet for large/dynamic types (>4KB stack cap).
		auto boxExpr = awst::makeBoxValueExpression(std::move(holder.key), storedWType, loc);
		storageRead = StorageMapper::makeStateGetWithDefault(std::move(boxExpr), storedWType, loc);
	}

	// Index into any array dims inside the box value (e.g. mapping(K=>T[N]) → index T[N]).
	// Solidity panics 0x32 on an out-of-range index; every rank is checked
	// against the loaded value's length (Privacy Pools' associationSets(uint256)
	// answered where the EVM reverted).
	StorageHolder inlineHolder{nullptr, std::move(storageRead)};
	StoragePathWalker ranks(tm, storedValueType, loc);
	for (size_t i = 0; i < indexArgCount
		&& dynamic_cast<solidity::frontend::ArrayType const*>(ranks.current()); ++i)
		inlineHolder = ranks.step(std::move(inlineHolder), argRef(keyArgCount + i), body.body);
	std::shared_ptr<awst::Expression> indexed = std::move(inlineHolder.value);

	// Struct: project primitive fields flat (skip mappings/non-bytes arrays).
	if (auto const* structType = dynamic_cast<solidity::frontend::StructType const*>(valueType))
	{
		if (returnTypeCount >= 1)
		{
			std::shared_ptr<awst::Expression> fullStruct = std::move(indexed);
			auto const* arc4Struct = dynamic_cast<awst::ARC4Struct const*>(fullStruct->wtype);

			readExpr = projectStructFields(
				tm, *var->functionType(false), arc4Struct, fullStruct, body.body, loc);
		}
		else
		{
			readExpr = std::move(indexed);
		}
	}
	else
	{
		readExpr = StorageMapper::makePartialBoxReadWithDefault(tm, std::move(indexed), body.body, loc);

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
	return readExpr;
}

/// ABI param validation for getter key params (sub-64-bit mapping keys) — reuses buildABIEntryChecks (same as the router), inserted …
void prependGetterAbiChecks(
	TypeMapper& types,
	solidity::frontend::ContractDefinition const& _contract,
	solidity::frontend::TypePointers const& solParamTypes,
	std::vector<std::string> const& solParamNames,
	awst::Block& body,
	awst::SourceLocation const& loc)
{
	if (types.profile().contractAbi == ContractAbi::Evm) return;
	auto const& coder = _contract.sourceUnit().annotation().useABICoderV2;
	bool const validate = types.profile().viaIRSequencing || !coder.set() || *coder;
	std::vector<ABIParamDesc> descs;
	for (size_t i = 0; i < solParamTypes.size(); ++i)
		descs.push_back({solParamTypes[i],
			i < solParamNames.size() && !solParamNames[i].empty()
				? solParamNames[i] : "key" + std::to_string(i), loc, types.map(solParamTypes[i])});
	// Unlike explicit functions, solc's v1 getters don't validate enum keys.
	auto checks = buildABIEntryChecks(descs, validate, /*enumChecksRequireV2=*/true);
	body.body.insert(body.body.begin(), checks.begin(), checks.end());
}

/// Remap biguint getter params to ARC4UIntN at the key's DECLARED width (not a blanket 256): explicit functions publish declared …
void remapGetterParamsToArc4(
	TypeMapper& tm,
	awst::ContractMethod& getter,
	solidity::frontend::TypePointers const& solParamTypes,
	awst::SourceLocation const& loc)
{
	std::vector<std::shared_ptr<awst::Statement>> decodes;
	for (size_t i = 0; i < getter.args.size(); ++i)
	{
		auto& arg = getter.args[i];
		CallParameterPlan parameter;
		parameter.name = arg.name;
		parameter.type = arg.wtype;
		parameter.setAbiWireType(tm, solParamTypes.at(i));
		if (parameter.type == parameter.wireType) continue;
		arg.name = parameter.wireName();
		arg.wtype = parameter.wireType;
		decodes.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(parameter.name, parameter.type, loc),
			parameter.decodeArgument(awst::makeVarExpression(arg.name, arg.wtype, loc), loc), loc));
	}
	getter.body->body.insert(getter.body->body.begin(), decodes.begin(), decodes.end());
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
		getter.cref = m_contractId;
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
			std::string paramName = (i < solParamNames.size() && !solParamNames[i].empty())
				? solParamNames[i]
				: "key" + std::to_string(i);
			getter.args.emplace_back(
				std::move(paramName), m_typeMapper.map(solParamTypes[i]), loc);
		}

		auto const& solReturnTypes = getterFuncType->returnParameterTypes();
		auto const& solReturnNames = getterFuncType->returnParameterNames();
		if (solReturnTypes.empty()) return;
		std::vector<awst::WType const*> returnTypes;
		for (auto const* type: solReturnTypes)
			returnTypes.push_back(abiReturnNativeType(m_typeMapper, type));
		getter.returnType = returnTypes.size() == 1 ? returnTypes.front()
			: m_typeMapper.createType<awst::WTuple>(std::move(returnTypes), solReturnNames);

		auto body = awst::makeBlock(loc);

		std::shared_ptr<awst::Expression> readExpr;
		// Transient vars are NOT in the storage layout (slot space is
		// persistent storage only) — they keep the transient blob
		// getter below, same as default mode.
		if (!var->isConstant() && !var->immutable()
			&& var->referenceLocation()
				!= solidity::frontend::VariableDeclaration::Location::Transient
			&& m_typeMapper.profile().evmStorageLayout)
		{
			readExpr = buildSlotModeGetterRead(
				*m_exprBuilder, m_typeMapper, *var, getter, *body, loc);
			if (!readExpr)
				return;   // unsupported shape (warning already logged)
		}
		else if (var->isConstant())
			readExpr = buildConstantGetterRead(
				*m_exprBuilder, *var, getter.returnType, loc);
		else if (getter.args.empty())
			readExpr = buildSimpleGetterRead(
				m_typeMapper, m_storageMapper, m_transientStorage, *var,
				getter, solReturnTypes.size(), *body, loc);
		else if (dynamic_cast<solidity::frontend::ArrayType const*>(var->type())
			&& !dynamic_cast<solidity::frontend::ArrayType const*>(var->type())->isByteArrayOrString()
			&& getter.args.size() == 1)
			readExpr = buildFlatArrayGetterRead(
				m_typeMapper, m_storageMapper, *var,
				getter, solReturnTypes.size(), *body, loc);
		else
			readExpr = buildKeyedGetterRead(
				m_typeMapper, m_storageMapper, *var,
				getter, solReturnTypes.size(), *body, loc);

		prependGetterAbiChecks(m_typeMapper, _contract, solParamTypes, solParamNames, *body, loc);
		readExpr = finishGetterRead(m_typeMapper, *getterFuncType, std::move(readExpr), body->body, loc);
		getter.returnType = readExpr->wtype;

		auto ret = awst::makeReturnStatement(std::move(readExpr), loc);
		body->body.push_back(std::move(ret));

		getter.body = body;

		remapGetterParamsToArc4(m_typeMapper, getter, solParamTypes, loc);

		prependNonPayableCheck(getter); // getters are always view/non-payable

		contract->methods.push_back(std::move(getter));
	});
}

} // namespace puyasol::builder
