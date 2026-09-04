#include "builder/contract/ContractBuilder.h"
#include "builder/abi/EvmAbiDecode.h"
#include "builder/sol-types/SolIntType.h"
#include "builder/assembly/AssemblyBuilder.h"
#include "builder/contract/PostInitTriggers.h"
#include "builder/contract/SelectorRouter.h"
#include "builder/contract/StateVarWalker.h"
#include "builder/sol-ast/calls/SolNewExpression.h"
#include "builder/sol-ast/stmts/SolBlock.h"
#include "builder/itxn/FunctionPointerBuilder.h"
#include "builder/sol-ast/EvmSlotLowering.h"
#include "builder/sol-types/Arc4Defaults.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/storage/EvmLayoutMode.h"
#include "Logger.h"

#include <libsolidity/ast/ASTVisitor.h>

#include <boost/multiprecision/cpp_int.hpp>
#include <map>
#include <set>
// yul nodes BY VALUE (the AST aliases are std::variant, which needs
// complete types). Kept out of AssemblyBuilder.h so only the TUs that
// actually instantiate them pay the ~223k lines.
#include <libyul/AST.h>
#include <libyul/Dialect.h>

namespace puyasol::builder
{

/// buildApprovalProgram phase: slot-mode state-var init — the slot space zero-initialises for free (absent box = 0); only explicit …
void ContractBuilder::emitSlotModeStateVarInit(
	solidity::frontend::VariableDeclaration const& _var,
	std::vector<std::shared_ptr<awst::Statement>>& targetBody,
	awst::SourceLocation const& loc)
{
	auto const* var = &_var;
	if (!var->value())
		return;
	auto const* t = var->type();
	if (t && !t->isValueType())
	{
		// aggregate initializer: build the value and hand it
		// to the aggregate writers (array / struct / bytes)
		sol_ast::EvmSlotLowering aggLow(
			*m_exprBuilder, *m_exprBuilder->currentScope,
			loc);
		auto aggAddr = aggLow.addrForStateVar(*var);
		auto aggVal = aggAddr
			? m_exprBuilder->buildExpr(*var->value()) : nullptr;
		bool done = false;
		if (aggAddr && aggVal)
		{
			aggAddr->solType = t;
			aggVal = TypeCoercion::coerceForAssignment(
				std::move(aggVal), aggAddr->wtype,
				loc);
			std::vector<std::shared_ptr<awst::Statement>> aggOut;
			if (sol_ast::EvmSlotLowering::isBytesLike(t))
			{
				std::shared_ptr<awst::Expression> bv =
					std::move(aggVal);
				if (bv->wtype
					&& bv->wtype->kind() != awst::WTypeKind::Bytes
					&& bv->wtype != awst::WType::stringType())
					bv = awst::makeARC4Decode(std::move(bv),
						awst::WType::bytesType(),
						loc);
				aggLow.writeBytesValue(*aggAddr, std::move(bv),
					aggOut);
				done = true;
			}
			else if (auto const* iat =
				dynamic_cast<solidity::frontend::ArrayType const*>(t))
				done = aggLow.writeArrayValue(
					*aggAddr, iat, std::move(aggVal), aggOut);
			else if (dynamic_cast<
				solidity::frontend::StructType const*>(t))
				done = aggLow.writeStructValue(
					*aggAddr, std::move(aggVal), aggOut);
			if (done)
			{
				for (auto& preStmt: m_exprBuilder->takePreEffects())
					targetBody.push_back(std::move(preStmt));
				for (auto& postStmt: m_exprBuilder->takePostEffects())
					targetBody.push_back(std::move(postStmt));
				for (auto& st3: aggOut)
					targetBody.push_back(std::move(st3));
			}
		}
		if (!done)
			Logger::instance().error(
				"--evm-storage-layout: aggregate state initializer "
				"not yet supported for '" + var->name() + "'",
				loc);
		return;
	}
	if (!t)
	{
		return;
	}
	sol_ast::EvmSlotLowering low(
		*m_exprBuilder, *m_exprBuilder->currentScope,
		loc);
	auto addr = low.addrForStateVar(*var);
	if (!addr)
		return;
	auto initVal = m_exprBuilder->buildExpr(*var->value());
	if (!initVal)
		return;
	initVal = TypeCoercion::coerceForAssignment(
		std::move(initVal), addr->wtype, loc);
	for (auto& preStmt: m_exprBuilder->takePreEffects())
		targetBody.push_back(std::move(preStmt));
	for (auto& postStmt: m_exprBuilder->takePostEffects())
		targetBody.push_back(std::move(postStmt));
	std::vector<std::shared_ptr<awst::Statement>> writes;
	low.writeValue(*addr, std::move(initVal), writes);
	for (auto& st: writes)
		targetBody.push_back(std::move(st));
	return;
}

/// buildApprovalProgram phase: state variable initialization for one contract level.
void ContractBuilder::emitStateVarInitFor(
	solidity::frontend::ContractDefinition const& base,
	std::vector<std::shared_ptr<awst::Statement>>& targetBody,
	std::set<int64_t>& stateVarInitialized,
	awst::SourceLocation const& loc)
{
	for (auto const* var: base.stateVariables())
	{
		if (var->isConstant())
			continue;
		if (stateVarInitialized.count(var->id()))
			continue;
		stateVarInitialized.insert(var->id());

		// --evm-storage-layout: slot space zero-initialises for free
		// (absent box = 0); only explicit initializers need a write.
		// Immutables keep their named cells (they are not in EVM
		// storage) and fall through to the existing path.
			if (m_typeMapper.profile().evmStorageLayout && !var->immutable()
			&& var->referenceLocation()
				!= solidity::frontend::VariableDeclaration::Location::Transient)
		{
			emitSlotModeStateVarInit(*var, targetBody, loc);
			continue;
		}

	auto binding = m_storageMapper.physicalBindingFor(*var);
		auto kind = binding.kind;

		auto* wtype = m_typeMapper.map(var->type());

		// Box ARC4 struct with explicit initializer: encode + box_put.
		// Dynamic arrays/bytes handled by m_boxArrayVars loop; skip here.
		if (kind == awst::AppStorageKind::Box)
		{
			if (!var->value())
				continue;
			bool isStructBox = wtype
				&& wtype->kind() == awst::WTypeKind::ARC4Struct;
			if (!isStructBox)
				continue;
			auto initVal = m_exprBuilder->buildExpr(*var->value());
			if (!initVal)
				continue;
			initVal = TypeCoercion::coerceForAssignment(
				std::move(initVal), wtype, loc);
			for (auto& preStmt: m_exprBuilder->takePreEffects())
				targetBody.push_back(std::move(preStmt));
			for (auto& postStmt: m_exprBuilder->takePostEffects())
				targetBody.push_back(std::move(postStmt));
			auto boxKey = awst::makeUtf8BytesConstant(
				binding.name, loc);
			auto put = awst::makeIntrinsicCall(
				"box_put", awst::WType::voidType(), loc);
			put->stackArgs.push_back(std::move(boxKey));
			put->stackArgs.push_back(std::move(initVal));
			targetBody.push_back(awst::makeExpressionStatement(
				std::move(put), loc));
			continue;
		}

		if (kind != awst::AppStorageKind::AppGlobal)
			continue;

		auto key = awst::makeUtf8BytesConstant(binding.name, loc);

		std::shared_ptr<awst::Expression> defaultVal;
		if (var->value())
		{
			// Pre-write zero so self-referencing immutable initializers
			// (`uint immutable x = x + 1`) read 0 via app_global_get_ex.
			// Non-immutable vars get zero from the fall-through below.
			if (var->immutable())
			{
				std::shared_ptr<awst::Expression> zeroVal;
				if (wtype == awst::WType::accountType())
					zeroVal = awst::makeAddressConstant(
						"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAY5HFKQ",
						loc);
				else if (wtype == awst::WType::biguintType())
					zeroVal = awst::makeZero(loc, awst::WType::biguintType());
				else if (wtype == awst::WType::boolType() || wtype == awst::WType::uint64Type())
					zeroVal = awst::makeZero(loc);
				else
					zeroVal = StorageMapper::makeDefaultValue(wtype, loc);
				auto preKey = awst::makeUtf8BytesConstant(
					binding.name, loc);
				auto prePut = awst::makeAppGlobalPut(
					preKey, std::move(zeroVal), loc);
				targetBody.push_back(
					awst::makeExpressionStatement(std::move(prePut), loc));
			}

			defaultVal = m_exprBuilder->buildExpr(*var->value());
			if (defaultVal)
				defaultVal = TypeCoercion::coerceForAssignment(
					std::move(defaultVal), wtype, loc);
			// Flush pre-effects (e.g. new C() inner-txn create+fund)
			// before the state-var assignment uses __new_app_id_N.
			for (auto& preStmt: m_exprBuilder->takePreEffects())
				targetBody.push_back(std::move(preStmt));
			for (auto& postStmt: m_exprBuilder->takePostEffects())
				targetBody.push_back(std::move(postStmt));
		}
		if (!defaultVal)
		{
		if (wtype == awst::WType::accountType())
			defaultVal = awst::makeAddressConstant(
				"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAY5HFKQ",
				loc);
		else if (wtype == awst::WType::biguintType())
		{
			auto val = awst::makeZero(loc, awst::WType::biguintType());
			defaultVal = val;
		}
		else if (wtype == awst::WType::boolType()
			|| wtype == awst::WType::uint64Type())
		{
			auto val = awst::makeZero(loc);
			defaultVal = val;
		}
		else if (wtype->kind() == awst::WTypeKind::ReferenceArray
			|| wtype->kind() == awst::WTypeKind::ARC4StaticArray
			|| wtype->kind() == awst::WTypeKind::ARC4DynamicArray)
		{
			defaultVal = StorageMapper::makeDefaultValue(wtype, loc);
		}
		else if (wtype->kind() == awst::WTypeKind::ARC4Struct
			|| wtype->kind() == awst::WTypeKind::WTuple)
		{
			defaultVal = StorageMapper::makeDefaultValue(wtype, loc);
		}
		else
		{
			// bytes1..bytes32: N zero bytes so the auto-getter ABI emits the
			// declared width. Dynamic bytes/string keep the empty default.
			int bytesLen = 0;
			if (auto const* bw = dynamic_cast<awst::BytesWType const*>(wtype))
				if (bw->length().has_value() && *bw->length() > 0)
					bytesLen = static_cast<int>(*bw->length());
			defaultVal = awst::makeBytesConstant(
				std::vector<uint8_t>(static_cast<size_t>(bytesLen), 0),
				loc,
				awst::BytesEncoding::Base16,
				wtype && wtype->kind() == awst::WTypeKind::Bytes
					? wtype : awst::WType::bytesType());
		}
		} // end if (!defaultVal)

		// app_global_put(key, defaultVal)
		auto put = awst::makeAppGlobalPut(key, defaultVal, loc);

		auto stmt = awst::makeExpressionStatement(put, loc);
		targetBody.push_back(stmt);
	}
}

namespace
{
/// True when a DYNAMIC bool array (`bool[]`) is reachable inside a storage
/// type (directly, or through mapping values / struct members / outer
/// arrays). That exact shape is puyabug.md #10: puya's box-backed
/// arc4.dynamic_array<arc4.bool> lowerings DISAGREE on packing granularity —
/// append writes one BYTE per element, IndexExpression reads getbit —
/// so push(true);push(true) reads back [true, false]: silent wrong data.
/// The granularity is chosen inside puya's ArrayExtend lowering (no frontend
/// channel). Fixed bool[N] whole-array init/reads are consistent (element
/// indexing already fails loud at runtime) and stay allowed.
bool reachesDynamicBoolArray(solidity::frontend::Type const* _t, int _depth = 0)
{
	using namespace solidity::frontend;
	if (!_t || _depth > 16)
		return false;
	if (auto const* at = dynamic_cast<ArrayType const*>(_t))
	{
		if (at->isDynamicallySized() && at->baseType()
			&& at->baseType()->category() == Type::Category::Bool)
			return true;
		return reachesDynamicBoolArray(at->baseType(), _depth + 1);
	}
	if (auto const* mt = dynamic_cast<MappingType const*>(_t))
		return reachesDynamicBoolArray(mt->valueType(), _depth + 1);
	if (auto const* st = dynamic_cast<StructType const*>(_t))
	{
		for (auto const& member: st->structDefinition().members())
			if (member && reachesDynamicBoolArray(member->type(), _depth + 1))
				return true;
	}
	return false;
}
} // anonymous namespace

/// buildApprovalProgram phase: collect box-stored array/bytes vars for box_create in __postInit (m_boxArrayVars).
void ContractBuilder::collectBoxArrayVars(
	solidity::frontend::ContractDefinition const& _contract,
	awst::SourceLocation const& loc)
{
	std::set<int64_t> lengthInitialized;
	forEachStateVarReverse(_contract, [&](auto const* var)
	{
		if (var->isConstant())
			return;
		// puyabug.md #10 gate (default storage mode only): storage `bool[]`
		// silently reads wrong values back — fail loud with the workaround.
		// Slot mode uses puya-sol's own byte-consistent lowering (verified
		// against solc raw slot words) and is unaffected.
		if (!m_typeMapper.profile().evmStorageLayout
			&& reachesDynamicBoolArray(var->type()))
			Logger::instance().error(
				"storage `bool[]` is unsupported in the default storage mode: "
				"puya's box-backed bool-array append and read disagree on "
				"packing (puyabug.md #10) — push(true);push(true) reads back "
				"[true, false]. Compile with --evm-storage-layout "
				"(byte-consistent "
				"slot storage), or use uint8[]/bool[N].",
				loc);
		if (lengthInitialized.count(var->id()))
			return;

		auto binding = m_storageMapper.physicalBindingFor(*var);
		auto kind = binding.kind;

		if (kind != awst::AppStorageKind::Box)
			return;

		auto* wtype = m_typeMapper.map(var->type());
		if (!wtype)
			return;
		// Dynamic arrays, dynamic bytes, and ARC4 static arrays all need
		// box_create at deploy time ("no such box" otherwise).
		bool isBoxType = wtype->kind() == awst::WTypeKind::ReferenceArray
			|| wtype->kind() == awst::WTypeKind::ARC4DynamicArray
			|| wtype->kind() == awst::WTypeKind::ARC4StaticArray
			|| awst::isDynamicBytes(wtype);   // covers the bytesType() singleton too
		if (!isBoxType)
			return;

		// ARC4StaticArray: oversized → multi-box layout (N boxes keyed
		// `<name>++itob(page)`). AVM single-box cap = 32768 B;
		// page = idx / elemsPerBox at runtime.
		if (wtype->kind() == awst::WTypeKind::ARC4StaticArray)
		{
			auto const* sa = dynamic_cast<awst::ARC4StaticArray const*>(wtype);
			if (sa && sa->arraySize() > 0)
			{
				uint64_t totalBytes = StorageMapper::arc4StaticArrayTotalBytes(wtype);
				// Cap pre-allocation at 4 boxes (128 KB). Beyond that,
				// __postInit box_create burst exceeds write-budget (~8
				// box_create per app call). .length reads still work
				// (compile-time constant); element writes on
				// un-pre-allocated arrays fail — see multi-box-storage.md.
				// totalBytes==0 (struct/dynamic-element) falls through to
				// single-box path.
				constexpr uint64_t MAX_PREALLOC_BYTES = 4ULL * 32768ULL;
				if (totalBytes > MAX_PREALLOC_BYTES)
				{
					Logger::instance().warning(
						"state array '" + var->name() + "' has declared size "
						+ std::to_string(sa->arraySize())
						+ " which exceeds 4-box (128 KB) pre-allocation cap — skipping box_create. "
						"Element writes will fail at runtime but .length reads "
						"still return the declared size.",
						loc);
					return;
				}
			}
		}

		lengthInitialized.insert(var->id());
		// Dynamic array boxes are created in __postInit (after funding)
		// Length is derived from box_len / element_size (no separate counter)
		m_boxArrayVars.push_back(var);
	});
}

/// buildApprovalProgram phase: decode constructor params from ApplicationArgs into the create block (EVM single-blob or ARC4 …
void ContractBuilder::emitCtorParamDecode(
	solidity::frontend::FunctionDefinition const& _constructor,
	std::shared_ptr<awst::Block> const& createBlock,
	bool needsPostInit,
	awst::SourceLocation const& loc)
{
	auto const* constructor = &_constructor;
	if (m_typeMapper.profile().contractAbi == ContractAbi::Evm
		&& !needsPostInit
		&& !constructor->parameters().empty())
	{
		std::vector<solidity::frontend::Type const*> parameterTypes;
		std::vector<awst::WType const*> mappedTypes;
		for (auto const& parameter: constructor->parameters())
		{
			parameterTypes.push_back(parameter->type());
			mappedTypes.push_back(m_typeMapper.map(parameter->type()));
		}
		auto const* decodedType = mappedTypes.size() == 1
			? mappedTypes[0]
			: m_typeMapper.createType<awst::WTuple>(mappedTypes);
		auto decoded = abi::decodeEvmAbi(
			m_typeMapper, awst::makeAppArg(0, loc),
			parameterTypes, decodedType, loc,
			createBlock->body);
		decoded = awst::makeEvalOnce(std::move(decoded), loc);
		for (size_t i = 0; i < constructor->parameters().size(); ++i)
		{
			auto value = constructor->parameters().size() == 1
				? decoded
				: awst::makeTupleItem(decoded, static_cast<int>(i),
					mappedTypes[i], loc);
			createBlock->body.push_back(awst::makeAssignmentStatement(
				awst::makeVarExpression(constructor->parameters()[i]->name(),
					mappedTypes[i], loc),
				std::move(value), loc));
		}
	}
	else if (m_typeMapper.profile().contractAbi == ContractAbi::Arc4)
	{
	// Decode constructor params from ApplicationArgs (ARC4-encoded, one per slot).
	int argIndex = 0;
	for (auto const& param: constructor->parameters())
	{
		auto* paramType = m_typeMapper.map(param->type());

		// txna ApplicationArgs i → raw ARC4 bytes
		auto readArg = awst::makeAppArg(argIndex, loc);

		std::shared_ptr<awst::Expression> paramVal;

		if (paramType == awst::WType::accountType())
		{
			auto cast = awst::makeAsAccount(std::move(readArg), loc);
			paramVal = std::move(cast);
		}
		else if (paramType == awst::WType::biguintType())
		{
			auto cast = awst::makeAsBiguint(std::move(readArg), loc);
			paramVal = std::move(cast);
		}
		else if (paramType == awst::WType::uint64Type()
			|| paramType == awst::WType::boolType())
		{
			// Args are 32-byte big-endian (EVM ABI); extract last 8 + btoi.
			auto len = awst::makeLen(readArg, loc);

			auto eight = awst::makeIntegerConstant("8", loc);

			auto offset = awst::makeUInt64BinOp(std::move(len), awst::UInt64BinaryOperator::Sub, eight, loc);

			auto eight2 = awst::makeIntegerConstant("8", loc);
			auto extract = awst::makeExtract3(
				std::move(readArg), std::move(offset), std::move(eight2),
				loc);

			paramVal = awst::makeBtoi(
				std::move(extract), loc, paramType);
		}
		else if (paramType == awst::WType::stringType())
		{
			auto cast = awst::makeReinterpretCast(std::move(readArg), awst::WType::stringType(), loc);
			paramVal = std::move(cast);
		}
		else if (paramType->kind() == awst::WTypeKind::ReferenceArray)
		{
			auto const* arc4Type = m_typeMapper.mapToARC4Type(paramType);
			auto cast = awst::makeReinterpretCast(std::move(readArg), arc4Type, loc);

			auto const* refArr = dynamic_cast<awst::ReferenceArray const*>(paramType);
			if (refArr && !refArr->arraySize().has_value())
				paramVal = awst::makeConvertArray(std::move(cast), paramType, loc);
			else
			{
				auto decode = awst::makeARC4Decode(std::move(cast), paramType, loc);
				paramVal = std::move(decode);
			}
		}
		else if (paramType->kind() == awst::WTypeKind::ARC4StaticArray
			|| paramType->kind() == awst::WTypeKind::ARC4DynamicArray)
		{
			auto cast = awst::makeReinterpretCast(std::move(readArg), paramType, loc);
			paramVal = std::move(cast);
		}
		else if (awst::fixedBytesLength(paramType).has_value())
		{
			auto cast = awst::makeReinterpretCast(std::move(readArg), paramType, loc);
			paramVal = std::move(cast);
		}
		else if (dynamic_cast<awst::ARC4Struct const*>(paramType))
		{
			auto cast = awst::makeReinterpretCast(std::move(readArg), paramType, loc);
			paramVal = std::move(cast);
		}
		else
		{
			paramVal = std::move(readArg);
		}

		auto target = awst::makeVarExpression(param->name(), paramType, loc);

		auto assignment = awst::makeAssignmentStatement(target, std::move(paramVal), loc);
		createBlock->body.push_back(std::move(assignment));

		++argIndex;
	}
	}

}

/// buildApprovalProgram phase: solc pre-populates baseConstructorArguments (InheritanceSpecifier or ModifierInvocation → args) — no …
std::map<solidity::frontend::ContractDefinition const*,
	std::vector<solidity::frontend::ASTPointer<solidity::frontend::Expression>> const*>
ContractBuilder::collectExplicitBaseArgs(
	solidity::frontend::ContractDefinition const& _contract)
{
	std::map<solidity::frontend::ContractDefinition const*,
		std::vector<solidity::frontend::ASTPointer<solidity::frontend::Expression>> const*>
		explicitBaseArgs;
	for (auto const& [baseCtor, argNode] : _contract.annotation().baseConstructorArguments)
	{
		auto const* baseContract = baseCtor->annotation().contract;
		if (!baseContract) continue;
		std::vector<solidity::frontend::ASTPointer<solidity::frontend::Expression>> const* args = nullptr;
		if (auto const* mod = dynamic_cast<solidity::frontend::ModifierInvocation const*>(argNode))
			args = mod->arguments();
		else if (auto const* spec = dynamic_cast<solidity::frontend::InheritanceSpecifier const*>(argNode))
			args = spec->arguments();
		if (args && !args->empty())
			explicitBaseArgs[baseContract] = args;
	}
	return explicitBaseArgs;
}

/// buildApprovalProgram phase: bind one base constructor's explicit args to its params in the create block — slot-mode storage refs …
void ContractBuilder::bindBaseCtorArgs(
	solidity::frontend::FunctionDefinition const& baseCtor,
	std::vector<solidity::frontend::ASTPointer<solidity::frontend::Expression>> const& args,
	std::shared_ptr<awst::Block> const& createBlock)
{
	auto const& params = baseCtor.parameters();
	for (size_t i = 0; i < args.size() && i < params.size(); ++i)
	{
		if (params[i]->referenceLocation()
			== solidity::frontend::VariableDeclaration::Location::Storage)
		{
			if (m_typeMapper.profile().evmStorageLayout)
			{
				sol_ast::EvmSlotLowering low(
					*m_exprBuilder, *m_exprBuilder->currentScope,
					makeLoc(args[i]->location()));
				if (auto addr = low.resolve(*args[i]))
				{
					for (auto& pst: m_exprBuilder->takePreEffects())
						createBlock->body.push_back(std::move(pst));
					createBlock->body.push_back(awst::makeAssignmentStatement(
						awst::makeVarExpression(params[i]->name(),
							awst::WType::biguintType(), makeLoc(args[i]->location())),
						addr->slot, makeLoc(args[i]->location())));
				}
				continue;
			}
		}
		auto argExpr = m_exprBuilder->buildExpr(*args[i]);
		if (!argExpr)
			continue;
		if (params[i]->referenceLocation()
			== solidity::frontend::VariableDeclaration::Location::Storage)
		{
			sol_ast::StorageAlias alias =
						sol_ast::StorageAlias::classify(std::move(argExpr));
			m_tr->setStorageAlias(params[i]->id(), std::move(alias));
			continue;
		}
		auto* targetType = m_typeMapper.map(params[i]->type());
		argExpr = TypeCoercion::implicitNumericCast(
			std::move(argExpr), targetType, makeLoc(args[i]->location()));

		// Drain the build's pre-statements (ternary/short-circuit temp
			// assignments) BEFORE the param binding — same fix as the
			// modifier-chain argument path, which this site missed.
		m_exprBuilder->appendEffectsTo(createBlock->body);

		auto target = awst::makeVarExpression(params[i]->name(), targetType, makeLoc(args[i]->location()));

		auto assignment = awst::makeAssignmentStatement(target, std::move(argExpr), target->sourceLocation);
		createBlock->body.push_back(std::move(assignment));
	}
}

/// buildApprovalProgram phase: the no-postInit path — inline base ctor bodies + the main ctor into the bool-returning approval …
void ContractBuilder::emitInlineCtorPath(
	solidity::frontend::ContractDefinition const& _contract,
	solidity::frontend::FunctionDefinition const* constructor,
	awst::ContractMethod& method,
	std::shared_ptr<awst::Block> const& createBlock,
	std::map<solidity::frontend::ContractDefinition const*,
		std::vector<solidity::frontend::ASTPointer<solidity::frontend::Expression>> const*>
		const& explicitBaseArgs,
	std::set<int64_t>& stateVarInitialized)
{
	// Inline ctor into the bool-returning approval program.
	// Assembly return() must emit bool (AssemblyBuilder::handleReturn when
	// m_returnType is bool) — set returnType accordingly.
	auto const* savedReturnType = m_functionCtx->returnType;
	m_functionCtx->returnType = awst::WType::boolType();

	// Legacy (compileViaYul:false): all state var inits before any ctor arg eval.
	// `constructor_inheritance_init_order_3_legacy`: A's `uint x = 2` runs first,
	// THEN B's `A(f())` evaluates f() (sets x=4) — final x=4. emitStateVarInitFor
	// deduplicates via stateVarInitialized so the interleaved loop below is safe.
	// viaIR: keep interleaved order (derived inits observe base ctor state).
	if (!m_viaIR)
	{
		auto const& linEarly = _contract.annotation().linearizedBaseContracts;
		for (auto itEarly = linEarly.rbegin(); itEarly != linEarly.rend(); ++itEarly)
			emitStateVarInitFor(**itEarly, createBlock->body,
				stateVarInitialized, method.sourceLocation);
	}

	// Pre-evaluate ctor args in dependency order (viaIR only).
	// For D→C→A, C's params must be assigned first so A's args (from C's modifier)
	// see C's param values. Phase 1: direct args. Phase 2: transitive args.
	std::map<solidity::frontend::ContractDefinition const*,
		std::vector<std::shared_ptr<awst::Expression>>> preEvaluatedArgs;
	{
		// Direct bases: ModifierInvocation on derived ctor or InheritanceSpecifier
		// on the contract itself.
		std::vector<solidity::frontend::ContractDefinition const*> directBases;
		std::set<int64_t> seenDirectBaseIds;
		auto recordBase = [&](solidity::frontend::Declaration const* _ref) {
			if (auto const* bc = dynamic_cast<solidity::frontend::ContractDefinition const*>(_ref))
				if (seenDirectBaseIds.insert(bc->id()).second)
					directBases.push_back(bc);
		};
		if (constructor)
			for (auto const& mod: constructor->modifiers())
				recordBase(mod->name().annotation().referencedDeclaration);
		for (auto const& baseSpec: _contract.baseContracts())
			recordBase(baseSpec->name().annotation().referencedDeclaration);

		// Phase 1: Assign direct base ctor params into createBlock
		// (so transitive args can reference them)
		for (auto const* directBase: directBases)
		{
			auto argIt = explicitBaseArgs.find(directBase);
			if (argIt == explicitBaseArgs.end() || !argIt->second || argIt->second->empty())
				continue;
			auto const* baseCtor = directBase->constructor();
			if (!baseCtor)
				continue;

			bindBaseCtorArgs(*baseCtor, *(argIt->second), createBlock);
		}

		// Phase 2: Transitive args in derived-first order so intermediates are
		// assigned before deeper transitives reference them.
		// E.g. Final→Derived→Base1→Base: assign Base1.k first (from Derived.i),
		// then evaluate Base.j (from Base1.k).
		auto const& lin = _contract.annotation().linearizedBaseContracts;
		for (auto it = lin.begin(); it != lin.end(); ++it)
		{
			auto const* base = *it;
			if (base == &_contract)
				continue;
			if (seenDirectBaseIds.count(base->id()))
				continue;

			auto argIt = explicitBaseArgs.find(base);
			if (argIt == explicitBaseArgs.end() || !argIt->second || argIt->second->empty())
				continue;
			auto const* baseCtor = base->constructor();
			if (!baseCtor)
				continue;

			// Assign these params into createBlock NOW (so deeper transitives can see them)
			bindBaseCtorArgs(*baseCtor, *(argIt->second), createBlock);

			// Mark these params as pre-evaluated (empty vector = already assigned)
			preEvaluatedArgs[base] = {};
		}
	}

	// Interleave state var init with ctor bodies (base-first MRO order) so
	// derived initializers (e.g. `uint y = f()`) observe base ctor state (viaIR).
	auto const& linearized = _contract.annotation().linearizedBaseContracts;
	for (auto it = linearized.rbegin(); it != linearized.rend(); ++it)
	{
		auto const* base = *it;

		emitStateVarInitFor(*base, createBlock->body,
			stateVarInitialized, method.sourceLocation);

		if (base == &_contract)
			continue; // Main ctor handled separately below

		auto const* baseCtor = base->constructor();
		if (!baseCtor || !baseCtor->isImplemented())
			continue;
		if (baseCtor->body().statements().empty())
			continue;

		// Direct base params were assigned in Phase 1; transitive use pre-evaluated.
		auto preIt = preEvaluatedArgs.find(base);
		if (preIt != preEvaluatedArgs.end())
		{
			auto const& evaledArgs = preIt->second;
			auto const& params = baseCtor->parameters();
			for (size_t i = 0; i < evaledArgs.size() && i < params.size(); ++i)
			{
				if (!evaledArgs[i])
					continue;

				auto target = awst::makeVarExpression(params[i]->name(), m_typeMapper.map(params[i]->type()), method.sourceLocation);

				auto assignment = awst::makeAssignmentStatement(target, evaledArgs[i], method.sourceLocation);
				createBlock->body.push_back(std::move(assignment));
			}
		}

		// Translate the base constructor body and lower its modifiers as calls.
		m_functionCtx->inConstructor = true;
		m_functionCtx->callableId = baseCtor->id();
		auto baseBody = buildBlock(baseCtor->body());
		m_functionCtx->inConstructor = false;
		m_functionCtx->callableId = 0;
		buildConstructorModifierChain(*baseCtor, baseBody, _contract.name());
		for (auto& stmt: baseBody->body)
			createBlock->body.push_back(std::move(stmt));
	}

	if (constructor && constructor->body().statements().size() > 0)
	{
		// Restore super targets (super.f() in ctor body) + per-ctor MRO overrides.
		for (auto const& [id, name]: m_allSuperTargetNames)
			m_tr->setSuperTarget(id, name);
		{
			auto pfit = m_perFuncSuperOverrides.find(constructor->id());
			if (pfit != m_perFuncSuperOverrides.end())
				for (auto const& [targetId, superName]: pfit->second)
					m_tr->setSuperTarget(targetId, superName);
		}
		m_tr->setInConstructor(true);
		m_functionCtx->inConstructor = true;
		m_functionCtx->callableId = constructor->id();
		auto ctorBody = buildBlock(constructor->body());
		m_functionCtx->inConstructor = false;
		m_functionCtx->callableId = 0;
		buildConstructorModifierChain(*constructor, ctorBody, _contract.name());
		m_tr->setInConstructor(false);
		m_tr->clearSuperTargets();
		for (auto& stmt: ctorBody->body)
			createBlock->body.push_back(std::move(stmt));
	}
	m_functionCtx->returnType = savedReturnType;
}

/// buildApprovalProgram phase: init the transient-storage blob (transient scratch slot) BEFORE the create/dispatch split so the …
void ContractBuilder::emitTransientBlobInit(
	awst::Block& body, awst::SourceLocation const& loc)
{
{
	unsigned blobBytes = m_transientStorage.blobSize();
	if (blobBytes < AssemblyBuilder::SLOT_SIZE)
		blobBytes = AssemblyBuilder::SLOT_SIZE;

	auto storeOp = awst::makeStoreSlot(
		m_typeMapper.profile().scratchLayout.transientSlot(),
		awst::makeBzero(blobBytes, loc),
		loc);

	auto exprStmt = awst::makeExpressionStatement(std::move(storeOp), loc);
	body.body.push_back(std::move(exprStmt));
}

}

/// buildApprovalProgram phase: init EVM memory blobs BEFORE the create/dispatch split so ctor body's `T memory t;` locals (FMP …
void ContractBuilder::emitMemoryBlobInit(
	awst::Block& body, awst::SourceLocation const& loc)
{
{
	auto const& scratch = m_typeMapper.profile().scratchLayout;
	for (int s = scratch.memoryFirst(); s <= scratch.memoryLast(); ++s)
	{
		auto storeOp = awst::makeStoreSlot(
			s,
			awst::makeBzero(ScratchLayout::slotSize, loc),
			loc);
		body.body.push_back(awst::makeExpressionStatement(std::move(storeOp), loc));
	}

	// Write the free memory pointer (FMP) at offset 0x40 = 0x80.
	auto loadBlob = awst::makeLoadSlot(
		scratch.memoryFirst(), loc);

	auto fmpOffset = awst::makeIntegerConstant("64", loc); // 0x40

	std::vector<uint8_t> fmpBytesVal(31, 0);
	fmpBytesVal.push_back(0x80);
	auto fmpBytes = awst::makeBytesConstant(
		std::move(fmpBytesVal), loc, awst::BytesEncoding::Unknown);

	auto replaceOp = awst::makeReplace3(std::move(loadBlob), std::move(fmpOffset), std::move(fmpBytes), loc);
	auto storeFmpOp = awst::makeStoreSlot(
		scratch.memoryFirst(), std::move(replaceOp), loc);

	auto fmpStmt = awst::makeExpressionStatement(std::move(storeFmpOp), loc);
	body.body.push_back(std::move(fmpStmt));
}

}

awst::ContractMethod ContractBuilder::buildApprovalProgram(
	solidity::frontend::ContractDefinition const& _contract,
	std::string const& _contractName
)
{
	awst::ContractMethod method;
	method.sourceLocation = makeLoc(_contract.location());
	method.returnType = awst::WType::boolType();
	method.cref = m_sourceFile + "." + _contractName;
	method.memberName = "approval_program";

	auto body = awst::makeBlock(method.sourceLocation);

	// __postInit triggers: box writes, new C(), msg.*, or AVM stdlib calls.
	bool needsPostInit = computeNeedsPostInit(_contract, m_storageMapper);

	// Create-time check: if (Txn.ApplicationID == 0) { base_ctors; ctor_body; return true; }
	{
		auto appIdCheck = awst::makeTxn(std::string("ApplicationID"), awst::WType::uint64Type(), method.sourceLocation);

		auto zero = awst::makeZero(method.sourceLocation);

		auto isCreate = awst::makeNumericCompare(appIdCheck, awst::NumericComparison::Eq, zero, method.sourceLocation);

		auto createBlock = awst::makeBlock(method.sourceLocation);

		// One dedup set spans every state-var emission site (legacy pass,
		// interleaved pass, __postInit).
		std::set<int64_t> stateVarInitialized;
		auto emitStateVarInit = [&](solidity::frontend::ContractDefinition const& base,
			std::vector<std::shared_ptr<awst::Statement>>& targetBody)
		{
			emitStateVarInitFor(base, targetBody, stateVarInitialized, method.sourceLocation);
		};

		// --evm-storage-layout: no named boxes exist; slot pages materialise
		// lazily on first write.
		if (!m_typeMapper.profile().evmStorageLayout)
			collectBoxArrayVars(_contract, method.sourceLocation);

		if (!m_boxArrayVars.empty())
			needsPostInit = true;

		auto const* constructor = _contract.constructor();
		if (constructor)
			emitCtorParamDecode(
				*constructor, createBlock, needsPostInit, method.sourceLocation);

		auto explicitBaseArgs = collectExplicitBaseArgs(_contract);

		if (needsPostInit)
		{
			// ~330 lines: an entire second ABI method (argument mirroring,
			// biguint->ARC4 remap, pending-flag guard, creator-only auth, MRO
			// base-ctor binding, body inlining, budget pump) synthesised inside
			// the approval-program builder. It only ever needed `method` and
			// `explicitBaseArgs` from this scope.
			buildPostInitMethod(_contract, _contractName, method, createBlock,
				explicitBaseArgs, emitStateVarInit);
		}
		else
			emitInlineCtorPath(_contract, constructor, method, createBlock,
				explicitBaseArgs, stateVarInitialized);

		// Return true to complete the create transaction
		auto createReturn = awst::makeReturnStatement(awst::makeTrue(method.sourceLocation), method.sourceLocation);
		createBlock->body.push_back(createReturn);

		emitTransientBlobInit(*body, method.sourceLocation);

		emitMemoryBlobInit(*body, method.sourceLocation);

		body->body.push_back(awst::makeIfElse(
			isCreate, createBlock, nullptr, method.sourceLocation));
	}

	// Transient vars: preamble bzero satisfies EIP-1153 per-tx reset; no
	// per-call app_global reset needed.

	// The selector dispatch (ARC-4 router + EVM compat arms) is appended by
	// ContractBuilder::build AFTER every method body exists: the EVM route
	// arms name generated methods, which are not built yet at this point.
	// Nothing else touches the approval body in between, so the emitted
	// statement order is unchanged.
	method.body = body;

	return method;
}

void ContractBuilder::emitBoxCreateForStateVars(
	awst::Block& _postInitBody,
	awst::SourceLocation const& _loc)
{
	// Create boxes for dynamic array state variables
	for (auto const* var: m_boxArrayVars)
	{
		if (!var)
			continue;
		auto const varName = m_storageMapper.physicalBindingFor(*var).name;
		auto boxKey = awst::makeUtf8BytesConstant(varName, _loc);

		// Dynamic bytes without init: box_create(size=0). Raw content has no length
		// header, so empty box = empty bytes. Required so BoxValueExpression (bare
		// box_extract path) works; old box_get→select fallback reverts on >4 KB
		// (AVM stack-value cap). See StorageMapper::makeStateGetWithDefault.
		auto const* declaredArray =
			dynamic_cast<solidity::frontend::ArrayType const*>(var->type());
		bool isDynamicBytesWithoutInit = declaredArray
			&& declaredArray->isByteArrayOrString() && !var->value();
		if (isDynamicBytesWithoutInit)
		{
			auto sizeZero = awst::makeIntegerConstant(0, _loc);
			auto boxCreate = awst::makeBoxCreate(
				std::move(boxKey), std::move(sizeZero),
				_loc);
			auto boxStmt = awst::makeExpressionStatement(
				std::move(boxCreate), _loc);
			_postInitBody.body.push_back(std::move(boxStmt));
			continue;
		}

		// boxSizeVal: 2 (ARC4 dyn-array length header), or literal size,
		// or elementSize*N for static arrays (e.g. uint[20]).
		unsigned boxSizeVal = 2; // ARC4 dynamic array length header
		std::shared_ptr<awst::Expression> boxInitVal;
		// ARC4StaticArray<dynamic T>: zeroed buffer is invalid ARC4 (head offsets
		// must exceed head). Synthesise default encoding → box_put instead.
		std::optional<std::vector<uint8_t>> dynArc4Default;
		// ARC4StaticArray (uint[N], int[N], etc.): allocate
		// elementSize * arraySize bytes so the contract can
		// write to slot indices without "no such box".
		auto* varWtype = m_typeMapper.map(var->type());
		if (varWtype && varWtype->kind() == awst::WTypeKind::ARC4StaticArray)
		{
			auto const* sa = dynamic_cast<awst::ARC4StaticArray const*>(varWtype);
			if (sa && sa->arraySize() > 0)
			{
				if (arc4IsDynamic(sa))
				{
					if (auto enc = arc4DefaultEncoding(sa))
						if (enc->size() > 0 && enc->size() <= 32768)
							dynArc4Default = std::move(*enc);
				}
				uint64_t elemSize = 32; // conservative fallback; dynamic defaults use box_put
				auto const* elemT = sa->elementType();
				if (int fixedSize = builder::computeEncodedElementSize(elemT);
					fixedSize > 0)
					elemSize = static_cast<uint64_t>(fixedSize);
				// AVM box cap = 32768 B; oversized → multi-box below.
				// Record per-box size here.
				uint64_t size = elemSize * static_cast<uint64_t>(sa->arraySize());
				if (size > 32768)
					size = 32768;
				boxSizeVal = static_cast<unsigned>(size);
			}
		}
		if (var->value())
		{
			auto const* arrType =
				dynamic_cast<solidity::frontend::ArrayType const*>(var->type());
			if (arrType && arrType->isByteArrayOrString())
			{
				if (auto const* lit = dynamic_cast<solidity::frontend::Literal const*>(
						var->value().get()))
					boxSizeVal = static_cast<unsigned>(lit->value().size());
				if (boxSizeVal > 0)
				{
					boxInitVal = m_exprBuilder->buildExpr(*var->value());
					if (boxInitVal && boxInitVal->wtype == awst::WType::stringType())
					{
						auto cast = awst::makeAsBytes(std::move(boxInitVal), _loc);
						boxInitVal = std::move(cast);
					}
				}
			}
			// Non-bytes dynamic array with initializer (e.g. `int16[] x = [-1,-2]`):
			// set boxInitVal so the loop below emits box_put instead of box_create(2).
			else if (arrType && arrType->isDynamicallySized()
				&& !arrType->isByteArrayOrString())
			{
				auto initVal = m_exprBuilder->buildExpr(*var->value());
				if (initVal)
				{
					auto* tgtWtype = m_typeMapper.map(arrType);
					initVal = TypeCoercion::coerceForAssignment(
						std::move(initVal), tgtWtype, _loc);
					// Materialise as bytes for box_put.
					if (initVal->wtype != awst::WType::bytesType())
						initVal = awst::makeAsBytes(std::move(initVal), _loc);
					boxInitVal = std::move(initVal);
				}
			}
		}

		// Multi-box detection: if the var's ARC4StaticArray total size
		// exceeds a single box's capacity, emit N box_create calls
		// keyed `<name>` ++ `itob(page)` instead of one. Element
		// reads/writes route at runtime via the same key suffix
		// scheme (see SolIndexAccessHandlers.cpp).
		unsigned multiBoxN = 0;
		unsigned multiBoxElemSize = 0;
		uint64_t multiBoxTotalBytes = 0;
		uint64_t multiBoxPerPageBytes = 0;
		if (StorageMapper::isMultiBoxArray(varWtype))
		{
			multiBoxN = StorageMapper::numBoxesForArray(varWtype);
			multiBoxElemSize = StorageMapper::arc4StaticArrayElementSize(varWtype);
			multiBoxTotalBytes = StorageMapper::arc4StaticArrayTotalBytes(varWtype);
			multiBoxPerPageBytes = static_cast<uint64_t>(
				StorageMapper::elementsPerBox(varWtype)) * multiBoxElemSize;
		}

		if (multiBoxN > 1 && multiBoxElemSize > 0 && !dynArc4Default && !boxInitVal)
		{
			// Multi-box: N box_create calls, key = name++itob(page).
			for (unsigned page = 0; page < multiBoxN; ++page)
			{
				auto nameBytes = awst::makeUtf8BytesConstant(varName, _loc);
				auto pageInt = awst::makeIntegerConstant(page, _loc);
				auto pageItob = awst::makeItob(std::move(pageInt), _loc);
				auto pageKey = awst::makeConcat(std::move(nameBytes), std::move(pageItob), _loc);

				uint64_t pageSize = (page == multiBoxN - 1)
					? (multiBoxTotalBytes - static_cast<uint64_t>(page) * multiBoxPerPageBytes)
					: multiBoxPerPageBytes;
				auto pageSizeExpr = awst::makeIntegerConstant(pageSize, _loc);

				auto boxCreate = awst::makeBoxCreate(
					std::move(pageKey), std::move(pageSizeExpr),
					_loc);

				auto boxStmt = awst::makeExpressionStatement(std::move(boxCreate), _loc);
				_postInitBody.body.push_back(std::move(boxStmt));
			}
		}
		else if (dynArc4Default)
		{
			// box_put creates + initialises with valid ARC4 head/tail in one op.
			auto put = awst::makeBoxPut(std::move(boxKey), awst::makeBytesConstant(
				std::move(*dynArc4Default), _loc), _loc);
			auto putStmt = awst::makeExpressionStatement(std::move(put), _loc);
			_postInitBody.body.push_back(std::move(putStmt));
		}
		else
		{
			// Non-bytes dyn-array init: encoded length ≠ header boxSizeVal=2;
			// box_put can't grow a pre-created box → skip box_create, let box_put
			// create at the right size.
			bool isNonBytesDynArrInit = var->value() && declaredArray
				&& declaredArray->isDynamicallySized()
				&& !declaredArray->isByteArrayOrString();

			if (!isNonBytesDynArrInit)
			{
				auto boxSize = awst::makeIntegerConstant(boxSizeVal, _loc);

				auto boxCreate = awst::makeBoxCreate(
					std::move(boxKey), std::move(boxSize),
					_loc);

				auto boxStmt = awst::makeExpressionStatement(std::move(boxCreate), _loc);
				_postInitBody.body.push_back(std::move(boxStmt));
			}

			if (boxInitVal)
			{
				auto putKey = awst::makeUtf8BytesConstant(varName, _loc);
				auto put = awst::makeBoxPut(std::move(putKey), std::move(boxInitVal), _loc);
				auto putStmt = awst::makeExpressionStatement(std::move(put), _loc);
				_postInitBody.body.push_back(std::move(putStmt));
			}
		}
	}
}

} // namespace puyasol::builder
