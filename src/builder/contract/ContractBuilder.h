#pragma once

#include "awst/Node.h"
#include "builder/sol-eb/ContractContext.h"
#include "builder/sol-ast/Context.h"
#include "builder/sol-ast/SolStatement.h"
#include "builder/storage/StorageLayout.h"
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
	/// Declared solc param types by BARE name (possible_solc item 2): the
	/// synthetic-calldata blob derives the EVM-ABI head layout + value
	/// widening (sign extension, static-aggregate inlining) from these.
	/// Appended at struct end (aggregate init) — assigned explicitly.
	std::map<std::string, solidity::frontend::Type const*> paramSolTypes;
	// Build-time ABI return encoding (D2) — mirror FunctionContext's fields.
	// Appended (assigned explicitly, not positional) like inConstructor/frameIsProgram.
	bool encodeReturnsAtBuildTime = false;
	bool returnAsmWrap = false;
	std::vector<ReturnWireElem> returnWirePlan;
	// Live-calldata-pointer set (see FunctionContext::seededCalldataPointers).
	std::set<std::string>* seededCalldataPointers = nullptr;
	/// --evm-storage-layout: storage-ref params + named storage returns,
	/// carried as biguint slot handles; `buildBlock` registers each as a
	/// slot-storage ref. Appended at struct end (aggregate init) — assigned
	/// explicitly, never positional.
	std::vector<solidity::frontend::VariableDeclaration const*> slotRefParams;
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

/// --evm-memory-layout: spill MEMORY PARAMS that inline assembly treats as
/// pointers (`keccak256(marketParams, 128)` on a struct param) into blob
/// regions at function entry, registering each as a blob aggregate on `_fn`.
/// Statements are appended to `_out` (caller prepends them to the body).
/// Shared by the contract-method path and the library/free-function path.
void emitAsmParamSpills(
	TypeMapper& _typeMapper,
	sol_ast::FunctionContext& _fn,
	solidity::frontend::Block const& _block,
	std::string const& _sourceFile,
	std::vector<std::shared_ptr<awst::Statement>>& _out);

/// Inverse of `emitBlobBackValue` for STRUCTS: read one EVM word per value
/// field at `_offVar` and rebuild the ARC4Struct value (blob-backed struct
/// used as a VALUE — `idToMarketParams[id] = marketParams`). Null + loud
/// error for non-value members.
std::shared_ptr<awst::Expression> materializeBlobStructValue(
	TypeMapper& _typeMapper,
	solidity::frontend::StructType const* _structType,
	awst::WType const* _wtype,
	std::string const& _offVar,
	awst::SourceLocation const& _loc);

/// True iff any inline-assembly block in `_block` references declaration
/// `_declId` (used to decide blob-backed named-return materialisation).
bool blockUsesDeclInAsm(
	solidity::frontend::Block const& _block, int64_t _declId);

/// --evm-memory-layout: emit statements that ALLOCATE a blob region in EVM
/// layout for `_value` (bytes/string → [len32][data]; struct → one word per
/// value field; arrays of 32-byte-encoded elements → [count32]?[elems]) and
/// bind `_offVar` (uint64) to its base offset. Returns false (+ loud error)
/// for unsupported shapes.
bool emitBlobBackValue(
	TypeMapper& typeMapper,
	solidity::frontend::Type const* declType,
	awst::WType const* wtype,
	std::shared_ptr<awst::Expression> value,
	std::string const& offVar,
	int uniqueId,
	awst::SourceLocation const& loc,
	std::vector<std::shared_ptr<awst::Statement>>& out);

/// Promote memory aggregates used as VALUES in inline assembly (their Yul memory
/// pointer) to blob-backed (pointer model) on `fn`. Must run before body
/// translation so SolVariableDeclaration blob-backs them at declaration. Shared
/// by the contract-method path (`buildBlock`) and the free/library-function path
/// (AWSTBuilder) so internal/library asm buffers (OZ Strings.toString) are marked
/// in both.
/// True iff `_func` is called INTERNALLY (plain `f(...)`) anywhere in
/// `_contract`. Gates the selector-checked non-payable guard, which is only
/// needed for methods reachable by callsub.
bool _isCalledInternally(
	solidity::frontend::ContractDefinition const& _contract,
	solidity::frontend::FunctionDefinition const& _func);

void markAssemblyAggregates(
	sol_ast::FunctionContext& fn,
	solidity::frontend::Block const& block);

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

