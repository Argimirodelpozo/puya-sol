#include "builder/AWSTBuilder.h"
#include "builder/storage/EvmLayoutMode.h"
#include "builder/sol-types/SolIntType.h"
#include "awst/Termination.h"
#include "builder/FunctionIdRegistry.h"
#include "builder/SubroutineReachability.h"
#include "builder/builtin/Ripemd160Builder.h"
#include "builder/sol-ast/ParamMutationDetector.h"
#include "builder/sol-ast/StorageRefPointer.h"
#include "builder/sol-ast/AsmScan.h"
#include "builder/sol-ast/stmts/SolBlock.h"
#include "builder/contract/ContractBuilder.h"
#include "builder/contract/ReturnRewriter.h"
#include "builder/sol-types/OverloadSuffix.h"
#include "builder/itxn/FunctionPointerBuilder.h"
#include "builder/assembly/AssemblyBuilder.h"
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
	bool _viaYulBehavior
)
{
	m_storageMapper = std::make_unique<StorageMapper>(m_typeMapper);
	m_libraryFunctionIds.clear();
	std::vector<std::shared_ptr<awst::RootNode>> roots;

	// Populate the box-keyed-struct registry: a struct used as a mapping VALUE
	// anywhere is box-keyed; plain state-var/local structs stay by-value. This
	// is a compile-time calling-convention classifier only. Scans all contracts
	// because the mapping(=>Struct) and the library methods on Struct may be in
	// different source units (V4: orchestrator declares, Position library uses).
	{
		auto& reg = boxKeyedStructRegistry();
		reg.clear();
		std::set<solidity::frontend::Type const*> seen;
		for (auto const& sourceName: _compiler.sourceNames())
			for (auto const* contract: solidity::frontend::ASTNode::filteredNodes<
				solidity::frontend::ContractDefinition>(_compiler.ast(sourceName).nodes()))
				for (auto const* sv: contract->stateVariables())
					collectMappingValueStructs(sv->type(), reg, seen);
	}

	// Populate the ref-passed-struct registry: any struct appearing as a `T storage`
	// PARAMETER is boxed (shouldUseBoxStorage) so its ref travels as a box-key handle that
	// writes through into contract methods. Targeted (only ref-passed types) so never-ref-
	// passed structs keep their app-global layout. Scans contract methods + free/library fns.
	{
		using namespace solidity::frontend;
		auto& refReg = refPassedStructRegistry();
		refReg.clear();
		auto scan = [&](FunctionDefinition const* fn) {
			if (!fn) return;
			for (auto const& p: fn->parameters())
				if (p->referenceLocation() == VariableDeclaration::Location::Storage)
					if (auto const* st = dynamic_cast<StructType const*>(p->type()))
						refReg.insert(st->structDefinition().id());
		};
		// Only CONTRACT methods need boxing: libraries + free functions go through
		// buildFreestandingSubroutine, which augments storage-ref params (copy+write-back), so
		// they already write through. Boxing structs they take regresses library-modifier paths.
		for (auto const& sourceName: _compiler.sourceNames())
		{
			auto const& unit = _compiler.ast(sourceName);
			for (auto const* contract: ASTNode::filteredNodes<ContractDefinition>(unit.nodes()))
			{
				if (contract->isLibrary())
					continue;
				for (auto const* fn: contract->definedFunctions())
					scan(fn);
			}
		}
	}

	// Memory-reassignment registry: memory aggregate vars whole-var REASSIGNED (`b = …`)
	// anywhere → the b=a copy-elision alias (SolVariableDeclaration) is unsafe for them
	// (re-pointing one side would clobber the aliased local), so they fall back to a copy.
	{
		using namespace solidity::frontend;
		auto& reassigned = reassignedMemoryLocalsRegistry();
		reassigned.clear();
		struct ReassignWalker: ASTConstVisitor {
			std::set<int64_t>& out;
			explicit ReassignWalker(std::set<int64_t>& o): out(o) {}
			bool visit(Assignment const& a) override {
				if (auto const* id = dynamic_cast<Identifier const*>(&a.leftHandSide()))
					if (auto const* vd = dynamic_cast<VariableDeclaration const*>(
							id->annotation().referencedDeclaration))
						if (vd->referenceLocation() == VariableDeclaration::Location::Memory)
							out.insert(vd->id());
				return true;
			}
		} walker(reassigned);
		auto scanBody = [&](FunctionDefinition const* fn) {
			if (fn && fn->isImplemented()) fn->body().accept(walker);
		};
		for (auto const& sourceName: _compiler.sourceNames())
		{
			auto const& unit = _compiler.ast(sourceName);
			for (auto const* contract: ASTNode::filteredNodes<ContractDefinition>(unit.nodes()))
				for (auto const* fn: contract->definedFunctions())
					scanBody(fn);
			for (auto const* fn: ASTNode::filteredNodes<FunctionDefinition>(unit.nodes()))
				scanBody(fn);
		}
	}

	// Struct-ref offset-convention pre-pass (handle-model dual handle): a storage struct-ref param
	// that receives an ARRAY-ELEMENT ref (`f(arr[i])`) at any call site → mark it so the callee
	// gains a companion uint64 offset param and `s.field` writes the element slice (not the whole
	// array box). Whole-box callers of the same param pass offset 0. Conservative: calls whose
	// arg/param counts don't line up (default args, exotic using-for) are skipped → those params
	// stay whole-box (no regression, just no fix). Precise marking avoids over-boxing.
	{
		using namespace solidity::frontend;
		auto& reg = structRefOffsetParamsRegistry();
		reg.clear();
		struct OffsetWalker: ASTConstVisitor {
			std::set<int64_t>& out;
			explicit OffsetWalker(std::set<int64_t>& o): out(o) {}
			static bool isArrayElemStructRef(Expression const* e) {
				auto const* ia = dynamic_cast<IndexAccess const*>(e);
				if (!ia) return false;
				auto const* at = dynamic_cast<ArrayType const*>(ia->baseExpression().annotation().type);
				if (!at || at->isByteArrayOrString()) return false;
				return at->baseType() && at->baseType()->category() == Type::Category::Struct;
			}
			bool visit(FunctionCall const& fc) override {
				Declaration const* refDecl = nullptr;
				if (auto const* id = dynamic_cast<Identifier const*>(&fc.expression()))
					refDecl = id->annotation().referencedDeclaration;
				else if (auto const* ma = dynamic_cast<MemberAccess const*>(&fc.expression()))
					refDecl = ma->annotation().referencedDeclaration;
				auto const* fd = dynamic_cast<FunctionDefinition const*>(refDecl);
				if (fd) {
					auto const& args = fc.arguments();
					auto const& params = fd->parameters();
					size_t shift = 0;
					if (params.size() == args.size()) shift = 0;
					else if (params.size() == args.size() + 1) shift = 1; // using-for receiver = param 0
					else return true; // can't reliably map arg→param; skip
					for (size_t i = 0; i < args.size(); ++i) {
						size_t pIdx = i + shift;
						if (pIdx < params.size() && isArrayElemStructRef(args[i].get())
							&& params[pIdx]->referenceLocation()
								== VariableDeclaration::Location::Storage)
							out.insert(params[pIdx]->id());
					}
				}
				return true;
			}
		} offsetWalker(reg);
		auto scanOffsetBody = [&](FunctionDefinition const* fn) {
			if (fn && fn->isImplemented()) fn->body().accept(offsetWalker);
		};
		for (auto const& sourceName: _compiler.sourceNames())
		{
			auto const& unit = _compiler.ast(sourceName);
			for (auto const* contract: ASTNode::filteredNodes<ContractDefinition>(unit.nodes()))
				for (auto const* fn: contract->definedFunctions())
					scanOffsetBody(fn);
			for (auto const* fn: ASTNode::filteredNodes<FunctionDefinition>(unit.nodes()))
				scanOffsetBody(fn);
		}
	}

	registerFunctionIds(_compiler, _sourceFile, m_libraryFunctionIds, m_freeFunctionById);
	presetDispatchCref(_compiler, _sourceFile);
	translateLibraryFunctions(_compiler, _sourceFile, roots);
	translateFreeFunctions(_compiler, _sourceFile, roots);
	translateContracts(_compiler, _sourceFile, _opupBudget, _ensureBudget, _viaYulBehavior, roots);

	// Inject the synthetic RIPEMD-160 subroutine. Always emitted; the
	// reachability filter below drops it when no contract calls it.
	{
		awst::SourceLocation builtinLoc;
		builtinLoc.file = _sourceFile;
		roots.push_back(builder::builtin::buildRipemd160Subroutine(builtinLoc));
	}

	// Drop any subroutine root not reachable from a contract method.
	return filterToReachableSubroutines(std::move(roots));
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

				// AST-id-first lookup (precise overload resolution), name-based fallback.
				std::string qualifiedName = libraryName + "." + func->name();
				std::string subroutineId;
				auto byId = m_freeFunctionById.find(func->id());
				if (byId != m_freeFunctionById.end())
				{
					subroutineId = byId->second;
				}
				else
				{
					auto it = m_libraryFunctionIds.find(qualifiedName);
					if (it != m_libraryFunctionIds.end())
					{
						subroutineId = it->second;
					}
					else
					{
						std::string overloadName = qualifiedName + paramCountSuffix(*func);
						auto it2 = m_libraryFunctionIds.find(overloadName);
						if (it2 != m_libraryFunctionIds.end())
						{
							qualifiedName = overloadName;
							subroutineId = it2->second;
						}
						else
							subroutineId = _sourceFile + "." + qualifiedName;
					}
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
				if (hasFnParam)
				{
					if (func->visibility() == solidity::frontend::Visibility::External)
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
					Logger::instance().debug("Registering internalizable library function: " + qualifiedName);
					m_internalizableLibFuncs.push_back(func);
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

			std::string qualifiedName = func->name();
			std::string subroutineId;
			auto byId = m_freeFunctionById.find(func->id());
			if (byId != m_freeFunctionById.end())
				subroutineId = byId->second;
			else
			{
				auto it = m_libraryFunctionIds.find(qualifiedName);
				subroutineId = (it != m_libraryFunctionIds.end())
					? it->second
					: _sourceFile + "." + qualifiedName;
			}

			Logger::instance().debug("Translating free function: " + qualifiedName);
			roots.push_back(buildFreestandingSubroutine(
				*func, _sourceFile, qualifiedName, subroutineId, /*libraryName=*/""));
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

	awst::SourceLocation loc;
	loc.file = _sourceFile;
	loc.line = _func.location().start >= 0 ? _func.location().start : 0;
	loc.endLine = _func.location().end >= 0 ? _func.location().end : 0;

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
		arg.sourceLocation.file = _sourceFile;
		arg.sourceLocation.line = param->location().start >= 0 ? param->location().start : 0;
		arg.sourceLocation.endLine = param->location().end >= 0 ? param->location().end : 0;

		// Mapping storage refs (including array-of-mapping): callee receives
		// the caller's box key prefix as bytes so `m[k]` hashes against the
		// caller's storage var, not the param name. Without widening,
		// array-of-mapping params encode as their own "state var" and box
		// keys diverge from the auto-getter's reads.
		if (builder::evmStorageLayout()
			&& param->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Storage)
		{
			// --evm-storage-layout: every storage ref IS a biguint slot handle;
			// writes go straight to the slot space (no box keys, no write-back).
			arg.wtype = awst::WType::biguintType();
			evmSlotRefParams.insert(pi);
		}
		else if (param->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Storage
			&& (isBoxKeyedStorageRef(param->type()) || slotParams.count(pi))) // widened: plain structs + asm .slot refs
		{
			arg.wtype = awst::WType::bytesType();
			mappingStorageParams.insert(pi);
		}
		else if (param->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Memory
			&& memoryUsesBlob(m_typeMapper.map(param->type())))
		{
			// Memory aggregate >4KB → passed as uint64 base offset (pointer model).
			arg.wtype = awst::WType::uint64Type();
			blobAggParams.insert(pi);
		}
		else
			arg.wtype = m_typeMapper.map(param->type());
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
	if (isMutating && !isPrivate && !builder::evmStorageLayout())
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
		ParamMutationDetector detector;
		for (auto const& p : _func.parameters())
			detector.paramIds.insert(p->id());
		_func.body().accept(detector);
		memoryRefParamIndices = collectParamIndices(_func, [&](size_t pi) {
			auto const& p = _func.parameters()[pi];
			return p->referenceLocation()
					== solidity::frontend::VariableDeclaration::Location::Memory
				&& !blobAggParams.count(pi) // blob-backed via multi-slot blob; caller sees mutations directly
				&& p->type() && isMemRefType(p->type())
				&& detector.mutated.count(p->id()); // skip read-only — no need to thread post-call value back
		});
	}

	// Return type — augmented with storage/memory param types for write-back.
	auto const& returnParams = _func.returnParameters();
	{
		std::vector<awst::WType const*> types;
		for (auto const& rp: returnParams)
		{
			// >4KB memory return → blob-backed, returns uint64 base offset (pointer model).
			auto const* rpW = m_typeMapper.map(rp->type());
			if (builder::evmStorageLayout()
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
			sub->returnType = new awst::WTuple(std::move(types));
	}

	sub->pure = _func.stateMutability() == solidity::frontend::StateMutability::Pure;

	// Build body. ContractContext stores overloadedNames as const& — must
	// pass a long-lived object (a temporary `{}` would dangle → SIGSEGV).
	static std::unordered_set<std::string> const EMPTY_OVERLOAD_NAMES;
	eb::ContractContext exprBuilder(
		m_typeMapper, *m_storageMapper, _sourceFile, _libraryName, m_libraryFunctionIds,
		EMPTY_OVERLOAD_NAMES, m_freeFunctionById
	);

	sol_ast::TranslationContext tr{exprBuilder, m_typeMapper, _sourceFile};
	auto trGuard = exprBuilder.pushScopeRaii(&tr);
	sol_ast::FunctionContext fnCtx{tr, {}, sub->returnType, {}};
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
				: m_typeMapper.map(param->type());
			paramContext.emplace_back(pname, ptype);
			// Struct storage-ref param used via `.slot` in asm: record the ARC4
			// struct wtype so `param.slot` resolves to a BoxValueExpression over
			// the box-key handle (the bytes param value). Slot mode: the param
			// IS the biguint slot — no sentinel.
			if (slotParams.count(pi) && !builder::evmStorageLayout())
				boxKeyStructParams[pname] = m_typeMapper.map(param->type());
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
		if (builder::evmStorageLayout())
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
		auto const* rpTypeB = m_typeMapper.map(rp->type());
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

	sub->body = sol_ast::buildBlock(blk, _func.body());

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

		FunctionTranslationCtx ftCtx{
			m_typeMapper, exprBuilder, tr, _sourceFile,
			fnCtx.params, sub->returnType, fnCtx.paramBitWidths,
			std::move(namedReturnList), std::move(mappingKeyList),
			std::move(blobAggList),
			/*currentContract=*/nullptr,
		};
		inlineModifiers(ftCtx, _func, sub->body);
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
			if (builder::evmStorageLayout()
				&& rp->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Storage)
			{
				inits.push_back(awst::makeAssignmentStatement(
					awst::makeVarExpression(rp->name(), awst::WType::biguintType(), loc),
					awst::makeZero(loc, awst::WType::biguintType()), loc));
				continue;
			}
			auto* rpType = m_typeMapper.map(rp->type());

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
			auto const* rpTypeC = m_typeMapper.map(rp->type());
			int szC = computeEncodedElementSize(rpTypeC);
			if (szC <= AssemblyBuilder::SLOT_SIZE)
				continue;
			std::string offN = "__blobagg_off_" + std::to_string(rp->id());
			auto blobLoad = awst::makeLoadSlot(AssemblyBuilder::MEMORY_SLOT_FIRST, loc);
			auto base = awst::makeExtractUInt64(std::move(blobLoad),
				awst::makeIntegerConstant("88", loc), loc);
			inits.push_back(awst::makeAssignmentStatement(
				awst::makeVarExpression(offN, awst::WType::uint64Type(), loc),
				std::move(base), loc));
			for (auto& s: AssemblyBuilder::emitFreeMemoryBump(szC, loc, static_cast<int>(rp->id())))
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
		auto varA = awst::makeVarExpression(_func.parameters()[0]->name(), m_typeMapper.map(_func.parameters()[0]->type()), loc);
		auto varB = awst::makeVarExpression(_func.parameters()[1]->name(), m_typeMapper.map(_func.parameters()[1]->type()), loc);
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
				&& builder::evmMemoryLayout()
				&& returnParams[0]->referenceLocation()
					== solidity::frontend::VariableDeclaration::Location::Memory
				&& [&]{ auto const* at3 = dynamic_cast<
						solidity::frontend::ArrayType const*>(
						returnParams[0]->type());
					return at3 && at3->isByteArrayOrString(); }()
				&& builder::blockUsesDeclInAsm(_func.body(), returnParams[0]->id()))
			{
				implicitReturn->value =
					builder::AssemblyBuilder::materializeBlobBytesValue(
						"__blobagg_off_" + std::to_string(returnParams[0]->id()),
						dynamic_cast<solidity::frontend::ArrayType const*>(
							returnParams[0]->type())->isString(), loc);
			}
			else if (returnParams.size() == 1 && totalAugmented2 == 0)
			{
				auto const* rp0W = m_typeMapper.map(returnParams[0]->type());
				if (builder::evmStorageLayout()
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
					auto const* rpW = m_typeMapper.map(rp->type());
					if (builder::evmStorageLayout()
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
	bool emittedDeployable = false;
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
				m_typeMapper, *m_storageMapper, _sourceFile, m_libraryFunctionIds,
				_opupBudget, m_freeFunctionById, _ensureBudget, _viaYulBehavior,
				m_internalizableLibFuncs
			);
			auto awstContract = translator.build(*contract);

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
				m_typeMapper, *m_storageMapper, _sourceFile, m_libraryFunctionIds,
				_opupBudget, m_freeFunctionById, _ensureBudget, _viaYulBehavior,
				m_internalizableLibFuncs
			);
			auto awstContract = translator.build(*lib);
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
