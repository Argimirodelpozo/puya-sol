#include <unordered_set>
#include "builder/SourceLocConvert.h"
#include "builder/AWSTBuilder.h"
#include "builder/sol-types/RefParamPassing.h"
#include "builder/storage/EvmLayoutMode.h"
#include "builder/sol-types/SolIntType.h"
#include "awst/Termination.hpp"
#include "awst/StatementWalk.h"
#include "builder/FunctionIdRegistry.h"
#include "builder/SubroutineRegistry.hpp"
#include "builder/abi/Arc4Stdlib.h"
#include "builder/builtin/Ripemd160Builder.h"
#include "builder/itxn/AsaIntrinsics.h"
#include "builder/sol-ast/StorageRefPointer.h"
#include "builder/sol-ast/AsmScan.h"
#include "builder/sol-ast/stmts/SolBlock.h"
#include "builder/contract/ContractBuilder.h"
#include "builder/contract/ReturnFinishing.h"
#include "builder/sol-types/OverloadSuffix.h"
#include "builder/itxn/FunctionPointerBuilder.h"
#include "builder/assembly/AssemblyBuilder.h"
#include "builder/proxies/Erc1967Lowering.h"
#include "builder/sol-types/Arc4Defaults.h"
#include "Logger.h"

#include <cctype>
#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/ASTVisitor.h>
// yul nodes BY VALUE (the AST aliases are std::variant, which needs
// complete types). Kept out of AssemblyBuilder.h so only the TUs that
// actually instantiate them pay the ~223k lines.
#include <libyul/AST.h>
#include <libyul/Dialect.h>

