#include "builder/contract/ContractBuilder.h"
#include "builder/ProgramAnalysis.h"
#include "builder/itxn/InnerCallHandlers.h"
#include "builder/storage/EvmLayoutMode.h"
#include "awst/Termination.h"
#include "awst/StatementWalk.h"
#include "awst/Visit.h"
#include "builder/AWSTBuilder.h"
#include "builder/NatSpecTags.h"
#include "builder/assembly/AssemblyBuilder.h"
#include "builder/contract/ParamABIValidator.h"
#include "builder/contract/ReturnRewriter.h"
#include "builder/sol-ast/stmts/SolBlock.h"
#include "builder/sol-ast/StorageRefPointer.h"
#include "builder/itxn/CallResolver.h"
#include "builder/sol-types/OverloadSuffix.h"
#include "builder/sol-types/SolIntType.h"
#include "builder/sol-types/Arc4Defaults.h"
#include "builder/sol-types/TypeCoercion.h"
#include "Logger.h"

#include <libsolidity/ast/ASTVisitor.h>

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
// Handle-model copy+write-back for MEMORY-ref params of internal contract methods. Solidity passes
// memory by reference (callee mutations propagate to the caller); our value-translation copies, so
// a method that mutates a memory STRUCT param would lose it. (Arrays already write through via puya
// ReferenceArray; libraries/free fns already augment in buildFreestandingSubroutine — this brings
// contract methods in line.) Each mutated memory-ref param is appended to the method's return; the
// internal caller (SolInternalCall) writes it back. Storage refs use the box-key/offset handle, not
// this. Mirrors the freestanding logic + the caller's memoryRefParamIndices filter exactly.
void augmentMethodForMutatedMemoryParams(
	awst::ContractMethod& method,
	solidity::frontend::FunctionDefinition const& func,
	TypeMapper& typeMapper,
	solidity::frontend::ContractDefinition const* mostDerived)
{
	using namespace solidity::frontend;
	if (!func.isImplemented() || !method.body) return;
	// Internal only: Public/External are ABI entry points (augmenting their return breaks the
	// selector's return ABI); Private is threaded by puya. Internal methods are pure callsub
	// targets — the analogue of library/free fns, which buildFreestandingSubroutine augments.
	if (func.visibility() != Visibility::Internal) return;

	auto isMemRefType = [](Type const* t) {
		if (auto const* arr = dynamic_cast<ArrayType const*>(t)) return !arr->isByteArrayOrString();
		return dynamic_cast<StructType const*>(t) != nullptr;
	};
	auto const& mutations = typeMapper.analysis().parameterMutations(
		mostDerived, func);

	std::vector<size_t> memIdx;
	for (size_t pi = 0; pi < func.parameters().size() && pi < method.args.size(); ++pi)
	{
		auto const& p = func.parameters()[pi];
		if (p->referenceLocation() != VariableDeclaration::Location::Memory) continue;
		if (!p->type() || !isMemRefType(p->type())) continue;
		if (!mutations.mutates(pi)) continue;
		memIdx.push_back(pi);
	}
	if (memIdx.empty()) return;

	auto const& loc = method.sourceLocation;

	// Augment the return type: original return value(s) (flattened), then each mem-param type.
	std::vector<awst::WType const*> types;
	bool origVoid = (method.returnType == awst::WType::voidType());
	auto const* origTuple = origVoid ? nullptr
		: dynamic_cast<awst::WTuple const*>(method.returnType);
	if (!origVoid)
	{
		if (origTuple) for (auto const* t : origTuple->types()) types.push_back(t);
		else types.push_back(method.returnType);
	}
	for (size_t idx : memIdx) types.push_back(method.args[idx].wtype);
	awst::WType const* newRetType =
		types.size() == 1 ? types[0] : typeMapper.createType<awst::WTuple>(std::move(types));
	method.returnType = newRetType;
	bool newIsTuple = (dynamic_cast<awst::WTuple const*>(newRetType) != nullptr);

	// Walk existing returns; append the mem-param vars to match the new shape.
	// forEachReturnStatement covers ALL nesting (if/else, nested blocks,
	// loops, switch) — the old hand-rolled walk recursed only IfElse, so an
	// early `return` inside a for loop kept its unaugmented value and puya
	// rejected valid Solidity with a return-type mismatch.
	forEachReturnStatement(method.body->body, [&](awst::ReturnStatement& ret) {
		if (!newIsTuple)
		{
			if (!ret.value && memIdx.size() == 1)
				ret.value = awst::makeVarExpression(
					method.args[memIdx[0]].name, method.args[memIdx[0]].wtype,
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
			for (size_t idx : memIdx)
				tuple->items.push_back(awst::makeVarExpression(
					method.args[idx].name, method.args[idx].wtype, ret.sourceLocation));
			ret.value = std::move(tuple);
		}
	});

	// Fall-through: only void methods reach here un-terminated (buildFunction already synthesised
	// a return for non-void fall-through, which the walk above augmented). Return the mem param(s).
	if (!awst::blockAlwaysTerminates(*method.body))
	{
		auto implicit = awst::makeReturnStatement(nullptr, loc);
		if (!newIsTuple && memIdx.size() == 1)
			implicit->value = awst::makeVarExpression(
				method.args[memIdx[0]].name, method.args[memIdx[0]].wtype, loc);
		else
		{
			auto tuple = awst::makeTupleExpression(newRetType, loc);
			for (size_t idx : memIdx)
				tuple->items.push_back(awst::makeVarExpression(
					method.args[idx].name, method.args[idx].wtype, loc));
			implicit->value = std::move(tuple);
		}
		method.body->body.push_back(std::move(implicit));
	}
}
} // namespace

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
		arg.wtype = m_typeMapper.map(param->type());
		// --evm-storage-layout: a storage ref IS a biguint slot number.
		if (m_typeMapper.profile().evmStorageLayout
			&& param->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Storage)
			arg.wtype = awst::WType::biguintType();
		else if (param->referenceLocation()
				== solidity::frontend::VariableDeclaration::Location::Storage
			&& (isBoxKeyedStorageRef(param->type(), m_typeMapper.analysis())
				|| asmSlotParamIds.count(param->id())))
			arg.wtype = awst::WType::bytesType();
		// Memory aggregate >4KB: pass as uint64 base offset (blob pointer model).
		// Callee re-registers via setBlobAggParams so p.field[i] hits blob word access.
		if (param->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Memory
			&& memoryUsesBlob(arg.wtype))
			arg.wtype = awst::WType::uint64Type();
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

	// Return type
	auto const& returnParams = _func.returnParameters();
	std::vector<SignedReturnInfo> signedReturns;
	std::vector<UnsignedMaskInfo> unsignedMasks;

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
			if (isAbiBoundary)
				signedReturns.push_back({intInfo->bits, 0});
		}
		else if (intInfo && !intInfo->isSigned && intInfo->bits < 64)
		{
			if (isAbiBoundary)
				unsignedMasks.push_back({intInfo->bits, 0});
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
					if (isAbiBoundary)
						signedReturns.push_back({intInfo->bits, ri});
				}
				else if (!intInfo->isSigned && intInfo->bits < 64)
				{
					if (isAbiBoundary)
						unsignedMasks.push_back({intInfo->bits, ri});
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

	// Pure/view
	method.pure = _func.stateMutability() == solidity::frontend::StateMutability::Pure;

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

	// ARC4 methods: remap param types to ARC4; stash decode ops for deferred insertion.
	struct ParamDecode
	{
		std::string name;
		awst::WType const* nativeType;
		awst::WType const* arc4Type;
		awst::SourceLocation loc;
		unsigned maskBits = 0; // >0 for sub-64-bit unsigned types needing input masking
		unsigned signedBits = 0; // >0 for signed 64<N<256 int params: sign-extend to 256-bit after decode
	};
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
				auto const* arc4Type = m_typeMapper.createType<awst::ARC4UIntN>(static_cast<int>(bits));
				// Signed sub-256 (64<N<256) decodes to N-bit two's complement; sign-extend
				// to 256-bit so downstream ops (compare, negate) see the correct sign.
				// int256 is already canonical; ≤64-bit is uint64-backed (buildABIEntryChecks).
				unsigned signedBits =
					(intInfo && intInfo->isSigned && bits > 64 && bits < 256) ? bits : 0;
				paramDecodes.push_back({arg.name, arg.wtype, arc4Type, arg.sourceLocation, 0, signedBits});
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

			awst::WType const* arc4Type = m_typeMapper.mapToARC4Type(arg.wtype);
			if (arc4Type != arg.wtype)
			{
				paramDecodes.push_back({arg.name, arg.wtype, arc4Type, arg.sourceLocation});
				arg.wtype = arc4Type;
			}
		}
	}

	if (_func.isImplemented())
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
		}


		// D2 build-time ABI return encoding. Instead of the ReturnRewriter post-pass
		// walking the finished body to convert each return value to its wire type,
		// SolReturnStatement encodes it as it builds the `return`. Scope (A1+A2):
		// non-chain, ABI-boundary, non-asm functions with any ENCODED return element
		// (biguint / signed, scalar or tuple). Sub-word masks (Pass 5), arrays
		// (Pass 1), asm, and modifier'd (chain) returns still go through the post-pass
		// / encodeChainDispatchReturn for now.
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

		m_functionCtx->inConstructor = _func.isConstructor();
		m_functionCtx->callableId = _func.id();
		m_functionCtx->frameIsProgram =
			_func.visibility() == solidity::frontend::Visibility::Internal
			|| _func.visibility() == solidity::frontend::Visibility::Private;
		method.body = buildBlock(_func.body());
		m_functionCtx->inConstructor = false;
		m_functionCtx->callableId = 0;
		m_functionCtx->frameIsProgram = false;

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

		// Storage-ref pointer: rewrite `return stateVar[idx]` to return just the
		// uint64 index; call sites reconstitute the location (SolInternalCall).
		if (storageRefPointerReturn(&_func))
		{
			std::function<void(std::vector<std::shared_ptr<awst::Statement>>&)> rewriteRet;
			rewriteRet = [&](std::vector<std::shared_ptr<awst::Statement>>& stmts)
			{
				for (auto& stmt: stmts)
				{
					if (auto* ret = dynamic_cast<awst::ReturnStatement*>(stmt.get()))
					{
						if (auto* ix = dynamic_cast<awst::IndexExpression*>(ret->value.get()))
							ret->value = TypeCoercion::implicitNumericCast(
								ix->index, awst::WType::uint64Type(),
								ret->value->sourceLocation);
					}
					else
						// awst::forEachChildBlock: the old hand list missed
						// WhileLoop/Switch/ForInLoop — `return stateArr;` inside
						// a loop skipped the storage-ref index rewrite.
						awst::forEachChildBlock(*stmt, [&](awst::Block& b, bool) {
							rewriteRet(b.body);
						});
				}
			};
			rewriteRet(method.body->body);
		}

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
					else if (m_typeMapper.profile().evmMemoryLayout
						&& retParams[0]->referenceLocation()
							== solidity::frontend::VariableDeclaration::Location::Memory
						&& [&]{ auto const* at3 = dynamic_cast<
								solidity::frontend::ArrayType const*>(
								retParams[0]->type());
							return at3 && at3->isByteArrayOrString(); }()
						&& blockUsesDeclInAsm(_func.body(), retParams[0]->id()))
						// blob-backed named bytes/string return: the asm may
						// have REPOINTED it (solady toHexString) — materialise
						// from the (possibly moved) offset var.
						retStmt->value = AssemblyBuilder::materializeBlobBytesValue(
							m_typeMapper.profile().scratchLayout,
							"__blobagg_off_" + std::to_string(retParams[0]->id()),
							dynamic_cast<solidity::frontend::ArrayType const*>(
								retParams[0]->type())->isString(),
							method.sourceLocation);
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
						auto var = awst::makeVarExpression(rp->name(), vt, method.sourceLocation);
						tuple->items.push_back(std::move(var));
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

		// Build-time encoding path: all return values were encoded in place (explicit
		// returns in SolReturnStatement, the implicit one above); promote the method's
		// declared type to the wire type and skip the post-pass. The body was translated
		// with the NATIVE returnType so named-return assignments still typecheck.
		if (encodeReturnsAtBuildTime)
		{
			if (returnPlan.size() == 1)
				method.returnType = returnPlan[0].wireType;
			else
			{
				std::vector<awst::WType const*> wireTypes;
				for (auto const& p: returnPlan)
					wireTypes.push_back(p.wireType);
				method.returnType = m_typeMapper.createType<awst::WTuple>(std::move(wireTypes));
			}
		}
		else
			rewriteARC4Returns(method, _func, m_typeMapper, signedReturns, unsignedMasks);

		// Asm bodies handle param data directly via calldataload; skip ARC4 decode.
		// Any-depth scan — must agree with funcHasInlineAssembly (remap gate).
		bool hasInlineAssembly = funcHasInlineAssembly;

		// Decode ARC4-remapped params: rename arg to __arc4_<name> and stash decodes.
		// Deferred until after modifier inlining: inlineModifiers replaces method.body
		// wholesale, so prepending earlier would bury the decode inside the wrap.
		std::vector<std::shared_ptr<awst::Statement>> deferredDecodes;
		if (!paramDecodes.empty())
		{
			for (auto& pd: paramDecodes)
			{
				// Rename the method arg to __arc4_<name>
				std::string arc4Name = "__arc4_" + pd.name;
				for (auto& arg: method.args)
				{
					if (arg.name == pd.name)
					{
						arg.name = arc4Name;
						arg.wtype = pd.arc4Type; // deferred for asm fns; idempotent for the rest
						break;
					}
				}

				auto arc4Var = awst::makeVarExpression(arc4Name, pd.arc4Type, pd.loc);

				std::shared_ptr<awst::Expression> decodeExpr;
				auto const* refArr = dynamic_cast<awst::ReferenceArray const*>(pd.nativeType);
				if (refArr && !refArr->arraySize().has_value())
				{
					// Dynamic array: ConvertArray (len+substring3) not ARC4Decode,
					// because extract3(v,2,0) returns empty bytes in the puya backend.
					auto convert = awst::makeConvertArray(std::move(arc4Var), pd.nativeType, pd.loc);
					decodeExpr = std::move(convert);
				}
				else
				{
					auto decode = awst::makeARC4Decode(std::move(arc4Var), pd.nativeType, pd.loc);
					// Signed sub-256: ARC4 decode yields N-bit form; sign-extend to 256-bit
					// so ops like getAmount*Delta(int128) liquidity<0 branch read sign correctly.
					if (pd.signedBits > 0)
						decodeExpr = TypeCoercion::signExtendToUint256(
							std::move(decode), pd.signedBits, pd.loc);
					else
						decodeExpr = std::move(decode);
				}

				auto target = awst::makeVarExpression(pd.name, pd.nativeType, pd.loc);

				auto assign = awst::makeAssignmentStatement(std::move(target), std::move(decodeExpr), pd.loc);
				deferredDecodes.push_back(std::move(assign));
			}
		}

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

		// Transient blob init is in the approval-program preamble (TRANSIENT_SLOT);
		// per-method init would reset it mid-dispatch, clobbering earlier writes.

		// Modifiers → a per-modifier SUBROUTINE CHAIN (mirrors solc's IR modifier lowering,
		// `IRGenerator::generateModifier`): `__mod{i}_N` + `__body_N` subs, each threading the
		// return params in/out and passing the still-ARC4-encoded `__arc4_*` params along. This
		// replaced the old textual `_`-expansion inliner as the default — the textual path
		// mis-CSE'd a multiple-`_;` modifier whose body contained a call, and the chain handles
		// it correctly (one lowering instead of two divergent ones). The textual inliner
		// (ModifierBodyInliner via ContractBuilder::inlineModifiers) is retained for constructors
		// / library / free functions. Any sub that USES a param needs the native decode, so hand
		// the decodes to buildModifierChain (it clones them into every sub); the outer method
		// just dispatches, so its own decode below is suppressed.
		if (!_func.modifiers().empty())
		{
			buildModifierChain(_func, method, _contractName, deferredDecodes);
			deferredDecodes.clear();   // consumed by the chain; the outer insert below is a no-op
			// The chain threads NATIVE return values; encode the outer dispatch
			// return to its ABI wire type (biguint would otherwise publish as
			// "uint512" while callers name the declared width → selector mismatch).
			encodeChainDispatchReturn(method, _func, m_typeMapper);
		}

		// Insert deferred ARC4 decodes at top of the now-modifier-wrapped body.
		if (!deferredDecodes.empty())
		{
			method.body->body.insert(
				method.body->body.begin(),
				std::make_move_iterator(deferredDecodes.begin()),
				std::make_move_iterator(deferredDecodes.end())
			);

			// Self-recursive callsub fix-up: after param remap, internal f(...) calls
			// still pass biguint args; wrap each remapped position in ARC4Encode.
			std::string thisName = eb::CallResolver::resolveMethodName(m_tr->contractCtx, _func);
			std::map<std::string, awst::WType const*> arc4ByOrig;
			for (auto const& pd: paramDecodes)
				arc4ByOrig[pd.name] = pd.arc4Type;

			awst::visitExpressions(*method.body, [&](awst::Expression& expression) {
				auto* call = dynamic_cast<awst::SubroutineCallExpression*>(&expression);
				if (!call)
					return;
				auto const* tgt = std::get_if<awst::InstanceMethodTarget>(&call->target);
				if (!tgt || tgt->memberName != thisName)
					return;
				size_t argI = 0;
				for (auto const& pd: paramDecodes)
				{
					if (argI >= call->args.size()) break;
					auto& a = call->args[argI++];
					if (!a.value || a.value->wtype == pd.arc4Type)
						continue;
					if (a.value->wtype != awst::WType::biguintType())
						continue;
					auto enc = awst::makeARC4Encode(std::move(a.value), pd.arc4Type, a.value->sourceLocation);
					a.value = std::move(enc);
				}
			});
		}

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
	else
	{
		// Abstract — empty body.
		Logger::instance().debug("function '" + method.memberName + "' has no implementation", method.sourceLocation);
		method.body = awst::makeBlock(method.sourceLocation);
	}

	// Write-back augmentation for mutated MEMORY-ref params (Solidity passes memory by ref).
	// No-op unless the method mutates a memory struct/array param; the internal caller writes back.
	augmentMethodForMutatedMemoryParams(
		method, _func, m_typeMapper, m_currentContract);

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
