#include <unordered_set>
#include "builder/SourceLocConvert.h"
#include "builder/AWSTBuilder.h"
#include "builder/storage/EvmLayoutMode.h"
#include "builder/sol-types/SolIntType.h"
#include "awst/Termination.h"
#include "builder/FunctionIdRegistry.h"
#include "builder/SubroutineRegistry.h"
#include "builder/builtin/Ripemd160Builder.h"
#include "builder/sol-ast/StorageRefPointer.h"
#include "builder/sol-ast/AsmScan.h"
#include "builder/sol-ast/stmts/SolBlock.h"
#include "builder/contract/ContractBuilder.h"
#include "builder/contract/ReturnRewriter.h"
#include "builder/sol-types/OverloadSuffix.h"
#include "builder/itxn/FunctionPointerBuilder.h"
#include "builder/assembly/AssemblyBuilder.h"
#include "builder/proxies/Erc1967Lowering.h"
#include "builder/sol-types/Arc4Defaults.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/ASTVisitor.h>

namespace puyasol::builder
{

using awst::statementAlwaysTerminates;
using awst::blockAlwaysTerminates;

/// Apply dead code elimination to all methods in a contract.
static void eliminateDeadCode(awst::Contract& _contract)
{
	auto dce = [](awst::ContractMethod& m) {
		if (m.body) awst::removeDeadCode(m.body->body);
	};
	dce(_contract.approvalProgram);
	dce(_contract.clearProgram);
	for (auto& m: _contract.methods)
		dce(m);
}

/// Collect the indices of `_func`'s parameters for which `_keep(pi)` is true,
/// in parameter order. Shared skeleton for the storage-ref and memory-ref
/// write-back param scans (the predicate is what differs between them).
template <typename Pred>
static std::vector<size_t> collectParamIndices(
	solidity::frontend::FunctionDefinition const& _func, Pred _keep)
{
	std::vector<size_t> indices;
	for (size_t pi = 0; pi < _func.parameters().size(); ++pi)
		if (_keep(pi))
			indices.push_back(pi);
	return indices;
}


std::vector<std::shared_ptr<awst::RootNode>> AWSTBuilder::build(
	solidity::frontend::CompilerStack& _compiler,
	std::string const& _sourceFile,
	uint64_t _opupBudget,
	std::map<std::string, uint64_t> const& _ensureBudget,
	bool _viaYulBehavior,
	std::map<std::string, std::string> const& _sourceAliases,
	TargetProfile _targetProfile
)
{
	_targetProfile.viaIRSequencing = _viaYulBehavior;
	m_session.begin(_compiler, _sourceAliases, std::move(_targetProfile));
	m_storageMapper = std::make_unique<StorageMapper>(m_session.typeMapper);
	m_hostBoundFunctions.clear();
	m_selectorContracts.clear();
	for (auto const& sourceName: _compiler.sourceNames())
		for (auto const* contract: solidity::frontend::ASTNode::filteredNodes<
			solidity::frontend::ContractDefinition>(_compiler.ast(sourceName).nodes()))
			if (contract && !contract->isInterface() && !contract->abstract()
				&& !contract->isLibrary())
				m_selectorContracts.push_back(contract);
	std::vector<std::shared_ptr<awst::RootNode>> roots;

	registerFunctionIds(_compiler, m_functionSymbols);
	presetDispatchCref(_compiler, _sourceFile, m_session.functionPointers);
	translateLibraryFunctions(_compiler, _sourceFile, roots);
	translateFreeFunctions(_compiler, _sourceFile, roots);
	translateContracts(_compiler, _sourceFile, _opupBudget, _ensureBudget, _viaYulBehavior, roots);

	// Builtin helpers are requested by their lowering sites, so unused
	// algorithms never enter the root set.
	if (m_session.artifacts.needsRipemd160)
	{
		awst::SourceLocation builtinLoc;
		builtinLoc.file = _sourceFile;
		roots.push_back(builder::builtin::buildRipemd160Subroutine(builtinLoc));
	}

	validateRootSubroutines(roots);
	return roots;
}


void AWSTBuilder::translateLibraryFunctions(
	solidity::frontend::CompilerStack& _compiler,
	std::string const& _sourceFile,
	std::vector<std::shared_ptr<awst::RootNode>>& roots)
{
	for (auto const& sourceName: _compiler.sourceNames())
	{
		auto const& sourceUnit = _compiler.ast(sourceName);

		for (auto const* contract: solidity::frontend::ASTNode::filteredNodes<
			solidity::frontend::ContractDefinition>(sourceUnit.nodes()))
		{
			if (!contract->isLibrary())
				continue;


			std::string libraryName = contract->name();
			Logger::instance().info("Translating library: " + libraryName);

			for (auto const* func: contract->definedFunctions())
			{
				if (!func->isImplemented())
					continue;

				std::string qualifiedName = libraryName + "." + func->name();
				auto const* symbol = m_functionSymbols.resolve(func->id());
				if (!symbol)
				{
					Logger::instance().error(
						"missing declaration identity for library function " +
						qualifiedName);
					continue;
				}
				auto const& subroutineId = *symbol;

				if (m_session.analysis.hasReachabilityGraphs
					&& !m_session.analysis.reachableFunctionIds.count(func->id()))
				{
					Logger::instance().debug(
						"skipping library function `" + qualifiedName + "`: no "
						"contract call graph reaches it (solc prunes it too)");
					continue;
				}

				// Library functions with fn-ptr params are internalized per-contract
				// instead of emitted as root subroutines: the fn-ptr dispatcher
				// may invoke contract instance methods, rejected from root scope.
				// EXTERNAL library fn-ptrs warn (Solidity would DELEGATECALL;
				// AVM has no equivalent) and are still internalized best-effort.
				bool hasFnParam = false;
				for (auto const& p: func->parameters())
				{
					if (dynamic_cast<solidity::frontend::FunctionType const*>(p->type()))
					{
						hasFnParam = true;
						break;
					}
				}
				bool const needsStorageHost = !m_session.profile.evmStorageLayout
					&& m_session.analysis.callablesWithStorageAssembly.count(func->id());
				if (hasFnParam || needsStorageHost)
				{
					if (hasFnParam
						&& func->visibility() == solidity::frontend::Visibility::External)
					{
						awst::SourceLocation warnLoc;
						warnLoc.file = _sourceFile;
						Logger::instance().warning(
							"external library function `" + qualifiedName + "` internalized "
							"into using-contract — Solidity would normally deploy this as a "
							"separate contract and DELEGATECALL it; AVM has no DELEGATECALL "
							"equivalent. Behaviour may diverge for storage-mutating bodies.",
							warnLoc);
					}
					Logger::instance().debug(
						"Registering host-bound library function: " + qualifiedName);
					m_hostBoundFunctions.push_back(func);
					continue;
				}

				Logger::instance().debug("Translating library function: " + qualifiedName);
				roots.push_back(buildFreestandingSubroutine(
					*func, _sourceFile, qualifiedName, subroutineId, libraryName));
			}
		}
	}
}


void AWSTBuilder::translateFreeFunctions(
	solidity::frontend::CompilerStack& _compiler,
	std::string const& _sourceFile,
	std::vector<std::shared_ptr<awst::RootNode>>& roots)
{
	for (auto const& sourceName: _compiler.sourceNames())
	{
		auto const& sourceUnit = _compiler.ast(sourceName);

		for (auto const* func: solidity::frontend::ASTNode::filteredNodes<
			solidity::frontend::FunctionDefinition>(sourceUnit.nodes()))
		{
			if (!func->isImplemented() || !func->isFree())
				continue;
			if (m_session.analysis.hasReachabilityGraphs
				&& !m_session.analysis.reachableFunctionIds.count(func->id()))
			{
				Logger::instance().debug(
					"skipping free function `" + func->name()
					+ "`: no contract call graph reaches it");
				continue;
			}

			std::string qualifiedName = func->name();
			if (!m_session.profile.evmStorageLayout
				&& m_session.analysis.callablesWithStorageAssembly.count(func->id()))
			{
				Logger::instance().debug(
					"Registering host-bound free function: " + qualifiedName);
				m_hostBoundFunctions.push_back(func);
				continue;
			}
			auto const* symbol = m_functionSymbols.resolve(func->id());
			if (!symbol)
			{
				Logger::instance().error(
					"missing declaration identity for free function " + qualifiedName);
				continue;
			}

			Logger::instance().debug("Translating free function: " + qualifiedName);
			roots.push_back(buildFreestandingSubroutine(
				*func, _sourceFile, qualifiedName, *symbol, /*libraryName=*/""));
		}
	}
}

std::shared_ptr<awst::Subroutine> AWSTBuilder::buildFreestandingSubroutine(
	solidity::frontend::FunctionDefinition const& _func,
	std::string const& _sourceFile,
	std::string const& _qualifiedName,
	std::string const& _subroutineId,
	std::string const& _libraryName)
{
	auto sub = std::make_shared<awst::Subroutine>();
	sub->inlineOpt = false; // Prevent puya from inlining large subroutines

	// EIP-1967 admin-slot uses lowered in this body attach to the contracts
	// whose call graphs reach this function, not to whichever contract builds
	// first (BuildArtifacts::noteErc1967AdminUse).
	struct FreestandingIdGuard
	{
		BuildArtifacts& artifacts;
		~FreestandingIdGuard() { artifacts.currentFreestandingFunctionId = -1; }
	} freestandingIdGuard{m_session.artifacts};
	m_session.artifacts.currentFreestandingFunctionId = _func.id();

	awst::SourceLocation loc = m_session.sourceMap.toAwstLoc(
		_sourceFile, _func.location());

	sub->sourceLocation = loc;
	sub->id = _subroutineId;
	sub->name = _qualifiedName;

	// Documentation
	if (_func.documentation())
		sub->documentation.description = *_func.documentation()->text();

	// Parameters — mapping storage refs become bytes (runtime key prefix).
	std::set<size_t> mappingStorageParams;
	std::set<size_t> blobAggParams;
	std::set<size_t> evmSlotRefParams;
	// Struct storage-ref params used via `.slot` in asm (solady storage libs):
	// travel as a box-key handle so `s.slot` resolves (see SolInlineAssembly).
	auto slotParams = structRefParamsUsedAsAsmSlot(_func);
	for (size_t pi = 0; pi < _func.parameters().size(); ++pi)
	{
		auto const& param = _func.parameters()[pi];
		awst::SubroutineArgument arg;
		arg.name = param->name();
		if (arg.name.empty())
			arg.name = "_param" + std::to_string(pi);
		arg.sourceLocation = m_session.sourceMap.toAwstLoc(
			_sourceFile, param->location());

		// Mapping storage refs (including array-of-mapping): callee receives
		// the caller's box key prefix as bytes so `m[k]` hashes against the
		// caller's storage var, not the param name. Without widening,
		// array-of-mapping params encode as their own "state var" and box
		// keys diverge from the auto-getter's reads.
		if (m_session.profile.evmStorageLayout
			&& param->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Storage)
		{
			// --evm-storage-layout: every storage ref IS a biguint slot handle;
			// writes go straight to the slot space (no box keys, no write-back).
			arg.wtype = awst::WType::biguintType();
			evmSlotRefParams.insert(pi);
		}
		else if (param->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Storage
			&& (isBoxKeyedStorageRef(param->type(), m_session.analysis)
				|| slotParams.count(pi))) // widened: plain structs + asm .slot refs
		{
			arg.wtype = awst::WType::bytesType();
			mappingStorageParams.insert(pi);
		}
		else if (param->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Memory
			&& memoryUsesBlob(m_session.typeMapper.map(param->type())))
		{
			// Memory aggregate >4KB → passed as uint64 base offset (pointer model).
			arg.wtype = awst::WType::uint64Type();
			blobAggParams.insert(pi);
		}
		else
			arg.wtype = m_session.typeMapper.map(param->type());
		sub->args.push_back(std::move(arg));
	}

	// Storage-ref params: augment return so callers can write the modified
	// value back. Mapping refs excluded (shared box key, no write-back).
	// Private functions excluded: puya threads their mutable args internally.
	std::vector<size_t> storageParamIndices;
	bool isMutating = _func.stateMutability() != solidity::frontend::StateMutability::View
		&& _func.stateMutability() != solidity::frontend::StateMutability::Pure;
	bool isPrivate = _func.visibility() == solidity::frontend::Visibility::Private;
	// --evm-storage-layout: slot handles write straight through — no write-back.
	if (isMutating && !isPrivate && !m_session.profile.evmStorageLayout)
	{
		storageParamIndices = collectParamIndices(_func, [&](size_t pi) {
			return _func.parameters()[pi]->referenceLocation()
					== solidity::frontend::VariableDeclaration::Location::Storage
				&& !mappingStorageParams.count(pi);
		});
	}

	// Memory-ref params: augment return so callers get write-back (Solidity
	// passes by ref; our translation copies at the boundary). Skipped on
	// private functions, bytes/string/mapping, and read-only params. The
	// read-only skip matters: Honk.Proof (14KB) would blow the AVM 4096B
	// per-stack-element cap if included unconditionally.
	std::vector<size_t> memoryRefParamIndices;
	if (!isPrivate && _func.isImplemented())
	{
		auto isMemRefType = [](solidity::frontend::Type const* t) {
			if (auto const* arr = dynamic_cast<solidity::frontend::ArrayType const*>(t))
				return !arr->isByteArrayOrString();
			return dynamic_cast<solidity::frontend::StructType const*>(t) != nullptr;
		};
		auto const& mutations = m_session.analysis.parameterMutations(
			nullptr, _func);
		memoryRefParamIndices = collectParamIndices(_func, [&](size_t pi) {
			auto const& p = _func.parameters()[pi];
			return p->referenceLocation()
					== solidity::frontend::VariableDeclaration::Location::Memory
				&& !blobAggParams.count(pi) // blob-backed via multi-slot blob; caller sees mutations directly
				&& p->type() && isMemRefType(p->type())
				&& mutations.mutates(pi); // skip read-only — no need to thread post-call value back
		});
	}

	// Return type — augmented with storage/memory param types for write-back.
	auto const& returnParams = _func.returnParameters();
	{
		std::vector<awst::WType const*> types;
		for (auto const& rp: returnParams)
		{
			// >4KB memory return → blob-backed, returns uint64 base offset (pointer model).
			auto const* rpW = m_session.typeMapper.map(rp->type());
			if (m_session.profile.evmStorageLayout
				&& rp->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Storage)
				types.push_back(awst::WType::biguintType());   // slot handle
			else if (!rp->name().empty()
				&& rp->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Memory
				&& memoryUsesBlob(rpW))
				types.push_back(awst::WType::uint64Type());
			else
				types.push_back(rpW);
		}
		for (size_t idx: storageParamIndices)
			types.push_back(sub->args[idx].wtype);
		for (size_t idx: memoryRefParamIndices)
			types.push_back(sub->args[idx].wtype);

		if (types.empty())
			sub->returnType = awst::WType::voidType();
		else if (types.size() == 1)
			sub->returnType = types[0];
		else
			sub->returnType = m_session.typeMapper.createType<awst::WTuple>(std::move(types));
	}

	sub->pure = _func.stateMutability() == solidity::frontend::StateMutability::Pure;

	// Build body. ContractContext stores overloadedNames as const& — must
	// pass a long-lived object (a temporary `{}` would dangle → SIGSEGV).
	static std::unordered_set<std::string> const EMPTY_OVERLOAD_NAMES;
	eb::ContractContext exprBuilder(
		m_session.typeMapper, *m_storageMapper, _sourceFile, _libraryName,
		EMPTY_OVERLOAD_NAMES, m_functionSymbols, m_session.functionPointers
	);
	exprBuilder.selectorContracts = m_selectorContracts;

	sol_ast::TranslationContext tr{exprBuilder, m_session.typeMapper, _sourceFile};
	auto trGuard = exprBuilder.pushScopeRaii(&tr);
	sol_ast::FunctionContext fnCtx{tr, {}, sub->returnType, {}};
	fnCtx.callableId = _func.id();
	for (auto const& rp: _func.returnParameters())
		fnCtx.returnSolTypes.push_back(rp->type());
	auto fnGuard = exprBuilder.pushScopeRaii(&fnCtx);

	// Register mapping-storage-ref params (must be after FunctionContext push;
	// setMappingKeyParam writes into nearestFunction(currentScope)).
	for (size_t idx: mappingStorageParams)
	{
		auto const& param = _func.parameters()[idx];
		fnCtx.setMappingKeyParam(param->id(), param->name());
	}

	// --evm-storage-layout: storage params are biguint slot handles.
	for (size_t idx: evmSlotRefParams)
	{
		auto const& param = _func.parameters()[idx];
		if (param->name().empty())
			continue;
		fnCtx.setSlotStorageRef(param->id(), awst::makeVarExpression(
			param->name(), awst::WType::biguintType(), awst::SourceLocation{}));
	}

	// Param/return context for inline assembly and sub-word integer truncation.
	{
		std::vector<std::pair<std::string, awst::WType const*>> paramContext;
		std::map<std::string, unsigned> bitWidths;
		std::map<std::string, awst::WType const*> boxKeyStructParams;
		for (size_t pi = 0; pi < _func.parameters().size(); ++pi)
		{
			auto const& param = _func.parameters()[pi];
			std::string pname = param->name();
			if (pname.empty())
				pname = "_param" + std::to_string(pi);
			auto* ptype = evmSlotRefParams.count(pi) ? awst::WType::biguintType()
				: mappingStorageParams.count(pi) ? awst::WType::bytesType()
				: blobAggParams.count(pi) ? awst::WType::uint64Type()
				: m_session.typeMapper.map(param->type());
			paramContext.emplace_back(pname, ptype);
			// Struct storage-ref param used via `.slot` in asm: record the ARC4
			// struct wtype so `param.slot` resolves to a BoxValueExpression over
			// the box-key handle (the bytes param value). Slot mode: the param
			// IS the biguint slot — no sentinel.
			if (slotParams.count(pi) && !m_session.profile.evmStorageLayout)
				boxKeyStructParams[pname] = m_session.typeMapper.map(param->type());
			if (auto it = builder::SolIntType::fromSol(param->annotation().type); it && it->bits < 64)
				bitWidths[pname] = it->bits;
		}
		for (auto const& rp: _func.returnParameters())
		{
			if (auto it = builder::SolIntType::fromSol(rp->annotation().type); it && it->bits < 64)
				bitWidths[rp->name()] = it->bits;
		}
		fnCtx.params = paramContext;
		fnCtx.returnType = sub->returnType;
		fnCtx.paramBitWidths = bitWidths;
		fnCtx.boxKeyStructParams = std::move(boxKeyStructParams);
	}

	// Construct the function-body block context for the body.
	auto blk = sol_ast::BlockContext::top(fnCtx);
	auto blkGuard = exprBuilder.pushScopeRaii(&blk);

	// Register mapping storage-ref return params (e.g. `returns (mapping(K=>V) storage r)`):
	// r[k] box-accesses using r's runtime bytes value as the holder prefix.
	// Slot mode: named storage returns are biguint slot handles instead.
	for (auto const& rp: returnParams)
	{
		if (rp->referenceLocation() != solidity::frontend::VariableDeclaration::Location::Storage
			|| rp->name().empty())
			continue;
		if (m_session.profile.evmStorageLayout)
			fnCtx.setSlotStorageRef(rp->id(), awst::makeVarExpression(
				rp->name(), awst::WType::biguintType(), awst::SourceLocation{}));
		else if (dynamic_cast<solidity::frontend::MappingType const*>(rp->type())
			|| storageRefReturnIsBytesKeyed(&_func)) // + box-keyed struct named returns
			fnCtx.setMappingKeyParam(rp->id(), rp->name());
	}

	// Register named memory return params >4KB as blob-backed (pointer model).
	for (auto const& rp: returnParams)
	{
		if (rp->name().empty()
			|| rp->referenceLocation() != solidity::frontend::VariableDeclaration::Location::Memory)
			continue;
		auto const* rpTypeB = m_session.typeMapper.map(rp->type());
		if (memoryUsesBlob(rpTypeB))
			fnCtx.setBlobAggregate(rp->id(), "__blobagg_off_" + std::to_string(rp->id()));
	}

	// Memory aggregate params >4KB: offset var = param name (caller passed it); no FMP bump.
	for (size_t idx: blobAggParams)
	{
		auto const& param = _func.parameters()[idx];
		std::string pname = param->name().empty() ? "_param" + std::to_string(idx) : param->name();
		fnCtx.setBlobAggregate(param->id(), pname);
	}

	// Promote memory aggregates used as asm-pointers (bytes/string buffers in
	// internal/library functions, e.g. OZ Strings.toString) to blob-backed before
	// body translation — the contract-method path does this in ContractBuilder's
	// buildBlock; the free/library path builds the body directly, so mark here too.
	markAssemblyAggregates(fnCtx, _func.body());

	// --evm-memory-layout: spill asm-pointer memory params (the LIBRARY path —
	// Morpho's MarketParamsLib.id(), Solady's LibString helpers, ...).
	std::vector<std::shared_ptr<awst::Statement>> asmParamSpills;
	emitAsmParamSpills(m_session.typeMapper, fnCtx, _func.body(), _sourceFile,
		asmParamSpills);

	sub->body = sol_ast::buildBlock(blk, _func.body());
	if (!asmParamSpills.empty())
		sub->body->body.insert(sub->body->body.begin(),
			std::make_move_iterator(asmParamSpills.begin()),
			std::make_move_iterator(asmParamSpills.end()));

	// Inline modifier bodies. currentContract=null (no virtual-override resolution).
	if (!_func.modifiers().empty())
	{
		std::vector<solidity::frontend::VariableDeclaration const*> namedReturnList;
		for (auto const& rp: returnParams)
			if (!rp->name().empty())
				namedReturnList.push_back(rp.get());

		std::vector<solidity::frontend::VariableDeclaration const*> mappingKeyList;
		for (size_t idx: mappingStorageParams)
			mappingKeyList.push_back(_func.parameters()[idx].get());
		for (auto const& rp: returnParams)
		{
			if (rp->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Storage
				&& dynamic_cast<solidity::frontend::MappingType const*>(rp->type())
				&& !rp->name().empty())
			{
				mappingKeyList.push_back(rp.get());
			}
		}

		std::vector<solidity::frontend::VariableDeclaration const*> blobAggList;
		for (size_t idx: blobAggParams)
			blobAggList.push_back(_func.parameters()[idx].get());

		fnCtx.namedReturns = std::move(namedReturnList);
		fnCtx.mappingKeyParams = std::move(mappingKeyList);
		fnCtx.blobAggParams = std::move(blobAggList);
		fnCtx.currentContract = nullptr;
		inlineModifiers(fnCtx, _func, sub->body);
	}

	// Zero-initialize named return variables (Solidity implicit init).
	{
		std::vector<std::shared_ptr<awst::Statement>> inits;
		for (auto const& rp: returnParams)
		{
			if (rp->name().empty())
				continue;
			// Box-keyed storage-ref named returns hold a bytes key — skip struct zero-init
			// (V4 Position.get's `position` is a bytes key, not a struct value).
			if (rp->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Storage
				&& storageRefReturnIsBytesKeyed(&_func))
				continue;
			// --evm-storage-layout: named storage return = biguint slot handle.
			if (m_session.profile.evmStorageLayout
				&& rp->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Storage)
			{
				inits.push_back(awst::makeAssignmentStatement(
					awst::makeVarExpression(rp->name(), awst::WType::biguintType(), loc),
					awst::makeZero(loc, awst::WType::biguintType()), loc));
				continue;
			}
			auto* rpType = m_session.typeMapper.map(rp->type());

			// Blob-backed (>4KB) returns: pre-zeroed via FMP bump; skip bzero init.
			if (rp->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Memory
				&& memoryUsesBlob(rpType))
				continue;

			auto target = awst::makeVarExpression(rp->name(), rpType, loc);

			std::shared_ptr<awst::Expression> zeroVal;
			if (rpType == awst::WType::boolType())
			{
				zeroVal = awst::makeBoolConstant(false, loc, rpType);
			}
			else if (rpType == awst::WType::uint64Type()
				|| rpType == awst::WType::biguintType())
			{
				zeroVal = awst::makeZero(loc, rpType);
			}
			else if (rpType && rpType->kind() == awst::WTypeKind::Bytes)
			{
				// For fixed-size bytes types (bytes1..bytes32), produce N zero bytes.
				std::vector<uint8_t> bytes;
				auto const* bytesType = dynamic_cast<awst::BytesWType const*>(rpType);
				if (bytesType && bytesType->length().has_value())
					bytes.assign(bytesType->length().value(), 0);
				zeroVal = awst::makeBytesConstant(
					std::move(bytes), loc, awst::BytesEncoding::Base16, rpType);
			}
			else
			{
				// Complex types: makeDefaultValue (fields may be partially assigned
				// via NewStruct copy-on-write before being fully initialized).
				zeroVal = StorageMapper::makeDefaultValue(rpType, loc);
			}

			auto assign = awst::makeAssignmentStatement(std::move(target), std::move(zeroVal), loc);
			inits.push_back(std::move(assign));
		}

		// Blob-backed (>4KB) memory returns: bind FMP base offset + bump FMP.
		for (auto const& rp: returnParams)
		{
			if (rp->referenceLocation()
				!= solidity::frontend::VariableDeclaration::Location::Memory)
				continue;
			auto const* rpTypeC = m_session.typeMapper.map(rp->type());
			int szC = computeEncodedElementSize(rpTypeC);
			if (szC <= AssemblyBuilder::SLOT_SIZE)
				continue;
			std::string offN = "__blobagg_off_" + std::to_string(rp->id());
			auto blobLoad = awst::makeLoadSlot(
				m_session.profile.scratchLayout.memoryFirst(), loc);
			auto base = awst::makeExtractUInt64(std::move(blobLoad),
				awst::makeIntegerConstant("88", loc), loc);
			inits.push_back(awst::makeAssignmentStatement(
				awst::makeVarExpression(offN, awst::WType::uint64Type(), loc),
				std::move(base), loc));
			for (auto& s: AssemblyBuilder::emitFreeMemoryBump(
					m_session.profile.scratchLayout, szC, loc,
					static_cast<int>(rp->id())))
				inits.push_back(std::move(s));
		}

		if (!inits.empty())
		{
			sub->body->body.insert(
				sub->body->body.begin(),
				std::make_move_iterator(inits.begin()),
				std::make_move_iterator(inits.end())
			);
		}
	}

	// Augment return statements to include storage/memory-ref param values.
	if (!storageParamIndices.empty() || !memoryRefParamIndices.empty())
	{
		// sub->returnType is WTuple (multi-augmented) or a bare type (void+1 aug arg).
		// TupleExpression with a non-WTuple wtype is invalid AWST — branch on shape.
		bool returnIsTuple =
			(dynamic_cast<awst::WTuple const*>(sub->returnType) != nullptr);
		size_t totalAugmented = storageParamIndices.size() + memoryRefParamIndices.size();

		// forEachReturnStatement covers ALL nesting (if/else, nested blocks,
		// loops, switch) — the old hand-rolled walk recursed only IfElse, so
		// an early `return` inside a loop kept its unaugmented value (the
		// FunctionBuilder twin had the same gap).
		forEachReturnStatement(sub->body->body, [&](awst::ReturnStatement& ret) {
			if (!returnIsTuple)
			{
				// Bare return type: one augmented arg. Only handle bare
				// `return;` — `return val;` in void+1-aug isn't valid Solidity.
				if (!ret.value && totalAugmented == 1)
				{
					size_t idx = !storageParamIndices.empty()
						? storageParamIndices[0]
						: memoryRefParamIndices[0];
					ret.value = awst::makeVarExpression(
						sub->args[idx].name, sub->args[idx].wtype,
						ret.sourceLocation);
				}
				// else: leave as-is; puya boundary will report the mismatch.
			}
			else
			{
				auto tuple = awst::makeTupleExpression(sub->returnType, ret.sourceLocation);
				if (ret.value)
				{
					// Flatten existing tuple items (WTuple is flat;
					// nesting would produce a shape mismatch).
					if (auto* origTup = dynamic_cast<awst::TupleExpression*>(ret.value.get()))
					{
						for (auto& it: origTup->items)
							tuple->items.push_back(it);
					}
					else
					{
						tuple->items.push_back(ret.value);
					}
				}
				for (size_t idx: storageParamIndices)
				{
					auto pv = awst::makeVarExpression(
						sub->args[idx].name, sub->args[idx].wtype,
						ret.sourceLocation);
					tuple->items.push_back(std::move(pv));
				}
				for (size_t idx: memoryRefParamIndices)
				{
					auto pv = awst::makeVarExpression(
						sub->args[idx].name, sub->args[idx].wtype,
						ret.sourceLocation);
					tuple->items.push_back(std::move(pv));
				}
				ret.value = std::move(tuple);
			}
		});
	}

	// Synthesize body for assembly-only library functions with known semantics.
	if (sub->body->body.empty() && _func.name() == "efficientKeccak256"
		&& _func.parameters().size() == 2)
	{
		auto varA = awst::makeVarExpression(_func.parameters()[0]->name(), m_session.typeMapper.map(_func.parameters()[0]->type()), loc);
		auto varB = awst::makeVarExpression(_func.parameters()[1]->name(), m_session.typeMapper.map(_func.parameters()[1]->type()), loc);
		auto concat = awst::makeConcat(std::move(varA), std::move(varB), loc);
		auto hash = awst::makeKeccak256(std::move(concat), loc);
		auto cast = awst::makeReinterpretCast(std::move(hash), sub->returnType, loc);
		auto ret = awst::makeReturnStatement(std::move(cast), loc);
		sub->body->body.push_back(std::move(ret));
	}

	// Synthesize implicit return on fall-through:
	//  1. Void + augmentation → return augmented args.
	//  2. Named returns → return named values.
	//  3. Otherwise → makeDefaultValue(returnType).
	if (!awst::blockAlwaysTerminates(*sub->body)
		&& (!returnParams.empty() || !storageParamIndices.empty()
			|| !memoryRefParamIndices.empty()))
	{
		bool hasNamedReturns = false;
		for (auto const& rp: returnParams)
			if (!rp->name().empty())
				hasNamedReturns = true;

		size_t totalAugmented2 = storageParamIndices.size() + memoryRefParamIndices.size();
		if (!hasNamedReturns && returnParams.empty() && totalAugmented2 > 0)
		{
			// Void + augmentation: return augmented args in storage-then-memory order.
			auto implicitReturn = awst::makeReturnStatement(nullptr, loc);
			if (totalAugmented2 == 1)
			{
				size_t idx = !storageParamIndices.empty()
					? storageParamIndices[0]
					: memoryRefParamIndices[0];
				implicitReturn->value = awst::makeVarExpression(sub->args[idx].name, sub->args[idx].wtype, loc);
			}
			else
			{
				auto tuple = awst::makeTupleExpression(sub->returnType, loc);
				for (size_t idx: storageParamIndices)
					tuple->items.push_back(awst::makeVarExpression(sub->args[idx].name, sub->args[idx].wtype, loc));
				for (size_t idx: memoryRefParamIndices)
					tuple->items.push_back(awst::makeVarExpression(sub->args[idx].name, sub->args[idx].wtype, loc));
				implicitReturn->value = std::move(tuple);
			}
			sub->body->body.push_back(std::move(implicitReturn));
		}
		else if (hasNamedReturns)
		{
			auto implicitReturn = awst::makeReturnStatement(nullptr, loc);

			// Include augmented args after named-return values to match sub->returnType.
			if (returnParams.size() == 1 && totalAugmented2 == 0
				&& returnParams[0]->referenceLocation()
					== solidity::frontend::VariableDeclaration::Location::Memory
				&& fnCtx.isAssemblyAggregate(returnParams[0]->id())
				&& !memoryUsesBlob(m_session.typeMapper.map(returnParams[0]->type())))
			{
				std::vector<std::shared_ptr<awst::Statement>> reads;
				implicitReturn->value = builder::materializeBlobValue(
					m_session.typeMapper, returnParams[0]->type(),
					m_session.typeMapper.map(returnParams[0]->type()),
					"__blobagg_off_" + std::to_string(returnParams[0]->id()),
					loc, reads);
				for (auto& st: reads)
					sub->body->body.push_back(std::move(st));
			}
			else if (returnParams.size() == 1 && totalAugmented2 == 0)
			{
				auto const* rp0W = m_session.typeMapper.map(returnParams[0]->type());
				if (m_session.profile.evmStorageLayout
					&& returnParams[0]->referenceLocation()
						== solidity::frontend::VariableDeclaration::Location::Storage)
					rp0W = awst::WType::biguintType();   // slot handle
				// Blob-backed >4KB → return uint64 base offset.
				if (returnParams[0]->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Memory
					&& memoryUsesBlob(rp0W))
					implicitReturn->value = awst::makeVarExpression(
						"__blobagg_off_" + std::to_string(returnParams[0]->id()),
						awst::WType::uint64Type(), loc);
				else
					implicitReturn->value = awst::makeVarExpression(
						returnParams[0]->name(), rp0W, loc);
			}
			else
			{
				auto tuple = awst::makeTupleExpression(nullptr, loc);
				for (auto const& rp: returnParams)
				{
					auto const* rpW = m_session.typeMapper.map(rp->type());
					if (m_session.profile.evmStorageLayout
						&& rp->referenceLocation()
							== solidity::frontend::VariableDeclaration::Location::Storage)
						rpW = awst::WType::biguintType();   // slot handle
					// Same blob-backed >4KB handling as the single-return case:
					// use the __blobagg_off_ uint64 offset var, not the aggregate
					// name/wtype (which was a nameless/mistyped tuple slot).
					if (rp->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Memory
						&& memoryUsesBlob(rpW))
						tuple->items.push_back(awst::makeVarExpression(
							"__blobagg_off_" + std::to_string(rp->id()),
							awst::WType::uint64Type(), loc));
					else if (rp->referenceLocation()
							== solidity::frontend::VariableDeclaration::Location::Memory
						&& fnCtx.isAssemblyAggregate(rp->id()))
					{
						std::vector<std::shared_ptr<awst::Statement>> reads;
						auto value = builder::materializeBlobValue(
							m_session.typeMapper, rp->type(), rpW,
							"__blobagg_off_" + std::to_string(rp->id()),
							loc, reads);
						for (auto& st: reads)
							sub->body->body.push_back(std::move(st));
						tuple->items.push_back(std::move(value));
					}
					else
						tuple->items.push_back(awst::makeVarExpression(rp->name(), rpW, loc));
				}
				for (size_t idx: storageParamIndices)
					tuple->items.push_back(awst::makeVarExpression(
						sub->args[idx].name, sub->args[idx].wtype, loc));
				for (size_t idx: memoryRefParamIndices)
					tuple->items.push_back(awst::makeVarExpression(
						sub->args[idx].name, sub->args[idx].wtype, loc));
				tuple->wtype = sub->returnType;
				implicitReturn->value = std::move(tuple);
			}

			sub->body->body.push_back(std::move(implicitReturn));
		}
		else
		{
			// No named returns: return zero default value.
			auto defReturn = awst::makeReturnStatement(StorageMapper::makeDefaultValue(sub->returnType, loc), loc);
			sub->body->body.push_back(std::move(defReturn));
		}
	}

	// 1967 slot constants surviving in a library/free body escaped into
	// runtime data flow (the OZ StorageSlot shape) — warn here; contract
	// bodies get the same scan in ContractBuilder.
	if (sub->body)
	{
		std::set<builder::proxies::Erc1967Slot> warned;
		builder::proxies::Erc1967Lowering::warnEscapedSlotConstants(
			*sub->body, warned);
	}

	return sub;
}

void AWSTBuilder::translateContracts(
	solidity::frontend::CompilerStack& _compiler,
	std::string const& _sourceFile,
	uint64_t _opupBudget,
	std::map<std::string, uint64_t> const& _ensureBudget,
	bool _viaYulBehavior,
	std::vector<std::shared_ptr<awst::RootNode>>& roots)
{
	bool evmStorageRuntimeNeeded = false;
	// Slot-mode unit pre-scan: the storage runtime subroutines share one
	// SubroutineID across the whole unit, so their bodies must be IDENTICAL for
	// every contract — decide dense-only / single-page globally BEFORE any
	// contract builds. Libraries and abstract bases count (their functions and
	// vars compile into hosts/derived contracts).
	if (m_session.profile.evmStorageLayout)
	{
		bool anySparse = false;
		unsigned long long maxSlots = 0;
		for (auto const& sourceName: _compiler.sourceNames())
			for (auto const* contract: solidity::frontend::ASTNode::filteredNodes<
				solidity::frontend::ContractDefinition>(_compiler.ast(sourceName).nodes()))
			{
				if (!contract || contract->isInterface())
					continue;
				auto const& storagePlan = m_session.storagePlan(*contract);
				auto const slots = storagePlan.solidityLayout.totalSlots();
				if (storagePlan.needsDispatch())
					evmStorageRuntimeNeeded = true;
				if (storagePlan.requiresSparseSlots)
					anySparse = true;
				if (slots > maxSlots)
					maxSlots = slots;
			}
		// Free/library subroutines are outside every contract's defined-function
		// walk. A reachable assembly sload/sstore still needs the unit runtime even
		// when every contract has zero declared state.
		for (auto const callableId:
			m_session.analysis.callablesWithStorageAssembly)
			if (!m_session.analysis.hasReachabilityGraphs
				|| m_session.analysis.reachableCallableIds.count(callableId))
			{
				evmStorageRuntimeNeeded = true;
				anySparse = true;
				break;
			}
		m_session.profile.denseOnlyStorage = !anySparse;
		m_session.profile.singlePageStorage =
			maxSlots <= builder::kEvmSlotsPerPage;
		Logger::instance().debug("PRESCAN dense=" + std::to_string(!anySparse)
			+ " singlePage=" + std::to_string(maxSlots <= builder::kEvmSlotsPerPage)
			+ " maxSlots=" + std::to_string(maxSlots));
	}

	bool emittedDeployable = false;
	bool emittedEvmStorageRuntime = false;
	std::vector<solidity::frontend::ContractDefinition const*> deployableLibraries;
	for (auto const& sourceName: _compiler.sourceNames())
	{
		auto const& sourceUnit = _compiler.ast(sourceName);

		for (auto const* contract: solidity::frontend::ASTNode::filteredNodes<
			solidity::frontend::ContractDefinition>(sourceUnit.nodes()))
		{
			// Skip interfaces, abstract contracts, and libraries (already handled)
			if (contract->isInterface())
			{
				Logger::instance().debug("Skipping interface: " + contract->name());
				continue;
			}

			if (contract->abstract())
			{
				Logger::instance().debug("Skipping abstract contract: " + contract->name());
				continue;
			}

			if (contract->isLibrary())
			{
				// Remember libraries with externally-callable functions: if the
				// source has NO deployable contract, EVM deploys the library
				// itself (public/external fns get external dispatch) — mirrored
				// after the loop.
				for (auto const* f: contract->definedFunctions())
					if (f->isImplemented() && !f->isConstructor()
						&& (f->visibility() == solidity::frontend::Visibility::Public
							|| f->visibility() == solidity::frontend::Visibility::External))
					{
						deployableLibraries.push_back(contract);
						break;
					}
				continue;
			}

			Logger::instance().info("Translating contract: " + contract->name());

			ContractBuilder translator(
				m_session.typeMapper, *m_storageMapper, m_session.functionPointers,
				_sourceFile, m_functionSymbols,
				_opupBudget, _ensureBudget, _viaYulBehavior,
				m_hostBoundFunctions
			);
			auto const& storagePlan = m_session.storagePlan(*contract);
			auto const emitEvmStorageRuntime = evmStorageRuntimeNeeded
				&& !emittedEvmStorageRuntime;
			auto awstContract = translator.build(
				*contract, storagePlan, emitEvmStorageRuntime);
			if (m_session.profile.evmStorageLayout && emitEvmStorageRuntime)
				emittedEvmStorageRuntime = true;

			// Collect dispatch subroutines as root nodes so library
			// subroutines can resolve them via SubroutineID.
			for (auto& sub : translator.takeDispatchSubroutines())
				roots.push_back(std::move(sub));

			// LogicSig: contract `is LogicSig` (AVM.sol) → AVM lsig instead of stateful app.
			// Entry function (logicsig modifier, or sole public method) becomes the program.
			// App state / inner-txns hard-fail downstream.
			{
				bool isLsig = false;
				for (auto const* base: contract->annotation().linearizedBaseContracts)
					if (base->name() == "LogicSig") { isLsig = true; break; }
				if (isLsig)
				{
					std::string entryName;
					for (auto const* f: contract->definedFunctions())
					{
						if (f->isConstructor() || !f->isImplemented())
							continue;
						for (auto const& modInv: f->modifiers())
						{
							auto const& p = modInv->name().path();
							if (!p.empty() && p.back() == "logicsig")
							{
								entryName = f->name();
								break;
							}
						}
						if (!entryName.empty())
							break;
					}
					awst::ContractMethod const* entry = nullptr;
					if (!entryName.empty())
					{
						for (auto const& m: awstContract->methods)
							if (m.memberName == entryName) { entry = &m; break; }
					}
					else
					{
						// Fallback: sole public/external ARC4 method.
						int pubCount = 0;
						for (auto const& m: awstContract->methods)
							if (m.arc4MethodConfig.has_value()) { entry = &m; ++pubCount; }
						if (pubCount != 1)
							entry = nullptr;
					}
					if (!entry)
					{
						Logger::instance().error(
							"contract `" + contract->name() + "` is LogicSig but has no single "
							"entry function — mark exactly one function with the `logicsig` modifier",
							awstContract->sourceLocation);
						continue;
					}
					auto program = std::make_shared<awst::Subroutine>();
					program->sourceLocation = entry->sourceLocation;
					program->id = awstContract->id;
					program->name = entry->memberName;
					program->args = entry->args;
					program->returnType = entry->returnType;
					program->body = entry->body;
					program->documentation = entry->documentation;
					program->pure = entry->pure;

					auto lsig = std::make_shared<awst::LogicSignature>();
					lsig->sourceLocation = awstContract->sourceLocation;
					lsig->id = awstContract->id;
					lsig->shortName = awstContract->name;
					lsig->program = std::move(program);
					lsig->docstring = awstContract->description;
					lsig->reservedScratchSpace = awstContract->reservedScratchSpace;
					lsig->avmVersion = awstContract->avmVersion;
					roots.push_back(std::move(lsig));
					emittedDeployable = true;
					Logger::instance().info("Emitted LogicSignature: " + contract->name());
					continue;
				}
			}

			// Only emit contracts with public methods or a constructor.
			// Non-deployable contracts (internal-only, e.g. ErrorReporter) are
			// translated for MRO resolution but not emitted to AWST.
			bool hasPublicMethod = false;
			for (auto const& method: awstContract->methods)
			{
				if (method.arc4MethodConfig.has_value())
				{
					hasPublicMethod = true;
					break;
				}
			}
			// Constructor-only contracts need a dummy ARC4 method so puya's
			// router has something to route (constructor runs at create time).
			if (!hasPublicMethod && !contract->abstract())
			{
				awst::ContractMethod dummy;
				dummy.sourceLocation = awstContract->sourceLocation;
				dummy.cref = awstContract->id;
				dummy.memberName = "__dummy";
				dummy.returnType = awst::WType::boolType();

				auto body = awst::makeBlock(dummy.sourceLocation);
				auto ret = awst::makeReturnStatement(awst::makeTrue(dummy.sourceLocation), dummy.sourceLocation);
				body->body.push_back(ret);
				dummy.body = body;

				awst::ARC4BareMethodConfig config;
				config.sourceLocation = dummy.sourceLocation;
				config.allowedCompletionTypes = {0}; // NoOp
				config.create = 3; // Disallow
				dummy.arc4MethodConfig = config;

				awstContract->methods.push_back(std::move(dummy));
				hasPublicMethod = true;
			}
			if (hasPublicMethod)
			{
				eliminateDeadCode(*awstContract);
				roots.push_back(std::move(awstContract));
				emittedDeployable = true;
			}
			else
				Logger::instance().debug("Skipping non-deployable contract: " + contract->name());
		}
	}

	// Library-only source: EVM deploys the library itself (public/external fns
	// get external dispatch). Mirror that by building the first such library as
	// a deployable contract — its fns are ALSO root subroutines (the library
	// pass above), which is fine: self-calls resolve to the subroutines, and
	// unused copies are DCE'd.
	if (!emittedDeployable)
		for (auto const* lib: deployableLibraries)
		{
			Logger::instance().info("Translating library as deployable contract: " + lib->name());
			ContractBuilder translator(
				m_session.typeMapper, *m_storageMapper, m_session.functionPointers,
				_sourceFile, m_functionSymbols,
				_opupBudget, _ensureBudget, _viaYulBehavior,
				m_hostBoundFunctions
			);
			auto const& storagePlan = m_session.storagePlan(*lib);
			auto const emitEvmStorageRuntime = evmStorageRuntimeNeeded
				&& !emittedEvmStorageRuntime;
			auto awstContract = translator.build(
				*lib, storagePlan, emitEvmStorageRuntime);
			if (m_session.profile.evmStorageLayout && emitEvmStorageRuntime)
				emittedEvmStorageRuntime = true;
			for (auto& sub : translator.takeDispatchSubroutines())
				roots.push_back(std::move(sub));
			bool hasPublicMethod = false;
			for (auto const& method: awstContract->methods)
				if (method.arc4MethodConfig.has_value()) { hasPublicMethod = true; break; }
			if (!hasPublicMethod)
				continue;
			eliminateDeadCode(*awstContract);
			roots.push_back(std::move(awstContract));
			break;
		}
}

} // namespace puyasol::builder
