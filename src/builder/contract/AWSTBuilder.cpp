#include <unordered_set>
#include "builder/solc/SourceLocConvert.h"
#include "builder/contract/AWSTBuilder.h"
#include "builder/solc/SolcFacts.h"
#include "builder/types/RefParamPassing.h"
#include "builder/target/EvmLayoutMode.h"
#include "builder/types/SolIntType.h"
#include "awst/Termination.hpp"
#include "awst/StatementWalk.h"
#include "builder/solc/FunctionIdentity.h"
#include "builder/context/SubroutineRegistry.hpp"
#include "builder/lowering/abi/Arc4Stdlib.h"
#include "builder/lowering/intrinsics/Ripemd160Builder.h"
#include "builder/lowering/intrinsics/AsaIntrinsics.h"
#include "builder/solc/StorageRefPointer.h"
#include "builder/storage/StorageMapper.h"
#include "builder/ast/SolStatement.h"
#include "builder/contract/ContractBuilder.h"
#include "builder/contract/ReturnFinishing.h"
#include "builder/solc/OverloadSuffix.h"
#include "builder/lowering/calls/FunctionPointerBuilder.h"
#include "builder/yul/AssemblyBuilder.h"
#include "builder/lowering/proxies/Erc1967Lowering.h"
#include "builder/codec/Arc4Defaults.h"
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
			&& !m_session.analysis.reachableCallableIds.count(function->id()))
			return;

		candidates.emplace(function->id(), function);

		bool const needsConcreteHost = hasFunctionPointerParameter(*function)
			|| hasModifierDefinition(*function)
			|| (!m_session.profile.evmStorageLayout
				&& m_session.analysis.callablesWithStorageSlotAccess.count(
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
	std::map<std::string, std::string> const& _sourceAliases,
	TargetProfile _targetProfile
)
{
	awst::NameGen::Scope namingScope;
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
	for (auto const* contract: m_session.analysis.contracts)
			if (contract && !contract->isInterface() && !contract->abstract()
				&& !contract->isLibrary())
				m_selectorContracts.push_back(contract);
	std::vector<std::shared_ptr<awst::RootNode>> roots;
	auto collectYulSubroutines = [&] {
		for (auto& sub: m_session.artifacts.pendingYulSubroutines)
			roots.push_back(std::move(sub));
		m_session.artifacts.pendingYulSubroutines.clear();
	};

	if (!m_selectorContracts.empty())
		m_session.functionPointers.currentCref = m_selectorContracts.front()->fullyQualifiedName();
	collectHostBoundFunctions();
	translateFreestandingFunctions(_sourceFile, roots);
	// Root helpers must survive the per-contract sink reset below. Host-bound
	// helpers are drained and storage-scoped by their own ContractBuilder.
	collectYulSubroutines();
	translateContracts(_sourceFile, _opupBudget, _ensureBudget, roots);

	// Callees specialized on an interior field path (requested by call sites
	// above; a specialized body may request further ones).
	for (size_t i = 0; i < m_session.artifacts.pendingPathSpecializations.size(); ++i)
	{
		auto const spec = m_session.artifacts.pendingPathSpecializations[i];
		auto const* scope = spec.function->annotation().contract;
		std::string const libraryName = scope && scope->isLibrary() ? scope->name() : std::string{};
		Logger::instance().debug("Translating path-specialized callee: " + spec.id);
		roots.push_back(buildFreestandingSubroutine(*spec.function, _sourceFile, spec.id, spec.id, libraryName, &spec));
	}
	collectYulSubroutines();

	// Builtin helpers are requested by their lowering sites, so unused
	// algorithms never enter the root set.
	for (auto const& [_, sub]: m_session.artifacts.bufferSubroutines)
		roots.push_back(sub);
	if (m_session.artifacts.needsRipemd160)
	{
		awst::SourceLocation builtinLoc(_sourceFile);
		roots.push_back(builder::builtin::buildRipemd160Subroutine(builtinLoc));
	}

	validateAwstRoots(roots);
	// Root subroutines (libraries, free functions) need the same dead-code
	// pass as contract methods: an asm `return()` ending a library body
	// leaves the synthesized epilogue unreachable, which puya rejects.
	for (auto& root: roots)
	{
		root->typeArena = m_session.typeMapper.typeArena();
		if (auto* lsig = dynamic_cast<awst::LogicSignature*>(root.get()))
			lsig->program->typeArena = root->typeArena;
		if (auto* sub = dynamic_cast<awst::Subroutine*>(root.get()))
			if (sub->body)
				awst::removeDeadCode(sub->body->body);
	}
	return roots;
}


void AWSTBuilder::translateFreestandingFunctions(
	std::string const& sourceFile, std::vector<std::shared_ptr<awst::RootNode>>& roots)
{
	for (auto const& [id, function]: m_session.analysis.functionDeclarations)
	{
		auto const* owner = function->annotation().contract;
		bool const library = owner && owner->isLibrary();
		if ((!function->isFree() && !library) || !function->isImplemented()
			|| function->isConstructor() || eb::Arc4Stdlib::isFacadeFunction(*function)
			|| m_session.analysis.avmIntrinsics.contains(id))
			continue;
		if (m_session.analysis.hasReachabilityGraphs
			&& !m_session.analysis.reachableCallableIds.contains(id)) continue;
		std::string ownerName = library ? owner->name() : "";
		std::string name = library ? ownerName + "." + function->name() : function->name();
		if (m_hostBoundFunctionIds.contains(id))
		{
			if (library && hasFunctionPointerParameter(*function)
				&& function->visibility() == solidity::frontend::Visibility::External)
				Logger::instance().warning("external library function " + name
					+ " internalized into its host; AVM has no DELEGATECALL semantics",
					m_session.sourceMap.toAwstLoc(sourceFile, function->location()));
			continue;
		}
		auto symbol = functionSymbol(*function);
		if (!symbol) throw std::logic_error("Missing function declaration identity: " + name);
		Logger::instance().debug("Translating freestanding function: " + name);
		roots.push_back(buildFreestandingSubroutine(*function, sourceFile, name, *symbol, ownerName));
	}
}

/// Root subroutines consume the same physical parameter plan as hosted methods.
void AWSTBuilder::buildFreestandingParams(
	solidity::frontend::FunctionDefinition const& function,
	std::string const& sourceFile, awst::Subroutine& sub)
{
	auto const& plan = m_session.typeMapper.callBoundaryPlan(function);
	for (auto const& parameter: plan.parameters)
		sub.args.emplace_back(parameter.name, parameter.type,
			m_session.sourceMap.toAwstLoc(
				sourceFile, parameter.declaration->location()));
	for (auto pi: plan.offsetParams)
	{
		auto const& parameter = plan.parameters[pi];
		sub.args.emplace_back(parameter.offsetName(), awst::WType::uint64Type(),
			m_session.sourceMap.toAwstLoc(
				sourceFile, parameter.declaration->location()));
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
	awst::SourceLocation const& loc)
{
	ImplicitReturnShape shape;
	shape.hasReturnValue = !_func.returnParameters().empty();
	shape.blobReturnsAsOffset = true;
	emitImplicitReturn(
		*sub.body, sub.returnType, _func, m_session.typeMapper, fnCtx, shape, loc);
}

std::shared_ptr<awst::Subroutine> AWSTBuilder::buildFreestandingSubroutine(
	solidity::frontend::FunctionDefinition const& _func,
	std::string const& _sourceFile,
	std::string const& _qualifiedName,
	std::string const& _subroutineId,
	std::string const& _libraryName,
	BuildArtifacts::PathSpecialization const* _pathSpec)
{
	auto sub = std::make_shared<awst::Subroutine>();
	sub->inlineOpt = false; // Prevent puya from inlining large subroutines

	// EIP-1967 admin-slot uses lowered in this body attach to the contracts
	// whose call graphs reach this function, not to whichever contract builds
	// first (BuildArtifacts::noteErc1967AdminUse).
	solidity::ScopedSaveAndRestore freestandingIdGuard(
		m_session.artifacts.currentFreestandingFunctionId, _func.id());

	awst::SourceLocation loc = m_session.sourceMap.toAwstLoc(
		_sourceFile, _func.location());

	sub->sourceLocation = loc;
	sub->id = _subroutineId;
	sub->name = _qualifiedName;

	// Documentation
	if (_func.documentation())
		sub->documentation.description = *_func.documentation()->text();

	auto const& plan = m_session.typeMapper.callBoundaryPlan(_func);
	buildFreestandingParams(_func, _sourceFile, *sub);
	sub->returnType = m_session.typeMapper.functionReturnPlan(_func).internalType;

	sub->pure = _func.stateMutability() == solidity::frontend::StateMutability::Pure;

	// Build body. ContractContext stores overloadedNames as const& — must
	// pass a long-lived object (a temporary `{}` would dangle → SIGSEGV).
	static std::unordered_set<std::string> const EMPTY_OVERLOAD_NAMES;
	eb::ContractContext exprBuilder(
		m_session.typeMapper, *m_storageMapper, _sourceFile, _libraryName,
		EMPTY_OVERLOAD_NAMES, m_session.functionPointers
	);
	exprBuilder.selectorContracts = m_selectorContracts;

	sol_ast::TranslationContext tr{exprBuilder, m_session.typeMapper, _sourceFile};
	auto trGuard = exprBuilder.pushScopeRaii(&tr.scope);
	sol_ast::FunctionContext fnCtx{tr, _func, sub->args, sub->returnType};
	auto fnGuard = exprBuilder.pushScopeRaii(&fnCtx.scope);

	// Construct the function-body block context for the body.
	sol_ast::BlockContext blk{fnCtx};
	auto blkGuard = exprBuilder.pushScopeRaii(&blk.scope);

	// Path specialization: the reference param aliases the field path inside
	// the enclosing box the caller passed (its key travels as the bytes param).
	if (_pathSpec)
		for (auto const& specParam: _pathSpec->params)
		{
			auto const& param = _func.parameters()[specParam.index];
			auto keyExpr = awst::makeReinterpretCast(
				awst::makeVarExpression(param->name(), awst::WType::bytesType(), loc),
				awst::WType::boxKeyType(), loc);
			std::shared_ptr<awst::Expression> cursor = StorageMapper::makeStateGetWithDefault(
				awst::makeBoxValueExpression(std::move(keyExpr), specParam.enclosingWType, loc),
				specParam.enclosingWType, loc);
			for (auto const& member: specParam.path)
			{
				auto const* structType = dynamic_cast<awst::ARC4Struct const*>(cursor->wtype);
				awst::WType const* fieldType = structType ? awst::structFieldType(structType, member) : nullptr;
				if (!fieldType)
					throw SizeError("path specialization: `" + member + "` is not a field of the enclosing storage struct");
				cursor = awst::makeFieldExpression(std::move(cursor), member, fieldType, loc);
			}
			fnCtx.scope.bindings.mappingKeyParams.set(param->id(), std::string{});
			blk.scope.bindings.storageAliases.set(param->id(), sol_ast::StorageAlias::fieldPath(std::move(cursor)));
		}

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
	auto const& proxyFunctions = m_session.analysis.proxy.utilsFunctions;
	if (auto fold = proxyFunctions.find(_func.id()); fold != proxyFunctions.end())
		sub->body = proxies::Erc1967Lowering::utilsFoldBody(
			fold->second, sub->returnType, sub->args, m_session.artifacts, loc);
	else
		sub->body = sol_ast::buildBlock(blk, _func.body());
	if (!asmParamSpills.empty())
		sub->body->body.insert(sub->body->body.begin(),
			std::make_move_iterator(asmParamSpills.begin()),
			std::make_move_iterator(asmParamSpills.end()));

	prependFreestandingReturnInits(_func, *sub, loc);

	synthesizeFreestandingImplicitReturn(
		_func, *sub, fnCtx, loc);
	sub->returnType = plan.augmentReturn(m_session.typeMapper, sub->returnType);
	plan.augmentReturns(*sub->body, sub->returnType, m_session.typeMapper, fnCtx.originalMemoryParams);

	// 1967 slot constants surviving in a library/free body escaped into
	// runtime data flow (the OZ StorageSlot shape) — warn here; contract
	// bodies get the same scan in ContractBuilder.
	if (sub->body && m_session.typeMapper.profile().proxyAdaptation)
	{
		std::set<builder::proxies::Erc1967Slot> warned;
		builder::proxies::Erc1967Lowering::warnEscapedSlotConstants(
			*sub->body, warned);
	}

	return sub;
}

namespace
{

/// Library with an externally-callable implemented function: EVM deploys such
/// a library itself when the unit has no deployable contract.
bool isDeployableLibrary(solidity::frontend::ContractDefinition const& _library)
{
	using namespace solidity::frontend;
	bool entry = false;
	for (auto const* f: _library.definedFunctions())
		if (f->isImplemented() && !f->isConstructor()
			&& (f->visibility() == solidity::frontend::Visibility::Public
				|| f->visibility() == solidity::frontend::Visibility::External))
		{
			entry = true;
			auto portable = [](auto const& parameters) {
				return std::all_of(parameters.begin(), parameters.end(), [](auto const& parameter) {
					return parameter->referenceLocation() != VariableDeclaration::Location::Storage
						&& bool(parameter->type()->interfaceType(false));
				});
			};
			if (!portable(f->parameters()) || !portable(f->returnParameters()))
			{
				Logger::instance().warning("Library " + _library.fullyQualifiedName()
					+ " requires host-only reference parameters; no standalone application artifact is emitted");
				return false;
			}
		}
	return entry;
}

/// Resolve the bundled marker declaration once through solc inheritance.
solidity::frontend::ModifierDefinition const* logicSigMarker(
	solidity::frontend::ContractDefinition const& contract)
{
	using namespace solidity::frontend;
	for (auto const* base: contract.annotation().linearizedBaseContracts)
		if (base->sourceUnitName() == "libs/AVM.sol" && base->name() == "LogicSig"
			&& base->abstract() && base->functionModifiers().size() == 1)
		{
			auto const* marker = base->functionModifiers().front();
			if (marker->name() == "logicsig" && marker->parameters().empty()
				&& marker->isImplemented() && marker->body().statements().size() == 1
				&& dynamic_cast<PlaceholderStatement const*>(marker->body().statements()[0].get()))
				return marker;
		}
	return nullptr;
}

/// Public entry selection uses resolved solc declarations, including inheritance
/// and overloads. A stateless program has no ABI arguments and returns bool.
awst::ContractMethod const* logicSigEntry(
	solidity::frontend::ContractDefinition const& contract, awst::Contract const& translated)
{
	using namespace solidity::frontend;
	auto const* marker = logicSigMarker(contract);
	std::map<int64_t, FunctionDefinition const*> entries, marked;
	for (auto const& [_, type]: contract.interfaceFunctionList())
		if (type->hasDeclaration())
			if (auto const* function = dynamic_cast<FunctionDefinition const*>(&type->declaration()))
			{
				function = &function->resolveVirtual(contract);
				entries.emplace(function->id(), function);
				for (auto const& invocation: function->modifiers())
					if (SolcFacts::resolveModifier(*invocation, &contract) == marker)
						marked.emplace(function->id(), function);
			}
	auto const& candidates = marked.empty() ? entries : marked;
	if (candidates.size() != 1) return nullptr;
	auto const& function = *candidates.begin()->second;
	if (!function.parameters().empty() || function.returnParameters().size() != 1
		|| !dynamic_cast<BoolType const*>(function.returnParameters()[0]->type()))
		return nullptr;
	// The unique zero-parameter declaration may have an overload suffix.
	for (auto const& method: translated.methods)
		if ((method.memberName == function.name() || method.memberName == function.name() + "()")
			&& method.args.empty() && method.returnType == awst::WType::boolType())
			return &method;
	return nullptr;
}

std::shared_ptr<awst::LogicSignature> makeLogicSignature(
	awst::Contract const& _awstContract, awst::ContractMethod const& _entry)
{
	auto program = std::make_shared<awst::Subroutine>();
	program->sourceLocation = _entry.sourceLocation;
	program->id = _awstContract.id;
	program->name = _entry.memberName;
	program->args = _entry.args;
	program->returnType = _entry.returnType;
	program->body = _entry.body;
	program->documentation = _entry.documentation;
	program->pure = _entry.pure;

	auto lsig = std::make_shared<awst::LogicSignature>();
	lsig->sourceLocation = _awstContract.sourceLocation;
	lsig->id = _awstContract.id;
	lsig->shortName = _awstContract.name;
	lsig->program = std::move(program);
	lsig->docstring = _awstContract.description;
	lsig->reservedScratchSpace = _awstContract.reservedScratchSpace;
	lsig->avmVersion = _awstContract.avmVersion;
	return lsig;
}

/// LogicSig: AVM lsig instead of stateful app. The entry function (logicsig
/// modifier, or sole public method) becomes the program; app state /
/// inner-txns hard-fail downstream. Returns whether a root was emitted.
bool emitLogicSignature(
	solidity::frontend::ContractDefinition const& _contract,
	awst::Contract const& _awstContract,
	std::vector<std::shared_ptr<awst::RootNode>>& _roots)
{
	auto const* entry = logicSigEntry(_contract, _awstContract);
	if (!entry)
	{
		Logger::instance().error(
			"contract `" + _contract.name() + "` is LogicSig but has no single "
			"public/external zero-argument bool entry — mark exactly one with the bundled `logicsig` modifier",
			_awstContract.sourceLocation);
		return false;
	}
	auto lsig = makeLogicSignature(_awstContract, *entry);
	// A LogicSignature has no instance methods. Retain only its reachable helper
	// closure and relocate instance calls to unique root subroutine identities.
	std::map<std::string, awst::ContractMethod const*> methods;
	for (auto const& method: _awstContract.methods) methods.emplace(method.memberName, &method);
	std::vector<std::shared_ptr<awst::Subroutine>> pending{lsig->program};
	std::set<std::string> included;
	for (size_t i = 0; i < pending.size(); ++i)
		awst::visitExpressions(*pending[i]->body, [&](awst::Expression& expression) {
			auto* call = dynamic_cast<awst::SubroutineCallExpression*>(&expression);
			if (!call) return;
			auto const* target = std::get_if<awst::InstanceMethodTarget>(&call->target);
			if (!target) return;
			std::string name = target->memberName;
			auto found = methods.find(name);
			if (found == methods.end()) throw std::logic_error("Unresolved LogicSig helper: " + name);
			std::string id = _awstContract.id + ".lsig." + name;
			call->target = awst::SubroutineID{id};
			if (included.insert(name).second)
			{
				auto sub = makeLogicSignature(_awstContract, *found->second)->program;
				sub->id = id;
				pending.push_back(std::move(sub));
			}
		});
	for (size_t i = 1; i < pending.size(); ++i) _roots.push_back(std::move(pending[i]));
	_roots.push_back(std::move(lsig));
	Logger::instance().info("Emitted LogicSignature: " + _contract.name());
	return true;
}

bool hasArc4Method(awst::Contract const& _awstContract)
{
	for (auto const& method: _awstContract.methods)
		if (method.arc4MethodConfig.has_value())
			return true;
	return false;
}

/// Constructor-only contracts need a dummy ARC4 method so puya's router has
/// something to route (constructor runs at create time).
void appendDummyArc4Method(awst::Contract& _awstContract)
{
	awst::ContractMethod dummy;
	dummy.sourceLocation = _awstContract.sourceLocation;
	dummy.cref = _awstContract.id;
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

	_awstContract.methods.push_back(std::move(dummy));
}

/// Only emit contracts with public methods or a constructor. Non-deployable
/// contracts (internal-only, e.g. ErrorReporter) are translated for MRO
/// resolution but not emitted to AWST. Returns whether a root was emitted.
bool emitDeployableContract(
	solidity::frontend::ContractDefinition const& _contract,
	std::shared_ptr<awst::Contract> _awstContract,
	std::vector<std::shared_ptr<awst::RootNode>>& _roots)
{
	bool hasPublicMethod = hasArc4Method(*_awstContract);
	if (!hasPublicMethod && !_contract.abstract())
	{
		appendDummyArc4Method(*_awstContract);
		hasPublicMethod = true;
	}
	if (!hasPublicMethod)
	{
		Logger::instance().debug("Skipping non-deployable contract: " + _contract.name());
		return false;
	}
	eliminateDeadCode(*_awstContract);
	_roots.push_back(std::move(_awstContract));
	return true;
}

} // namespace

bool AWSTBuilder::prescanEvmStorageLayout()
{
	if (!m_session.profile.evmStorageLayout) return false;
	for (auto const* contract: m_session.analysis.contracts)
		if (contract && !contract->isInterface() && m_session.storagePlan(*contract).needsDispatch())
			return true;
	// Root library/free functions have no concrete host. Their storage calls
	// use the generic runtime; unrelated roots never change its implementation.
	for (auto id: m_session.analysis.callablesWithStorageSlotAccess)
		if (!m_session.analysis.hasReachabilityGraphs || m_session.analysis.reachableCallableIds.count(id))
			return true;
	return false;
}

std::shared_ptr<awst::Contract> AWSTBuilder::translateContract(
	solidity::frontend::ContractDefinition const& _contract,
	std::string const& _sourceFile,
	uint64_t _opupBudget,
	std::map<std::string, uint64_t> const& _ensureBudget,
	bool _evmStorageRuntimeNeeded,
	bool& _emittedEvmStorageRuntime,
	std::vector<std::shared_ptr<awst::RootNode>>& _roots)
{
	ContractBuilder translator(
		m_session.typeMapper, *m_storageMapper, m_session.functionPointers,
		_sourceFile,
		_opupBudget, _ensureBudget,
		m_hostBoundFunctions
	);
	translator.setArtifactNames(m_artifactNames);
	auto const& storagePlan = m_session.storagePlan(_contract);
	auto const emitEvmStorageRuntime = _evmStorageRuntimeNeeded
		&& !_emittedEvmStorageRuntime;
	auto awstContract = translator.build(
		_contract, storagePlan, emitEvmStorageRuntime);
	if (m_session.profile.evmStorageLayout && emitEvmStorageRuntime)
		_emittedEvmStorageRuntime = true;

	// Collect dispatch subroutines as root nodes so library
	// subroutines can resolve them via SubroutineID.
	for (auto& sub : translator.takeDispatchSubroutines())
		_roots.push_back(std::move(sub));
	return awstContract;
}

void AWSTBuilder::translateContracts(
	std::string const& sourceFile, uint64_t opupBudget,
	std::map<std::string, uint64_t> const& ensureBudget,
	std::vector<std::shared_ptr<awst::RootNode>>& roots)
{
	bool const storageRuntimeNeeded = prescanEvmStorageLayout();
	bool emittedStorageRuntime = false;
	for (auto const* contract: m_session.analysis.contracts)
	{
		if (contract->isInterface() || contract->abstract()) continue;
		if (contract->isLibrary() && !isDeployableLibrary(*contract)) continue;
		Logger::instance().info("Translating deployable root: " + contract->name());
		auto translated = translateContract(*contract, sourceFile, opupBudget, ensureBudget,
			storageRuntimeNeeded, emittedStorageRuntime, roots);
		if (logicSigMarker(*contract))
			emitLogicSignature(*contract, *translated, roots);
		else
			emitDeployableContract(*contract, std::move(translated), roots);
	}
}

} // namespace puyasol::builder