namespace puyasol::builder
{

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

namespace
{
bool hasModifierDefinition(solidity::frontend::FunctionDefinition const& function)
{
	for (auto const& invocation: function.modifiers())
		if (dynamic_cast<solidity::frontend::ModifierDefinition const*>(
				invocation->name().annotation().referencedDeclaration))
			return true;
	return false;
}

bool hasFunctionPointerParameter(
	solidity::frontend::FunctionDefinition const& function)
{
	for (auto const& parameter: function.parameters())
		if (dynamic_cast<solidity::frontend::FunctionType const*>(
				parameter->type()))
			return true;
	return false;
}

} // namespace

void AWSTBuilder::collectHostBoundFunctions()
{
	std::map<int64_t, solidity::frontend::FunctionDefinition const*> candidates;
	auto consider = [&](solidity::frontend::FunctionDefinition const* function) {
		if (!function || !function->isImplemented() || function->isConstructor())
			return;
		auto const* scope = function->annotation().contract;
		if (!function->isFree() && (!scope || !scope->isLibrary()))
			return;
		if (m_session.analysis.hasReachabilityGraphs
			&& !m_session.analysis.reachableFunctionIds.count(function->id()))
			return;

		candidates.emplace(function->id(), function);

		bool const needsConcreteHost = hasFunctionPointerParameter(*function)
			|| hasModifierDefinition(*function)
			|| (!m_session.profile.evmStorageLayout
				&& m_session.analysis.callablesWithStorageAssembly.count(
					function->id()));
		if (needsConcreteHost)
			m_hostBoundFunctionIds.insert(function->id());
	};

	for (auto const& [_, function]: m_session.analysis.functionDeclarations)
		consider(function);

	// A root subroutine cannot call an instance method. Pull callers of a
	// host-bound function into the same concrete contract until the set closes.
	std::vector<int64_t> pending(m_hostBoundFunctionIds.begin(), m_hostBoundFunctionIds.end());
	for (size_t i = 0; i < pending.size(); ++i)
		if (auto it = m_session.analysis.callableCallers.find(pending[i]);
			it != m_session.analysis.callableCallers.end())
			for (auto caller: it->second)
				if (candidates.count(caller) && m_hostBoundFunctionIds.insert(caller).second)
					pending.push_back(caller);

	for (auto const& [id, function]: candidates)
		if (m_hostBoundFunctionIds.count(id))
			m_hostBoundFunctions.push_back(function);
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
	m_hostBoundFunctionIds.clear();
	m_selectorContracts.clear();
	// solc emits colliding contract names under their fully qualified name
	// (CompilerStack::filesystemFriendlyName); unique names stay plain. The
	// backend derives artifact stems from the AWST contract name, so apply the
	// same rule here, sanitized to the artifact-stem alphabet.
	m_artifactNames.clear();
	for (auto const& qualifiedName: _compiler.contractNames())
	{
		std::string friendly = _compiler.filesystemFriendlyName(qualifiedName);
		std::string const plain = qualifiedName.substr(qualifiedName.rfind(':') + 1);
		if (friendly == plain)
			continue;
		for (auto& c: friendly)
			if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_'))
				c = '_';
		if (!friendly.empty() && std::isdigit(static_cast<unsigned char>(friendly.front())))
			friendly.insert(friendly.begin(), '_');
		m_artifactNames[qualifiedName] = friendly;
	}
	for (auto const& sourceName: _compiler.sourceNames())
		for (auto const* contract: solidity::frontend::ASTNode::filteredNodes<
			solidity::frontend::ContractDefinition>(_compiler.ast(sourceName).nodes()))
			if (contract && !contract->isInterface() && !contract->abstract()
				&& !contract->isLibrary())
				m_selectorContracts.push_back(contract);
	std::vector<std::shared_ptr<awst::RootNode>> roots;

	registerFunctionIds(_compiler, m_functionSymbols);
	presetDispatchCref(_compiler, m_session.functionPointers);
	collectHostBoundFunctions();
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

	validateAwstRoots(roots);
	// Root subroutines (libraries, free functions) need the same dead-code
	// pass as contract methods: an asm `return()` ending a library body
	// leaves the synthesized epilogue unreachable, which puya rejects.
	for (auto& root: roots)
		if (auto* sub = dynamic_cast<awst::Subroutine*>(root.get()))
			if (sub->body)
				awst::removeDeadCode(sub->body->body);
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
				// Compiler-recognised stdlib facades are consumed at their call sites;
				// their reverting safety-net bodies are not executable subroutines.
				if (eb::Arc4Stdlib::isFacadeFunction(*func)
					|| eb::AsaIntrinsics::isBitsBitlenFacade(*func))
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

				// Modifier chains and function-pointer/storage-assembly callees need
				// instance-method targets. collectHostBoundFunctions also closes this
				// set over root callers, so no emitted SubroutineID is left dangling.
				if (m_hostBoundFunctionIds.count(func->id()))
				{
					if (hasFunctionPointerParameter(*func)
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
			if (m_hostBoundFunctionIds.count(func->id()))
			{
				Logger::instance().debug(
					"Registering host-bound free function: " + qualifiedName);
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

/// Root subroutines consume the same physical parameter plan as hosted methods.
void AWSTBuilder::buildFreestandingParams(
	solidity::frontend::FunctionDefinition const& function,
	std::string const& sourceFile, awst::Subroutine& sub)
{
	auto const& plan = m_session.typeMapper.callBoundaryPlan(function);
	for (auto const& parameter: plan.parameters)
		sub.args.push_back({parameter.name,
			m_session.sourceMap.toAwstLoc(sourceFile, parameter.declaration->location()), parameter.type});
	for (auto pi: plan.offsetParams)
	{
		auto const& parameter = plan.parameters[pi];
		sub.args.push_back({parameter.offsetName(),
			m_session.sourceMap.toAwstLoc(sourceFile, parameter.declaration->location()), awst::WType::uint64Type()});
	}
}

/// buildFreestandingSubroutine phase: register param context on the FunctionContext (mapping-key params, slot handles, asm …
void AWSTBuilder::registerFreestandingParamContext(
	solidity::frontend::FunctionDefinition const& _func,
	sol_ast::FunctionContext& fnCtx,
	awst::Subroutine const& sub,
	std::set<size_t> const& slotParams,
	std::set<size_t> const& mappingStorageParams,
	std::set<size_t> const& blobAggParams,
	std::set<size_t> const& evmSlotRefParams)
{
	// Register mapping-storage-ref params (must be after FunctionContext push;
	// setMappingKeyParam writes into nearestFunction(currentScope)).
	for (size_t idx: mappingStorageParams)
	{
		auto const& param = _func.parameters()[idx];
		fnCtx.setMappingKeyParam(param->id(), param->name());
	}
	auto const& plan = m_session.typeMapper.callBoundaryPlan(_func);
	for (auto pi: plan.offsetParams)
		fnCtx.setStructRefOffset(plan.parameters[pi].declaration->id(), plan.parameters[pi].offsetName());

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
		fnCtx.returnType = sub.returnType;
		fnCtx.paramBitWidths = bitWidths;
		fnCtx.boxKeyStructParams = std::move(boxKeyStructParams);
	}

}

/// buildFreestandingSubroutine phase: register storage/blob return params and blob param offsets on the FunctionContext.
void AWSTBuilder::registerFreestandingReturnParams(
	solidity::frontend::FunctionDefinition const& _func,
	sol_ast::FunctionContext& fnCtx,
	std::set<size_t> const& blobAggParams)
{
	auto const& returnParams = _func.returnParameters();
	// Register mapping storage-ref return params (e.g. `returns (mapping(K=>V) storage r)`):
	// r[k] box-accesses using r's runtime bytes value as the holder prefix.
	// Slot mode: named storage returns are biguint slot handles instead.
	for (auto const& rp: returnParams)
	{
		if (rp->referenceLocation() != solidity::frontend::VariableDeclaration::Location::Storage
			|| rp->name().empty())
			continue;
		if (m_session.profile.evmStorageLayout || storageRefReturnUsesSlot(&_func, m_session.analysis))
			fnCtx.setSlotStorageRef(rp->id(), awst::makeVarExpression(
				rp->name(), awst::WType::biguintType(), awst::SourceLocation{}));
		else if (dynamic_cast<solidity::frontend::MappingType const*>(rp->type())
			|| storageRefReturnIsBytesKeyed(&_func, m_session.analysis))
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

}

/// buildFreestandingSubroutine phase: zero-initialize named return variables (Solidity implicit init) + blob-backed memory-return …
void AWSTBuilder::prependFreestandingReturnInits(
	solidity::frontend::FunctionDefinition const& _func,
	awst::Subroutine& sub,
	awst::SourceLocation const& loc)
{
	// Only blob-backed (>4KB) memory returns bind an FMP base offset + bump.
	emitNamedReturnInits(
		*sub.body, _func, m_session.typeMapper,
		/*_skipValueInits=*/false,
		/*_memoryBumpMinBytes=*/AssemblyBuilder::SLOT_SIZE, loc);
}



/// buildFreestandingSubroutine phase: synthesize the implicit fall-through return (augmented args / named values / default zero).
void AWSTBuilder::synthesizeFreestandingImplicitReturn(
	solidity::frontend::FunctionDefinition const& _func,
	awst::Subroutine& sub,
	sol_ast::FunctionContext& fnCtx,
	std::vector<size_t> const& storageParamIndices,
	std::vector<size_t> const& memoryRefParamIndices,
	awst::SourceLocation const& loc)
{
	ImplicitReturnShape shape;
	shape.hasReturnValue = !_func.returnParameters().empty()
		|| !storageParamIndices.empty() || !memoryRefParamIndices.empty();
	shape.storageParamIndices = &storageParamIndices;
	shape.memoryRefParamIndices = &memoryRefParamIndices;
	shape.args = &sub.args;
	shape.blobReturnsAsOffset = true;
	emitImplicitReturn(
		*sub.body, sub.returnType, _func, m_session.typeMapper, fnCtx, shape, loc);
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

	auto const& plan = m_session.typeMapper.callBoundaryPlan(_func);
	auto const& slotParams = plan.asmSlotParams;
	auto const& mappingStorageParams = plan.keyParams;
	auto const& blobAggParams = plan.blobParams;
	auto const& evmSlotRefParams = plan.slotParams;
	auto const& storageParamIndices = plan.storageWriteBackParams;
	auto const& memoryRefParamIndices = plan.memoryWriteBackParams;
	buildFreestandingParams(_func, _sourceFile, *sub);
	sub->returnType = plan.augmentReturn(m_session.typeMapper,
		m_session.typeMapper.functionReturnPlan(_func).internalType);

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

	registerFreestandingParamContext(_func, fnCtx, *sub, slotParams,
		mappingStorageParams, blobAggParams, evmSlotRefParams);

	// Construct the function-body block context for the body.
	auto blk = sol_ast::BlockContext::top(fnCtx);
	auto blkGuard = exprBuilder.pushScopeRaii(&blk);

	registerFreestandingReturnParams(_func, fnCtx, blobAggParams);

	// Promote memory aggregates used as asm-pointers (bytes/string buffers in
	// internal/library functions, e.g. OZ Strings.toString) to blob-backed before
	// body translation — the contract-method path does this in ContractBuilder's
	// buildBlock; the free/library path builds the body directly, so mark here too.
	markAssemblyAggregates(fnCtx, _func.body());

	// EVM blob memory: spill asm-pointer memory params (the LIBRARY path —
	// Morpho's MarketParamsLib.id(), Solady's LibString helpers, ...).
	std::vector<std::shared_ptr<awst::Statement>> asmParamSpills;
	emitAsmParamSpills(m_session.typeMapper, fnCtx, _func.body(), _sourceFile,
		asmParamSpills);

	// OZ ERC1967Utils function-level folds (proxy.md §1/§3): the real bodies
	// reach the 1967 slots through StorageSlot (the escaped-slot shape) and
	// would drag the storage-dispatch runtime + delegatecall into the demand
	// graph; each has an exact native meaning instead.
	if (auto fold = proxies::Erc1967Lowering::classifyUtilsFunction(_func);
		fold != proxies::Erc1967Lowering::UtilsFold::None)
		sub->body = proxies::Erc1967Lowering::utilsFoldBody(
			fold, sub->returnType, sub->args, m_session.artifacts, loc);
	else
		sub->body = sol_ast::buildBlock(blk, _func.body());
	if (!asmParamSpills.empty())
		sub->body->body.insert(sub->body->body.begin(),
			std::make_move_iterator(asmParamSpills.begin()),
			std::make_move_iterator(asmParamSpills.end()));

	prependFreestandingReturnInits(_func, *sub, loc);

	plan.augmentReturns(*sub->body, sub->returnType);

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

	synthesizeFreestandingImplicitReturn(
		_func, *sub, fnCtx, storageParamIndices, memoryRefParamIndices, loc);

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
			translator.setArtifactNames(m_artifactNames);
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
			translator.setArtifactNames(m_artifactNames);
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