/// Builds an AWST Contract from a Solidity ContractDefinition (approval/clear programs,
/// ARC4 methods, modifier inlining, super-call resolution, __postInit generation).
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

	/// Take root-level dispatch subroutines generated by build() (callable from library fns via SubroutineID).
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

	/// Per-contract translation context; constructed in build() once m_exprBuilder exists.
	std::optional<sol_ast::TranslationContext> m_tr;

	// ── Per-function scratch (set by setFunctionContext, used by buildBlock) ──
	bool m_currentInConstructor = false;
	bool m_currentFrameIsProgram = false;
	std::vector<std::pair<std::string, awst::WType const*>> m_currentParams;
	std::map<std::string, solidity::frontend::Type const*> m_currentParamSolTypes;
	awst::WType const* m_currentReturnType = nullptr;
	std::map<std::string, unsigned> m_currentBitWidths;
	std::shared_ptr<awst::Block> m_currentPlaceholder;
	std::vector<solidity::frontend::VariableDeclaration const*> m_currentNamedReturns;
	std::vector<solidity::frontend::VariableDeclaration const*> m_currentMappingKeyParams;
	std::vector<solidity::frontend::VariableDeclaration const*> m_currentBlobAggParams;
	std::vector<solidity::frontend::VariableDeclaration const*> m_currentSlotRefParams;
	// Build-time ABI return encoding (D2): plan + flags flow into FunctionContext so
	// SolReturnStatement encodes each return value as it builds it.
	bool m_currentEncodeReturnsAtBuildTime = false;
	bool m_currentReturnAsmWrap = false;
	std::vector<ReturnWireElem> m_currentReturnWirePlan;
	// Live-calldata-pointer set: OUTLIVES buildBlock (the implicit-return synth in
	// FunctionBuilder consults it after body translation) — hence scratch here, with
	// FunctionContext holding a pointer.
	std::set<std::string> m_currentSeededCalldataPointers;

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
		std::map<std::string, unsigned> const& _bitWidths = {},
		std::map<std::string, solidity::frontend::Type const*> const& _paramSolTypes = {});

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

	/// Enable build-time ABI return encoding for the current function (D2).
	/// `buildBlock` forwards the plan to the FunctionContext; SolReturnStatement
	/// then ARC4-encodes each return value in place instead of the ReturnRewriter
	/// post-pass. Cleared per function by resetFunctionContext.
	void setReturnWirePlan(std::vector<ReturnWireElem> _plan, bool _asmWrap)
	{
		m_currentReturnWirePlan = std::move(_plan);
		m_currentReturnAsmWrap = _asmWrap;
		m_currentEncodeReturnsAtBuildTime = true;
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

	/// --evm-storage-layout: storage-ref params/named-returns of the current
	/// function (biguint slot handles); registered in `buildBlock`.
	void setSlotRefParams(
		std::vector<solidity::frontend::VariableDeclaration const*> const& _params
	)
	{
		m_currentSlotRefParams = _params;
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

	/// Prepend assert(incoming_amount==0,"not payable") to externally-callable non-payable methods.
	void prependNonPayableCheck(awst::ContractMethod& _method,
		std::string const& _arc4Selector = {});
	OverloadedNamesSet m_overloadedNames;

	/// Box-stored dynamic array variable names that need box_create in __postInit
	std::vector<std::string> m_boxArrayVarNames;

	/// Transient storage manager (blob-based, reset per transaction)
	TransientStorage m_transientStorage;
	/// Unified AppGlobal / Box / Transient dispatch facade.
	std::optional<StorageBackend> m_storageBackend;

	/// --evm-storage-layout: per-contract solc-exact layout handed to the
	/// expression builders via ContractContext::evmSlotLayout.
	std::unique_ptr<StorageLayout> m_evmLayout;

	/// The contract currently being built (for modifier override resolution).
	solidity::frontend::ContractDefinition const* m_currentContract = nullptr;

	awst::SourceLocation makeLoc(solidity::langutil::SourceLocation const& _solLoc);

	/// Build the approval program for the contract.
	awst::ContractMethod buildApprovalProgram(
		solidity::frontend::ContractDefinition const& _contract,
		std::string const& _contractName
	);

	/// Emit box_create/box_put for each var in m_boxArrayVarNames. Appends to _postInitBody.
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
	/// _asInternalCopy: build as a plain INTERNAL subroutine even for a
	/// public/external _func — no ARC4 config, and therefore no ABI entry
	/// checks, no not-payable group assert, no ARC4 param remap/decodes, no
	/// wire-return encoding, no opup budget. Used for super/Base.f() impl
	/// copies: they are direct callsub targets, and baking the callee's
	/// entry semantics in made a payable caller inherit the base's
	/// not-payable assert (false revert when grouped with a payment).
	awst::ContractMethod buildFunction(
		solidity::frontend::FunctionDefinition const& _func,
		std::string const& _contractName,
		std::string const& _nameOverride = "",
		bool _asInternalCopy = false
	);

	/// Build an ARC4 method config for a public/external function.
	std::optional<awst::ARC4MethodConfig> buildARC4Config(
		solidity::frontend::FunctionDefinition const& _func,
		awst::SourceLocation const& _loc
	);

	/// Inline modifier bodies (for constructors). For regular functions use buildModifierChain.
	void inlineModifiers(
		solidity::frontend::FunctionDefinition const& _func,
		std::shared_ptr<awst::Block>& _body
	);

	/// Build modifier chain as separate subroutines; adds to m_modifierSubroutines.
	void buildModifierChain(
		solidity::frontend::FunctionDefinition const& _func,
		awst::ContractMethod& _method,
		std::string const& _contractName,
		std::vector<std::shared_ptr<awst::Statement>> const& _paramDecodes = {}
	);

	/// Pending modifier subroutines generated by buildModifierChain.
	std::vector<awst::ContractMethod> m_modifierSubroutines;

	/// Ctor-scope super target names snapshotted after collection (AST ID →
	/// subroutine name); buildApprovalProgram re-applies them for ctor bodies.
	std::unordered_map<int64_t, std::string> m_allSuperTargetNames;
	/// Per-caller super/base-call overrides (caller func ID → (call-site
	/// refDecl ID, resolved subroutine name)).
	std::map<int64_t, std::vector<std::pair<int64_t, std::string>>> m_perFuncSuperOverrides;
	/// Resolved base implementations to emit, deduped by target (target func
	/// ID → definition); each becomes one f<suffix>__impl_<targetId> subroutine.
	std::map<int64_t, solidity::frontend::FunctionDefinition const*> m_superImplsToEmit;

	/// Emit __storage_read/__storage_write: switch on slot → app_global, box fallthrough for dynamic.
	void buildStorageDispatch(
		solidity::frontend::ContractDefinition const& _contract,
		awst::Contract* _contractNode,
		std::string const& _contractName
	);

	/// --evm-storage-layout: __storage_read/__storage_write over a flat slot
	/// space — dense slots (< 2^16) in 2048-byte page boxes ("p:" ++ itob(page)),
	/// hashed slots in one box per slot ("s:" ++ slot32). No named-cell routing.
	void buildEvmSlotStorageDispatch(
		solidity::frontend::ContractDefinition const& _contract,
		awst::Contract* _contractNode,
		std::string const& _contractName
	);

	/// Emit ARC4 getters for public state vars (mappings+key params, arrays+idx params,
	/// structs sans mapping/dyn-array fields). Explicit functions take precedence.
	void buildPublicStateVariableGetters(
		solidity::frontend::ContractDefinition const& _contract,
		awst::Contract& _contractNode,
		std::string const& _contractName,
		std::set<std::string>& _translatedFunctions);

	// ── Super-call resolution (SuperCallResolution.cpp) ──
	// super.f() / Base.f() resolved via solc's requiredLookup annotation +
	// FunctionDefinition::resolveVirtual (fable-review item 11). One emitted
	// subroutine per resolved TARGET: f<suffix>__impl_<targetId>.

	/// Subroutine name for a resolved base implementation.
	std::string superImplName(solidity::frontend::FunctionDefinition const& _target) const;

	/// Resolve every super.f() / Base.f() call site across the linearized
	/// hierarchy (methods + ctors); populates m_perFuncSuperOverrides /
	/// m_superImplsToEmit. Ctor-body sites are also registered eagerly into
	/// superTargetNames for the buildApprovalProgram snapshot.
	void collectSuperCallMetadata(solidity::frontend::ContractDefinition const& _contract);

	/// Set superTargetNames to the resolved targets of _callerFuncId's body.
	void applySuperOverridesFor(int64_t _callerFuncId);

	/// Clear superTargetNames between function bodies.
	void clearSuperOverrides();

	/// Emit the deduped f<suffix>__impl_<targetId> subroutines after all
	/// regular bodies are translated.
	void emitSuperSubroutines(awst::Contract& _contractNode, std::string const& _contractName);

	/// If buildApprovalProgram detects box writes in the constructor,
	/// it populates this with an auto-generated __postInit method.
	std::optional<awst::ContractMethod> m_postInitMethod;
};

/// Slot-mode unit pre-scan (see evmDenseOnlyUnit in EvmLayoutMode.h): true iff
/// this contract can reach keccak-derived (sparse) slots — inline assembly
/// anywhere in its defined functions, or a persistent state var whose type
/// derives hashed slots (mapping / dynamic array / bytes / string, recursively
/// through structs and fixed arrays). `_denseSlotsOut` receives the contract's
/// dense layout slot count (for the unit-global single-page decision).
bool evmContractNeedsSparseSlots(
	solidity::frontend::ContractDefinition const& _contract,
	TypeMapper& _typeMapper,
	unsigned long long& _denseSlotsOut);

} // namespace puyasol::builder
