#pragma once

#include "awst/Node.h"
#include "builder/sol-eb/ContractContext.h"
#include "builder/sol-ast/Context.h"
#include "builder/sol-ast/SolStatement.h"
#include "builder/storage/StorageMapper.h"
#include "builder/storage/StorageBackend.h"
#include "builder/storage/TransientStorage.h"
#include "builder/sol-types/TypeMapper.h"

#include <libsolidity/ast/AST.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace puyasol::builder
{

/// Maps "LibraryName.functionName" → subroutine ID string.
using LibraryFunctionIdMap = std::unordered_map<std::string, std::string>;

/// Maps AST node ID → subroutine ID for free functions (used by operator overloading).
using FreeFunctionIdMap = std::unordered_map<int64_t, std::string>;

/// Set of function names that have overloads (multiple definitions with same name).
using OverloadedNamesSet = std::unordered_set<std::string>;

/// Per-function translation state. Bundles everything `buildBlock` and
/// `inlineModifiers` need so the contract-method path (`ContractBuilder`)
/// and the library/free-function path (`AWSTBuilder`) can share the same
/// implementations instead of each carrying its own copy.
///
/// `currentContract` may be null when there is no enclosing contract
/// (libraries, file-level free functions). In that case the modifier
/// inliner skips virtual-override resolution, which is the only thing
/// it needs `currentContract` for.
struct FunctionTranslationCtx
{
	TypeMapper& typeMapper;
	eb::ContractContext& exprBuilder;
	sol_ast::TranslationContext& tr;
	std::string sourceFile;

	// Per-function state.
	std::vector<std::pair<std::string, awst::WType const*>> params;
	awst::WType const* returnType = nullptr;
	std::map<std::string, unsigned> paramBitWidths;
	std::vector<solidity::frontend::VariableDeclaration const*> namedReturns;
	std::vector<solidity::frontend::VariableDeclaration const*> mappingKeyParams;
	// Memory aggregate params >4KB: passed as their uint64 base offset into the
	// multi-slot blob (pointer model). `buildBlock` registers each as a blob
	// aggregate so `p.field[i]` in the body lowers to blob word access.
	std::vector<solidity::frontend::VariableDeclaration const*> blobAggParams;

	// Enclosing contract for modifier virtual-override lookup; nullable.
	solidity::frontend::ContractDefinition const* currentContract = nullptr;

	// True while building a constructor body (incl. base ctors and the
	// __postInit-deferred copy): flows into FunctionContext::inConstructor
	// so ctor-only semantics (e.g. msg.data is EMPTY during construction)
	// apply wherever the body ends up. NOT part of makeFunctionCtx's
	// positional aggregate init — assigned explicitly after construction.
	bool inConstructor = false;
	// Mirrors FunctionContext::frameIsProgram (internal/private function =>
	// assembly return() halts the program). Appended at struct end like
	// inConstructor — mid-struct insertion breaks aggregate init.
	bool frameIsProgram = false;
};

/// Make an `awst::SourceLocation` from a Solidity `SourceLocation`.
awst::SourceLocation makeLoc(
	std::string const& sourceFile,
	solidity::langutil::SourceLocation const& solLoc);

/// Translate a Solidity Block in the given function context.
///
/// If `placeholder` is non-null, any `_` (PlaceholderStatement) inside the
/// block gets replaced by it. Used during modifier body translation, where
/// `_` stands for the wrapped function body.
std::shared_ptr<awst::Block> buildBlock(
	FunctionTranslationCtx& ctx,
	solidity::frontend::Block const& block,
	std::shared_ptr<awst::Block> placeholder = nullptr);

/// Inline modifier bodies into a function body in place.
///
/// Walks `func.modifiers()` outermost-first; each iteration translates the
/// modifier's Solidity body with the current accumulator passed in as
/// `placeholder`, so `_` inside the modifier expands to the wrapped body.
/// The accumulator becomes the new body for the next iteration.
void inlineModifiers(
	FunctionTranslationCtx& ctx,
	solidity::frontend::FunctionDefinition const& func,
	std::shared_ptr<awst::Block>& body);

/// Builds an AWST Contract node from a Solidity ContractDefinition.
///
/// Orchestrates the translation of a complete contract including:
///   - Approval and clear-state programs
///   - Public/external methods with ARC4 method configs
///   - Constructor with inheritance specifier arguments
///   - Modifier inlining into function bodies
///   - Super call resolution across inheritance hierarchy
///   - Automatic __postInit generation for box-writing constructors
class ContractBuilder
{
public:
	ContractBuilder(
		TypeMapper& _typeMapper,
		StorageMapper& _storageMapper,
		std::string const& _sourceFile,
		LibraryFunctionIdMap const& _libraryFunctionIds,
		uint64_t _opupBudget = 0,
		FreeFunctionIdMap const& _freeFunctionById = {},
		std::map<std::string, uint64_t> const& _ensureBudget = {},
		bool _viaIR = false,
		std::vector<solidity::frontend::FunctionDefinition const*> const& _internalizableLibFuncs = {}
	);

	/// Build AWST from a full contract definition.
	std::shared_ptr<awst::Contract> build(
		solidity::frontend::ContractDefinition const& _contract
	);

	/// Take any function-pointer dispatch subroutines generated during build().
	/// These are root-level Subroutine nodes (not contract methods) so they're
	/// callable from library functions via SubroutineID.
	std::vector<std::shared_ptr<awst::Subroutine>> takeDispatchSubroutines()
	{
		return std::move(m_dispatchSubroutines);
	}

private:
	std::vector<std::shared_ptr<awst::Subroutine>> m_dispatchSubroutines;
	TypeMapper& m_typeMapper;
	StorageMapper& m_storageMapper;
	std::string m_sourceFile;
	LibraryFunctionIdMap const& m_libraryFunctionIds;
	uint64_t m_opupBudget = 0;
	FreeFunctionIdMap const& m_freeFunctionById;
	std::map<std::string, uint64_t> m_ensureBudget;
	bool m_viaIR = false;
	std::vector<solidity::frontend::FunctionDefinition const*> m_internalizableLibFuncs;

	std::unique_ptr<eb::ContractContext> m_exprBuilder;

	/// Translation-level context (per-contract): typeMapper, sourceFile, exprBuilder.
	/// Constructed in build() once m_exprBuilder exists; FunctionContext/BlockContext
	/// derive from this during function/block translation.
	std::optional<sol_ast::TranslationContext> m_tr;

	// ── Per-function scratch (set by setFunctionContext, used by buildBlock) ──
	bool m_currentInConstructor = false;
	bool m_currentFrameIsProgram = false;
	std::vector<std::pair<std::string, awst::WType const*>> m_currentParams;
	awst::WType const* m_currentReturnType = nullptr;
	std::map<std::string, unsigned> m_currentBitWidths;
	std::shared_ptr<awst::Block> m_currentPlaceholder;
	std::vector<solidity::frontend::VariableDeclaration const*> m_currentNamedReturns;
	std::vector<solidity::frontend::VariableDeclaration const*> m_currentMappingKeyParams;
	std::vector<solidity::frontend::VariableDeclaration const*> m_currentBlobAggParams;

	/// Build a function body block with function context set.
	std::shared_ptr<awst::Block> buildBlock(
		solidity::frontend::Block const& _block);

	/// Snapshot of the current per-function state as a `FunctionTranslationCtx`,
	/// so the contract-method path can share the free-function `buildBlock` /
	/// `inlineModifiers` implementations with the library/free-function path.
	FunctionTranslationCtx makeFunctionCtx();

	/// Set function context for inline assembly.
	void setFunctionContext(
		std::vector<std::pair<std::string, awst::WType const*>> const& _params,
		awst::WType const* _returnType,
		std::map<std::string, unsigned> const& _bitWidths = {});

	/// Set/clear placeholder body for modifier inlining.
	void setPlaceholderBody(std::shared_ptr<awst::Block> _body);

	/// Set the named-return parameter decls for the current function.
	/// `buildBlock` registers them in the function-body BlockContext so
	/// inner declarations with the same name get shadow-renamed.
	void setNamedReturns(
		std::vector<solidity::frontend::VariableDeclaration const*> const& _namedReturns
	)
	{
		m_currentNamedReturns = _namedReturns;
	}

	/// Set the mapping-storage-ref param decls for the current function.
	/// `buildBlock` registers them on the function-body FunctionContext so
	/// SolIndexAccess can build dynamic box-key prefixes at runtime.
	void setMappingKeyParams(
		std::vector<solidity::frontend::VariableDeclaration const*> const& _params
	)
	{
		m_currentMappingKeyParams = _params;
	}

	/// Set the blob-backed (>4KB) memory aggregate param decls for the current
	/// function. `buildBlock` registers them on the function-body FunctionContext
	/// so `p.field[i]` lowers to multi-slot blob word access (pointer model).
	void setBlobAggParams(
		std::vector<solidity::frontend::VariableDeclaration const*> const& _params
	)
	{
		m_currentBlobAggParams = _params;
	}

	/// Emit Solidity's non-payable check at method body entry:
	///   assert((GroupIndex > 0 ? gtxns(GroupIndex-1).Amount : 0) == 0,
	///          "not payable")
	/// No-op unless the method has an ARC4 config (i.e. is externally
	/// callable). Called for both public state-variable getters and
	/// public/external contract methods whose stateMutability is not Payable.
	void prependNonPayableCheck(awst::ContractMethod& _method);
	OverloadedNamesSet m_overloadedNames;

	/// Box-stored dynamic array variable names that need box_create in __postInit
	std::vector<std::string> m_boxArrayVarNames;

	/// Transient storage manager (blob-based, reset per transaction)
	TransientStorage m_transientStorage;
	/// Unified dispatch facade over AppGlobal / Box / Transient backends.
	/// Constructed once per ContractBuilder; tracks the current TransientStorage
	/// (or nullptr) so callers can route reads/writes by VariableDeclaration
	/// without re-implementing the backend dispatch.
	std::optional<StorageBackend> m_storageBackend;

	/// The contract currently being built (for modifier override resolution).
	solidity::frontend::ContractDefinition const* m_currentContract = nullptr;

	awst::SourceLocation makeLoc(solidity::langutil::SourceLocation const& _solLoc);

	/// Build the approval program for the contract.
	awst::ContractMethod buildApprovalProgram(
		solidity::frontend::ContractDefinition const& _contract,
		std::string const& _contractName
	);

	/// Emit `box_create` (or `box_put` for initialised vars) for every
	/// state variable listed in `m_boxArrayVarNames`. Appends to
	/// `_postInitBody`. Extracted from `buildApprovalProgram` for
	/// readability — the box-creation phase is ~230 lines of
	/// type-aware emission logic that doesn't share local state with the
	/// rest of the __postInit body assembly.
	void emitBoxCreateForStateVars(
		solidity::frontend::ContractDefinition const& _contract,
		awst::Block& _postInitBody,
		awst::SourceLocation const& _loc);

	/// Build the clear-state program.
	awst::ContractMethod buildClearProgram(
		solidity::frontend::ContractDefinition const& _contract,
		std::string const& _contractName
	);

	/// Build a contract method from a function definition.
	awst::ContractMethod buildFunction(
		solidity::frontend::FunctionDefinition const& _func,
		std::string const& _contractName,
		std::string const& _nameOverride = ""
	);

	/// Build an ARC4 method config for a public/external function.
	std::optional<awst::ARC4MethodConfig> buildARC4Config(
		solidity::frontend::FunctionDefinition const& _func,
		awst::SourceLocation const& _loc
	);

	/// Inline modifier bodies into function body (legacy, used for constructors).
	void inlineModifiers(
		solidity::frontend::FunctionDefinition const& _func,
		std::shared_ptr<awst::Block>& _body
	);

	/// Inline modifier bodies into function body (legacy, used for constructors).
	/// For regular functions, use buildModifierChain instead.
	/// Build modifier chain as separate subroutines (Solidity IRGenerator pattern).
	/// Adds subroutines to m_modifierSubroutines; caller must flush them.
	void buildModifierChain(
		solidity::frontend::FunctionDefinition const& _func,
		awst::ContractMethod& _method,
		std::string const& _contractName
	);

	/// Pending modifier subroutines generated by buildModifierChain.
	std::vector<awst::ContractMethod> m_modifierSubroutines;

	/// Fallback super targets (cross-function super calls, used by constructor body).
	std::unordered_map<int64_t, solidity::frontend::FunctionDefinition const*> m_fallbackSuperFuncs;
	/// All super target name registrations (AST ID → subroutine name).
	std::unordered_map<int64_t, std::string> m_allSuperTargetNames;
	/// Per-function MRO super overrides (caller func ID → list of (targetId, superName)).
	std::map<int64_t, std::vector<std::pair<int64_t, std::string>>> m_perFuncSuperOverrides;
	/// MRO super targets (caller func ID → target function whose body becomes the subroutine).
	std::unordered_map<int64_t, solidity::frontend::FunctionDefinition const*> m_superTargetFuncs;
	/// Explicit base class call targets (target func ID → target function).
	std::unordered_map<int64_t, solidity::frontend::FunctionDefinition const*> m_explicitBaseTargetFuncs;

	/// Generate __storage_read and __storage_write dispatch subroutines.
	/// Uses StorageLayout to build a switch table mapping slot numbers
	/// to app_global_get/put for known variables, with box fallthrough for dynamic slots.
	void buildStorageDispatch(
		solidity::frontend::ContractDefinition const& _contract,
		awst::Contract* _contractNode,
		std::string const& _contractName
	);

	/// Auto-generate getter methods for public state variables. Walks the
	/// linearized base contracts and emits one ARC4 getter per `public`
	/// variable not already covered by an explicit function. Mappings add key
	/// params, arrays add index params, structs return their non-mapping/
	/// non-dynamic-array fields. Skips variables whose names are in
	/// `_translatedFunctions` (explicit getters take precedence) and adds
	/// each emitted name to that set.
	void buildPublicStateVariableGetters(
		solidity::frontend::ContractDefinition const& _contract,
		awst::Contract& _contractNode,
		std::string const& _contractName,
		std::set<std::string>& _translatedFunctions);

	// ── Super-call resolution ──
	//
	// `super.f()` inside a function body resolves to the next implementation of
	// `f` in the linearized-base-contract MRO, not necessarily this contract's
	// own `f`. We resolve this by emitting per-caller `f__super_<callerId>`
	// subroutines that stand in for the right base implementation. There are
	// three flavours: MRO-driven (most super.f() calls), cross-function
	// fallback (super.g() called from f), and explicit Base.f() calls.

	/// Walk the contract's MRO and constructor, collect every super.f() and
	/// Base.f() target, and populate m_perFuncSuperOverrides / m_superTargetFuncs
	/// / m_fallbackSuperFuncs / m_explicitBaseTargetFuncs accordingly. Also
	/// pre-registers names into m_exprBuilder->superTargetNames.
	void collectSuperCallMetadata(solidity::frontend::ContractDefinition const& _contract);

	/// Before translating a function body, set m_exprBuilder->superTargetNames
	/// to the per-caller MRO overrides for `_callerFuncId` plus the always-on
	/// fallback / explicit-base entries.
	void applySuperOverridesFor(int64_t _callerFuncId);

	/// Wipe m_exprBuilder->superTargetNames (called between function bodies to
	/// avoid super-context cross-contamination).
	void clearSuperOverrides();

	/// Emit the actual `f__super_<callerId>` subroutines once all bodies have
	/// been translated — one per MRO entry, fallback, or explicit base target.
	void emitSuperSubroutines(awst::Contract& _contractNode, std::string const& _contractName);

	/// If buildApprovalProgram detects box writes in the constructor,
	/// it populates this with an auto-generated __postInit method.
	std::optional<awst::ContractMethod> m_postInitMethod;
};

} // namespace puyasol::builder
