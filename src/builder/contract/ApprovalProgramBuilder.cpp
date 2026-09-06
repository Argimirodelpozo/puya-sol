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
#include "builder/sol-types/ConversionPlan.h"
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

std::shared_ptr<awst::Expression> ContractBuilder::lowerStateInitializer(
	solidity::frontend::VariableDeclaration const& _var,
	awst::WType const* _target,
	awst::SourceLocation const& _loc)
{
	auto operand = m_exprBuilder->lower(*_var.value(), false);
	bool const pin = !operand.effects.post.empty();
	auto value = m_exprBuilder->emitSequencedOperand(
		std::move(operand.effects), std::move(operand.value), pin, _loc);
	return ConversionPlan{_var.value()->annotation().type, _var.type(), _target,
		ConversionPlan::Context::Initialization}.emit(
			std::move(value), _loc, &m_exprBuilder->preEffects());
}

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
			? lowerStateInitializer(*var, aggAddr->wtype, loc) : nullptr;
		bool done = false;
		if (aggAddr && aggVal)
		{
			aggAddr->solType = t;
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
	auto initVal = lowerStateInitializer(*var, addr->wtype, loc);
	if (!initVal)
		return;
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
		// Transient state belongs only to the transient runtime, never named
		// persistent cells (which are also absent from its advertised schema).
		auto binding = m_storageMapper.physicalBindingFor(*var);
		if (!binding.hasPersistentCell())
			continue;
		if (stateVarInitialized.count(var->id()))
			continue;
		stateVarInitialized.insert(var->id());

		// --evm-storage-layout: slot space zero-initialises for free
		// (absent box = 0); only explicit initializers need a write.
		// Immutables keep their named cells (they are not in EVM
		// storage) and fall through to the existing path.
		if (binding.initialization == StorageMapper::RootInitialization::Slot)
		{
			emitSlotModeStateVarInit(*var, targetBody, loc);
			continue;
		}

		auto kind = binding.kind;

		auto* wtype = binding.wtype;

		// Box ARC4 struct with explicit initializer: encode + box_put.
		// Dynamic arrays/bytes handled by m_boxArrayVars loop; skip here.
		if (kind == awst::AppStorageKind::Box)
		{
			if (binding.initialization != StorageMapper::RootInitialization::ExplicitBox)
				continue;
			auto initVal = lowerStateInitializer(*var, wtype, loc);
			if (!initVal)
				continue;
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

			defaultVal = lowerStateInitializer(*var, wtype, loc);
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
		auto binding = m_storageMapper.physicalBindingFor(*var);
		if (!binding.hasPersistentCell())
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

		if (binding.initialization == StorageMapper::RootInitialization::UnallocatedArrayBox)
		{
			auto const* array = static_cast<awst::ARC4StaticArray const*>(binding.wtype);
			Logger::instance().warning(
				"state array '" + var->name() + "' has declared size "
				+ std::to_string(array->arraySize())
				+ " which exceeds 4-box (128 KB) pre-allocation cap — skipping box_create. "
				"Element writes will fail at runtime but .length reads "
				"still return the declared size.", loc);
			return;
		}
		if (binding.initialization != StorageMapper::RootInitialization::DeferredArrayBox)
			return;

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


/// Bind each argument at its evaluation position, before later arguments can mutate it.
void ContractBuilder::bindBaseCtorArgs(
	solidity::frontend::FunctionDefinition const& constructor,
	std::vector<solidity::frontend::ASTPointer<solidity::frontend::Expression>> const& args,
	std::shared_ptr<awst::Block> const& body)
{
	using solidity::frontend::VariableDeclaration;
	auto const& params = constructor.parameters();
	for (size_t i = 0; i < args.size(); ++i)
	{
		auto const& parameter = *params.at(i);
		auto loc = makeLoc(args[i]->location());
		bool storage = parameter.referenceLocation() == VariableDeclaration::Location::Storage;
		bool slot = storage && m_typeMapper.profile().evmStorageLayout;
		auto const* type = slot ? awst::WType::biguintType() : m_typeMapper.map(parameter.type());
		auto operand = m_exprBuilder->lowerOperand([&]() {
			if (!slot)
				return m_exprBuilder->buildExpr(*args[i]);
			sol_ast::EvmSlotLowering low(*m_exprBuilder, *m_exprBuilder->currentScope, loc);
			auto address = low.resolve(*args[i]);
			return address ? address->slot : nullptr;
		}, true);
		bool const pin = !operand.effects.post.empty();
		auto value = m_exprBuilder->emitSequencedOperand(
			std::move(operand.effects), std::move(operand.value), pin, loc);
		if (!value)
			continue;
		if (storage && !slot)
		{
			m_tr->setStorageAlias(parameter.id(), sol_ast::StorageAlias::classify(std::move(value)));
			m_exprBuilder->appendEffectsTo(body->body);
			continue;
		}
		if (!slot)
			value = ConversionPlan{args[i]->annotation().type, parameter.type(), type,
				ConversionPlan::Context::Argument}.emit(std::move(value), loc);
		auto target = awst::makeVarExpression(m_tr->awstVarName(parameter), type, loc);
		if (slot)
			m_tr->setSlotStorageRef(parameter.id(), target);
		m_exprBuilder->appendEffectsTo(body->body);
		body->body.push_back(awst::makeAssignmentStatement(target, std::move(value), loc));
	}
}

/// Shared creation/post-init schedule, using solc's effective argument nodes and C3 order.
void ContractBuilder::emitConstructorPlan(
	solidity::frontend::ContractDefinition const& _contract,
	std::shared_ptr<awst::Block> const& body,
	std::function<void(solidity::frontend::ContractDefinition const&,
		std::vector<std::shared_ptr<awst::Statement>>&)> const& emitStateVarInit)
{
	using namespace solidity::frontend;
	auto const& linearized = _contract.annotation().linearizedBaseContracts;
	// Argument nodes are not Scopable. Index their lexical owners once; solc's
	// baseConstructorArguments still decides which node supplies each base.
	std::map<ASTNode const*, ContractDefinition const*> owners;
	std::vector<int64_t> remappedParams;
	for (auto const* level: linearized)
	{
		for (auto const& spec: level->baseContracts())
			owners.emplace(spec.get(), level);
		if (auto const* ctor = level->constructor())
		{
			for (auto const& invocation: ctor->modifiers())
				owners.emplace(invocation.get(), level);
			if (level != &_contract)
				for (auto const& param: ctor->parameters())
				{
					auto const* type = m_typeMapper.profile().evmStorageLayout
						&& param->referenceLocation() == VariableDeclaration::Location::Storage
						? awst::WType::biguintType() : m_typeMapper.map(param->type());
					m_tr->setParamRemap(param->id(), sol_ast::ParamRemap{
						"__ctor_param_" + std::to_string(param->id()), type});
					remappedParams.push_back(param->id());
				}
		}
	}
	struct Arguments {
		ContractDefinition const* owner;
		std::vector<ASTPointer<Expression>> const* expressions;
	};
	std::map<ContractDefinition const*, Arguments> arguments;
	for (auto const& [ctor, node]: _contract.annotation().baseConstructorArguments)
	{
		auto const* expressions = dynamic_cast<ModifierInvocation const*>(node)
			? static_cast<ModifierInvocation const*>(node)->arguments()
			: static_cast<InheritanceSpecifier const*>(node)->arguments();
		if (expressions && !expressions->empty())
			arguments.emplace(ctor->annotation().contract, Arguments{owners.at(node), expressions});
	}
	auto activate = [&](FunctionDefinition const* ctor) {
		m_tr->clearSuperTargets();
		for (auto const& [id, name]: m_allSuperTargetNames)
			m_tr->setSuperTarget(id, name);
		if (ctor)
			if (auto it = m_perFuncSuperOverrides.find(ctor->id()); it != m_perFuncSuperOverrides.end())
				for (auto const& [id, name]: it->second)
					m_tr->setSuperTarget(id, name);
		m_functionCtx->inConstructor = true;
		m_functionCtx->callableId = ctor ? ctor->id() : 0;
	};

	// Legacy initializes all state before evaluating base arguments. Via-IR
	// initializes each level immediately before executing its constructor.
	if (!m_viaIR)
		for (auto it = linearized.rbegin(); it != linearized.rend(); ++it)
			emitStateVarInit(**it, body->body);

	// Legacy visits base constructors in C3 order. IR evaluates each owner's
	// explicit argument expressions first, sorted by the same inheritance order.
	for (auto const* level: linearized)
	{
		activate(level->constructor());
		for (auto const* target: linearized)
			if (auto it = arguments.find(target); it != arguments.end()
				&& (m_viaIR ? it->second.owner == level : target == level))
			{
				activate(it->second.owner->constructor());
				bindBaseCtorArgs(*target->constructor(), *it->second.expressions, body);
			}
	}
	for (auto it = linearized.rbegin(); it != linearized.rend(); ++it)
	{
		auto const* ctor = (*it)->constructor();
		activate(ctor);
		emitStateVarInit(**it, body->body);
		if (!ctor || !ctor->isImplemented())
			continue;
		// An empty body can still have an effectful modifier chain.
		auto ctorBody = buildBlock(ctor->body());
		buildConstructorModifierChain(*ctor, ctorBody, _contract.name());
		for (auto& statement: ctorBody->body)
			body->body.push_back(std::move(statement));
	}
	m_functionCtx->inConstructor = false;
	m_functionCtx->callableId = 0;
	m_tr->clearSuperTargets();
	for (auto id: remappedParams)
		m_tr->eraseParamRemap(id);
}

/// buildApprovalProgram phase: init the transient-storage blob (transient scratch slot) BEFORE the create/dispatch split so the …
void ContractBuilder::emitTransientBlobInit(
	awst::Block& body, awst::SourceLocation const& loc)
{
	body.body.push_back(awst::makeExpressionStatement(awst::makeStoreSlot(
		m_transientStorage.scratchSlot(), awst::makeBzero(AssemblyBuilder::SLOT_SIZE, loc), loc), loc));
	if (auto size = m_transientStorage.addressShadowSize())
		body.body.push_back(awst::makeExpressionStatement(awst::makeStoreSlot(
			m_transientStorage.addressShadowSlot(), awst::makeBzero(size, loc), loc), loc));
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
	method.cref = m_contractId;
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

		if (needsPostInit)
		{
			buildPostInitMethod(_contract, _contractName, method, createBlock,
				emitStateVarInit);
		}
		else
		{
			auto const* savedReturnType = m_functionCtx->returnType;
			m_functionCtx->returnType = awst::WType::boolType();
			emitConstructorPlan(_contract, createBlock, emitStateVarInit);
			m_functionCtx->returnType = savedReturnType;
		}

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
		auto binding = m_storageMapper.physicalBindingFor(*var);
		auto const& varName = binding.name;
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
		auto* varWtype = binding.wtype;
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
				if (auto fixedSize = builder::computeEncodedElementSize(elemT).fixedBytes())
					elemSize = *fixedSize;
				// AVM box cap = 32768 B; oversized → multi-box below.
				// Record per-box size here.
				uint64_t count = static_cast<uint64_t>(sa->arraySize());
				uint64_t size = elemSize && count > 32768 / elemSize
					? 32768 : elemSize * count;
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
				auto initVal = lowerStateInitializer(*var, varWtype, _loc);
				if (initVal)
				{
					// Materialise as bytes for box_put.
					if (initVal->wtype != awst::WType::bytesType())
						initVal = awst::makeAsBytes(std::move(initVal), _loc);
					boxInitVal = std::move(initVal);
				}
			}
		}

		m_exprBuilder->appendEffectsTo(_postInitBody.body);

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
