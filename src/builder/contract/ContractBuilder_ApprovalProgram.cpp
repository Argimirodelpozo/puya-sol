#include "builder/contract/ContractBuilder.h"
#include "builder/types/ConstructorWirePlan.h"
#include "builder/codec/EvmAbiDecode.h"
#include "builder/types/SolIntType.h"
#include "builder/yul/AssemblyBuilder.h"
#include "builder/contract/PostInitTriggers.h"
#include "builder/contract/SelectorRouter.h"
#include "builder/storage/StateVarWalker.h"
#include "builder/ast/calls/SolNewExpression.h"
#include "builder/ast/SolStatement.h"
#include "builder/lowering/calls/FunctionPointerBuilder.h"
#include "builder/storage/slot/EvmSlotLowering.h"
#include "builder/codec/Arc4Defaults.h"
#include "builder/types/TypeCoercion.h"
#include "builder/types/ConversionPlan.h"
#include "builder/target/EvmLayoutMode.h"
#include "Logger.h"

#include <libsolidity/ast/ASTVisitor.h>
#include <libsolutil/Common.h>

#include <boost/multiprecision/cpp_int.hpp>
#include <algorithm>
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

/// Slot space defaults to zero; explicit initializers use the shared typed writer.
void ContractBuilder::emitSlotModeStateVarInit(
	solidity::frontend::VariableDeclaration const& var,
	std::vector<std::shared_ptr<awst::Statement>>& targetBody,
	awst::SourceLocation const& loc)
{
	if (!var.value()) return;
	sol_ast::EvmSlotLowering low(*m_exprBuilder, *m_exprBuilder->currentScope, loc);
	auto addr = low.addrForStateVar(var);
	if (!addr) return;
	auto value = lowerStateInitializer(var, addr->wtype, loc);
	if (!value) return;
	std::vector<std::shared_ptr<awst::Statement>> writes;
	if (!low.writeAny(*addr, var.type(), std::move(value), writes))
	{
		Logger::instance().error("--evm-storage-layout: unsupported state initializer for '" + var.name() + "'", loc);
		return;
	}
	m_exprBuilder->appendEffectsTo(targetBody);
	targetBody.insert(targetBody.end(), writes.begin(), writes.end());
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

		// Absent named cells already read as the solc default. Writing zero
		// here would erase writes made by an earlier initializer to this var.
		// Explicit values alone execute in the constructor schedule; the
		// shared typed store also resizes preallocated dynamic boxes.
		if (!var->value()) continue;
		auto value = lowerStateInitializer(*var, binding.valueType(m_typeMapper), loc);
		if (!value) continue;
		m_exprBuilder->appendEffectsTo(targetBody);
		targetBody.push_back(awst::makeExpressionStatement(
			m_storageMapper.createStateWrite(binding, std::move(value), loc), loc));
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
bool reachesDynamicBoolArray(solidity::frontend::Type const* _t)
{
	using namespace solidity::frontend;
	std::set<Type const*> seen;
	std::vector<Type const*> pending{_t};
	while (!pending.empty())
	{
		auto const* type = pending.back();
		pending.pop_back();
		if (!type || !seen.insert(type).second) continue;
		if (auto const* array = dynamic_cast<ArrayType const*>(type))
		{
			if (array->isDynamicallySized() && array->baseType()->category() == Type::Category::Bool)
				return true;
			pending.push_back(array->baseType());
		}
		else if (auto const* mapping = dynamic_cast<MappingType const*>(type))
			pending.push_back(mapping->valueType());
		else if (auto const* structure = dynamic_cast<StructType const*>(type))
			for (auto const& member: structure->members(nullptr)) pending.push_back(member.type);
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
	// Deferred arguments enter through __postInit, not the bare create call.
	if (needsPostInit) return;
	auto const* constructor = &_constructor;
	if (m_typeMapper.profile().contractAbi == ContractAbi::Evm
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
		ConstructorWirePlan wire(m_typeMapper, constructor, false);
		for (size_t i = 0; i < wire.parameters.size(); ++i)
		{
			auto const& parameter = wire.parameters[i];
			auto value = wire.decodeCreate(i, awst::makeAppArg(static_cast<int>(i), loc), loc, createBlock->body);
			createBlock->body.push_back(awst::makeAssignmentStatement(
				awst::makeVarExpression(parameter.name, parameter.type, loc),
				std::move(value), loc));
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
			m_functionCtx->scope.bindings.storageAliases.set(parameter.id(), sol_ast::StorageAlias::classify(std::move(value)));
			m_exprBuilder->appendEffectsTo(body->body);
			continue;
		}
		if (!slot)
			value = ConversionPlan{args[i]->annotation().type, parameter.type(), type,
				ConversionPlan::Context::Argument}.emit(std::move(value), loc);
		auto target = awst::makeVarExpression(m_functionCtx->scope.awstVarName(parameter), type, loc);
		if (slot)
			m_functionCtx->scope.bindings.slotStorageRefs.set(parameter.id(), target);
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
	// Partial writes to global aggregates need a backing value. Allocate all
	// of them before any initializer/base argument can read or mutate one;
	// per-declaration zeroing would erase earlier writes to later variables.
	forEachStateVar(_contract, [&](auto const* var) {
		auto binding = m_storageMapper.physicalBindingFor(*var);
		if (binding.initialization == StorageMapper::RootInitialization::NamedCell
			&& !var->type()->isValueType())
		{
			auto loc = makeLoc(var->location());
			body->body.push_back(awst::makeExpressionStatement(m_storageMapper.createStateWrite(
				binding, TypeCoercion::makeDefaultValue(binding.wtype, loc), loc), loc));
		}
	});
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
					m_functionCtx->scope.bindings.paramRemaps.set(param->id(), sol_ast::ParamRemap{
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
		m_functionCtx->inConstructor = true;
		m_functionCtx->callableId = ctor ? ctor->id() : 0;
	};

	// Legacy initializes all state before evaluating base arguments. Via-IR
	// initializes each level immediately before executing its constructor.
	if (!m_typeMapper.profile().viaIRSequencing)
		for (auto it = linearized.rbegin(); it != linearized.rend(); ++it)
			emitStateVarInit(**it, body->body);

	// Legacy visits base constructors in C3 order. IR evaluates each owner's
	// explicit argument expressions first, sorted by the same inheritance order.
	for (auto const* level: linearized)
	{
		activate(level->constructor());
		for (auto const* target: linearized)
			if (auto it = arguments.find(target); it != arguments.end()
				&& (m_typeMapper.profile().viaIRSequencing ? it->second.owner == level : target == level))
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
		if (!ctor->modifiers().empty())
			registerModifierMemoryRootParams(*ctor);
		auto ctorBody = buildBlock(ctor->body());
		buildConstructorModifierChain(*ctor, ctorBody, _contract.name());
		for (auto& statement: ctorBody->body)
			body->body.push_back(std::move(statement));
	}
	m_functionCtx->inConstructor = false;
	m_functionCtx->callableId = 0;
	for (auto id: remappedParams)
		m_functionCtx->scope.bindings.paramRemaps.erase(id);
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
	auto method = awst::ContractMethod(m_contractId, "approval_program",
		awst::WType::boolType(), {}, makeLoc(_contract.location()));

	auto body = method.body;

	// __postInit triggers: box writes, new C(), msg.*, or AVM stdlib calls.
	bool needsPostInit = computeNeedsPostInit(_contract, m_storageMapper, m_typeMapper.analysis());

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
			solidity::ScopedSaveAndRestore returnTypeGuard(
				m_functionCtx->returnType, awst::WType::boolType());
			emitConstructorPlan(_contract, createBlock, emitStateVarInit);
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
	return method;
}

void ContractBuilder::emitBoxCreateForStateVars(
	awst::Block& body,
	awst::SourceLocation const& loc)
{
	// Allocation/defaults precede constructor execution so earlier
	// initializers can read any state variable. No initializer runs here.
	for (auto const* var: m_boxArrayVars)
	{
		auto binding = m_storageMapper.physicalBindingFor(*var);
		auto* type = binding.wtype;
		auto key = awst::makeUtf8BytesConstant(binding.key, loc);
		if (StorageMapper::isMultiBoxArray(type))
		{
			auto const total = StorageMapper::arc4StaticArrayTotalBytes(type);
			auto const pageBytes = uint64_t(StorageMapper::elementsPerBox(type))
				* StorageMapper::arc4StaticArrayElementSize(type);
			for (unsigned page = 0; page < StorageMapper::numBoxesForArray(type); ++page)
			{
				auto pageKey = awst::makeConcat(key, awst::makeItob(
					awst::makeIntegerConstant(page, loc), loc), loc);
				auto size = std::min(pageBytes, total - page * pageBytes);
				body.body.push_back(awst::makeExpressionStatement(awst::makeBoxCreate(
					std::move(pageKey), awst::makeIntegerConstant(size, loc), loc), loc));
			}
			continue;
		}
		if (type->kind() == awst::WTypeKind::ARC4StaticArray && arc4IsDynamic(type))
		{
			// Dynamic-element heads need valid offsets, not a zeroed buffer.
			auto value = TypeCoercion::makeDefaultValue(type, loc);
			body.body.push_back(awst::makeExpressionStatement(
				m_storageMapper.createStateWrite(binding, std::move(value), loc), loc));
			continue;
		}
		auto const* array = dynamic_cast<solidity::frontend::ArrayType const*>(var->type());
		auto size = array && array->isByteArrayOrString() ? 0
			: builder::computeEncodedElementSize(type).fixedBytes().value_or(2);
		body.body.push_back(awst::makeExpressionStatement(awst::makeBoxCreate(
			std::move(key), awst::makeIntegerConstant(size, loc), loc), loc));
	}
}

} // namespace puyasol::builder
