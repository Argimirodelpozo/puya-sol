#pragma once

#include <functional>

#include "awst/Node.h"
#include "builder/sol-eb/ContractContext.h"
#include "builder/FunctionSymbolTable.h"
#include "builder/sol-ast/Context.h"
#include "builder/sol-ast/SolStatement.h"
#include "builder/storage/StorageLayout.h"
#include "builder/storage/StorageMapper.h"
#include "builder/storage/StorageRuntimePlan.h"
#include "builder/storage/StorageBackend.h"
#include "builder/storage/TransientStorage.h"
#include "builder/sol-types/TypeMapper.h"

#include <libsolidity/ast/ASTForward.h>
#include "builder/sol-types/SolcFwd.h"

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

/// Set of function names that have overloads (multiple definitions with same name).
using OverloadedNamesSet = std::unordered_set<std::string>;

/// Make an `awst::SourceLocation` from a Solidity `SourceLocation`.
awst::SourceLocation makeLoc(
	TypeMapper const& typeMapper,
	std::string const& sourceFile,
	solidity::langutil::SourceLocation const& solLoc);

/// Translate a Solidity Block in the given function context.
///
/// If `placeholder` is non-null, any `_` (PlaceholderStatement) inside the
/// block gets replaced by it. Used during modifier body translation, where
/// `_` stands for the wrapped function body.
std::shared_ptr<awst::Block> buildBlock(
	sol_ast::FunctionContext& ctx,
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

/// Inverse of `emitBlobBackValue`: recursively materialise any supported EVM
/// memory value.  Reference children follow their pointer words and arrays use
/// solc's memoryStride(); generated loops are appended to `_out`.
std::shared_ptr<awst::Expression> materializeBlobValue(
	TypeMapper& _typeMapper,
	solidity::frontend::Type const* _solType,
	awst::WType const* _wtype,
	std::string const& _offVar,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out);

/// --evm-memory-layout: recursively allocate and spill `_value` using solc's
/// memory head/data sizes.  Reference children receive real pointer words;
/// scalar width conversion is delegated to the shared EVM leaf codec.
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
	sol_ast::FunctionContext& ctx,
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
		eb::FunctionPointerRegistry& _functionPointers,
		std::string const& _sourceFile,
		FunctionSymbolTable const& _functionSymbols,
		uint64_t _opupBudget = 0,
		std::map<std::string, uint64_t> const& _ensureBudget = {},
		bool _viaIR = false,
		std::vector<solidity::frontend::FunctionDefinition const*> const& _hostBoundFunctions = {}
	);

	/// Build AWST from a full contract definition.
	std::shared_ptr<awst::Contract> build(
		solidity::frontend::ContractDefinition const& _contract,
		StorageRuntimePlan const& _storagePlan,
		bool _emitEvmStorageRuntime
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
	eb::FunctionPointerRegistry& m_functionPointers;
	std::string m_sourceFile;
	FunctionSymbolTable const& m_functionSymbols;
	uint64_t m_opupBudget = 0;
	std::map<std::string, uint64_t> m_ensureBudget;
	bool m_viaIR = false;
	std::vector<solidity::frontend::FunctionDefinition const*> m_hostBoundFunctions;

	std::unique_ptr<eb::ContractContext> m_exprBuilder;

	/// Per-contract translation context; constructed in build() once m_exprBuilder exists.
	std::optional<sol_ast::TranslationContext> m_tr;

	/// The sole owner of per-function translation and lexical function state.
	std::optional<sol_ast::FunctionContext> m_functionCtx;

	/// Build a function body block with function context set.
	std::shared_ptr<awst::Block> buildBlock(
		solidity::frontend::Block const& _block);

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
		m_functionCtx->namedReturns = _namedReturns;
	}

	/// Enable build-time ABI return encoding for the current function (D2).
	/// `buildBlock` forwards the plan to the FunctionContext; SolReturnStatement
	/// then ARC4-encodes each return value in place instead of the ReturnRewriter
	/// post-pass. Cleared for each function by setFunctionContext.
	void setReturnWirePlan(std::vector<ReturnWireElem> _plan, bool _asmWrap)
	{
		m_functionCtx->returnWirePlan = std::move(_plan);
		m_functionCtx->returnAsmWrap = _asmWrap;
		m_functionCtx->encodeReturnsAtBuildTime = true;
	}

	/// Set the mapping-storage-ref param decls for the current function.
	/// `buildBlock` registers them on the function-body FunctionContext so
	/// SolIndexAccess can build dynamic box-key prefixes at runtime.
	void setMappingKeyParams(
		std::vector<solidity::frontend::VariableDeclaration const*> const& _params
	)
	{
		m_functionCtx->mappingKeyParams = _params;
	}

	/// --evm-storage-layout: storage-ref params/named-returns of the current
	/// function (biguint slot handles); registered in `buildBlock`.
	void setSlotRefParams(
		std::vector<solidity::frontend::VariableDeclaration const*> const& _params
	)
	{
		m_functionCtx->slotRefParams = _params;
	}

	/// Set the blob-backed (>4KB) memory aggregate param decls for the current
	/// function. `buildBlock` registers them on the function-body FunctionContext
	/// so `p.field[i]` lowers to multi-slot blob word access (pointer model).
	void setBlobAggParams(
		std::vector<solidity::frontend::VariableDeclaration const*> const& _params
	)
	{
		m_functionCtx->blobAggParams = _params;
	}

	/// Prepend assert(incoming_amount==0,"not payable") to externally-callable non-payable methods.
	void prependNonPayableCheck(awst::ContractMethod& _method,
		std::string const& _arc4Selector = {});
	OverloadedNamesSet m_overloadedNames;

	/// Box-stored declarations that need box_create in __postInit.
	std::vector<solidity::frontend::VariableDeclaration const*> m_boxArrayVars;

	/// Transient storage manager (blob-based, reset per transaction)
	TransientStorage m_transientStorage;
	/// Unified AppGlobal / Box / Transient dispatch facade.
	std::optional<StorageBackend> m_storageBackend;

	/// The contract currently being built (for modifier override resolution).
	solidity::frontend::ContractDefinition const* m_currentContract = nullptr;

	awst::SourceLocation makeLoc(solidity::langutil::SourceLocation const& _solLoc);

	/// Build the approval program for the contract.
	awst::ContractMethod buildApprovalProgram(
		solidity::frontend::ContractDefinition const& _contract,
		std::string const& _contractName
	);

	/// Emit box_create/box_put for each var in m_boxArrayVars. Appends to _postInitBody.
	void emitBoxCreateForStateVars(
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
		StorageRuntimePlan const& _storagePlan,
		awst::Contract* _contractNode,
		std::string const& _contractName
	);

	/// --evm-storage-layout: __storage_read/__storage_write over a flat slot
	/// space — dense slots (< 2^16) in 2048-byte page boxes ("p:" ++ itob(page)),
	/// hashed slots in one box per slot ("s:" ++ slot32). No named-cell routing.
	void buildEvmSlotStorageDispatch(
		StorageRuntimePlan const& _storagePlan,
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

	/// Replace public ARC4 exposure with a deterministic Solidity-selector
	/// adapter. ApplicationArgs[1] is decoded as one canonical EVM ABI tuple;
	/// method bodies retain their existing native/ARC4-backed representation.
	void emitEvmEntryDispatch(
		solidity::frontend::ContractDefinition const& _contract,
		awst::Contract& _contractNode);

	/// ARC-4 profile: mount the EVM route arms as a compatibility alias ahead
	/// of the native ARC-4 router, so canonical EVM calldata (abi.encode* over
	/// a low-level .call) dispatches. Does not touch arc4MethodConfigs.
	void emitEvmCompatRoutes(
		solidity::frontend::ContractDefinition const& _contract,
		awst::Contract& _awstContract);

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
	/// Synthesise the deferred-constructor `__postInit` method (PostInitBuilder.cpp).
	void buildPostInitMethod(
		solidity::frontend::ContractDefinition const& _contract,
		std::string const& _contractName,
		awst::ContractMethod& method,
		std::shared_ptr<awst::Block> const& createBlock,
		std::map<solidity::frontend::ContractDefinition const*,
			std::vector<solidity::frontend::ASTPointer<solidity::frontend::Expression>> const*>
			const& explicitBaseArgs,
		std::function<void(solidity::frontend::ContractDefinition const&,
			std::vector<std::shared_ptr<awst::Statement>>&)> const& emitStateVarInit);

	std::optional<awst::ContractMethod> m_postInitMethod;
};

} // namespace puyasol::builder
