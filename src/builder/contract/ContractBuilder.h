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
/// If `placeholder` is set, each `_` (PlaceholderStatement) invokes it to
/// construct a fresh replacement block. Used by modifier-chain translation.
std::shared_ptr<awst::Block> buildBlock(
	sol_ast::FunctionContext& ctx,
	solidity::frontend::Block const& block,
	sol_ast::PlaceholderFactory placeholder = {});

/// EVM blob memory: spill MEMORY PARAMS that inline assembly treats as
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

/// EVM blob memory: recursively allocate and spill `_value` using solc's
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

/// Builds an AWST Contract from a Solidity ContractDefinition (approval/clear programs,
/// ARC4 methods, modifier lowering, super-call resolution, __postInit generation).
/// Recipe for rebuilding one ABI-to-native parameter assignment. Modifier
/// chain members each materialize their own statements from this immutable
/// description; no AWST copying is involved.
struct ParamDecode
{
	size_t argIndex;
	std::string name;
	awst::WType const* nativeType;
	awst::WType const* arc4Type;
	awst::SourceLocation loc;
	unsigned signedBits = 0;
};

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
	/// Artifact names for colliding contract names (see ContractBuilder::build).
	void setArtifactNames(std::map<std::string, std::string> _names)
	{
		m_artifactNames = std::move(_names);
	}

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
	/// solc source-unit-qualified declaration identity, independent of display paths.
	std::string m_contractId;
	FunctionSymbolTable const& m_functionSymbols;
	uint64_t m_opupBudget = 0;
	std::map<std::string, uint64_t> m_ensureBudget;
	bool m_viaIR = false;
	std::vector<solidity::frontend::FunctionDefinition const*> m_hostBoundFunctions;
	/// Fully qualified name → artifact (AWST contract) name for contracts whose
	/// plain name collides within the compilation (solc's filesystem-friendly rule).
	std::map<std::string, std::string> m_artifactNames;

	std::unique_ptr<eb::ContractContext> m_exprBuilder;

	/// Per-contract translation context; constructed in build() once m_exprBuilder exists.
	std::optional<sol_ast::TranslationContext> m_tr;

	/// The sole owner of per-function translation and lexical function state.
	std::optional<sol_ast::FunctionContext> m_functionCtx;

	/// Build a function body block with function context set.
	std::shared_ptr<awst::Block> buildBlock(
		solidity::frontend::Block const& _block,
		sol_ast::PlaceholderFactory _placeholder = {});

	/// Start a fresh function frame, retaining the contract's declaration bindings.
	void setFunctionContext(
		std::vector<std::pair<std::string, awst::WType const*>> const& _params,
		awst::WType const* _returnType,
		std::map<std::string, unsigned> const& _bitWidths = {},
		std::map<std::string, solidity::frontend::Type const*> const& _paramSolTypes = {});


	/// Enable build-time ABI return encoding for the current function (D2).
	/// `buildBlock` forwards the plan to the FunctionContext; SolReturnStatement
	/// then ARC4-encodes each return value in place. Cleared for each function by
	/// setFunctionContext.
	void setReturnWirePlan(std::vector<ReturnWireElem> _plan, bool _asmWrap)
	{
		m_functionCtx->returnWirePlan = std::move(_plan);
		m_functionCtx->returnAsmWrap = _asmWrap;
		m_functionCtx->encodeReturnsAtBuildTime = true;
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

	// ── buildFunction phases (FunctionBuilder.cpp) ──────────────────────
	void buildMethodSignature(
		awst::ContractMethod& method,
		solidity::frontend::FunctionDefinition const& _func,
		std::string const& _nameOverride);
	void prependNamedReturnInits(
		awst::ContractMethod& method,
		solidity::frontend::FunctionDefinition const& _func);
	void synthesizeImplicitReturn(
		awst::ContractMethod& method,
		solidity::frontend::FunctionDefinition const& _func,
		bool encodeReturnsAtBuildTime,
		std::vector<ReturnWireElem> const& returnPlan,
		bool funcHasInlineAssembly);
	void prependAbiEntryChecks(
		awst::ContractMethod& method,
		solidity::frontend::FunctionDefinition const& _func);
	void prependEnsureBudget(
		awst::ContractMethod& method,
		solidity::frontend::FunctionDefinition const& _func);
	void maybePrependNonPayable(
		awst::ContractMethod& method,
		solidity::frontend::FunctionDefinition const& _func);

	/// Build an ARC4 method config for a public/external function.
	std::optional<awst::ARC4MethodConfig> buildARC4Config(
		solidity::frontend::FunctionDefinition const& _func,
		awst::SourceLocation const& _loc
	);

	/// Lower a constructor's modifiers through the same subroutine chain used
	/// by regular functions, replacing `body` with a call to the outer chain.
	void buildConstructorModifierChain(
		solidity::frontend::FunctionDefinition const& _func,
		std::shared_ptr<awst::Block>& _body,
		std::string const& _contractName
	);

	/// Build modifier chain as separate subroutines; adds to m_modifierSubroutines.
	void buildModifierChain(
		solidity::frontend::FunctionDefinition const& _func,
		awst::ContractMethod& _method,
		std::string const& _contractName,
		std::vector<ParamDecode> const& _paramDecodes = {},
		std::vector<size_t> const& _writeBackParams = {}
	);
	std::vector<std::shared_ptr<awst::Statement>> makeParamDecodeStatements(
		std::vector<ParamDecode> const& _paramDecodes);
	/// Function memory parameters whose objects are passed to memory modifier
	/// parameters. solc models these as pointers; the chain gives each root one
	/// shared scratch allocation and passes its offset through every link.
	std::vector<solidity::frontend::VariableDeclaration const*>
	modifierMemoryRootParams(
		solidity::frontend::FunctionDefinition const& _func) const;
	void registerModifierMemoryRootParams(
		solidity::frontend::FunctionDefinition const& _func);
	/// Bind one modifier invocation's arguments for the chain link being built:
	/// binding statements go to `_modBody`; value-remapped and pointer-backed
	/// modifier declarations are returned separately for deterministic cleanup.
	void bindModifierArguments(
		solidity::frontend::ModifierInvocation const& _invocation,
		solidity::frontend::ModifierDefinition const& _modifier,
		awst::Block& _modBody,
		std::vector<int64_t>& _remappedDeclIds,
		std::vector<int64_t>& _blobDeclIds);

	/// Pending modifier subroutines generated by buildModifierChain.
	std::vector<awst::ContractMethod> m_modifierSubroutines;

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

	// ── build phases (ContractBuilder.cpp), invoked in this order ───────
	/// Reset per-contract state and name counters; returns the artifact
	/// (AWST) contract name.
	std::string beginContract(
		solidity::frontend::ContractDefinition const& _contract,
		StorageRuntimePlan const& _storagePlan);
	/// Fill m_overloadedNames; returns the ids of every function that a
	/// more-derived definition overrides.
	std::set<int64_t> collectOverloadedNames(
		solidity::frontend::ContractDefinition const& _contract);
	void createExpressionBuilder(
		solidity::frontend::ContractDefinition const& _contract,
		StorageRuntimePlan const& _storagePlan,
		std::string const& _contractName);
	/// Host-bound free/library functions this contract's call graph reaches.
	std::vector<solidity::frontend::FunctionDefinition const*>
	collectReachableHostBoundFunctions(
		solidity::frontend::ContractDefinition const& _contract) const;
	void registerHostBoundFunctionNames(
		std::vector<solidity::frontend::FunctionDefinition const*> const& _functions);
	void createFunctionContexts();
	std::shared_ptr<awst::Contract> makeContractNode(
		solidity::frontend::ContractDefinition const& _contract,
		std::string const& _contractName);
	/// Approval/clear programs, the deferred-constructor __postInit method and
	/// the constructor's modifier subroutines.
	void buildPrograms(
		solidity::frontend::ContractDefinition const& _contract,
		std::string const& _contractName,
		awst::Contract& _contractNode);
	void buildDefinedFunctions(
		solidity::frontend::ContractDefinition const& _contract,
		std::string const& _contractName,
		awst::Contract& _contractNode,
		std::set<std::string>& _translatedFunctions);
	void buildInheritedFunctions(
		solidity::frontend::ContractDefinition const& _contract,
		std::string const& _contractName,
		awst::Contract& _contractNode,
		std::set<int64_t> const& _overriddenIds,
		std::set<std::string>& _translatedFunctions);
	/// Child-program provisioning, EVM entry / compat routes, ARC-4 selector
	/// dispatch and the router-memoized struct decoders.
	void buildRouters(
		solidity::frontend::ContractDefinition const& _contract,
		awst::Contract& _contractNode);
	void buildHostBoundFunctions(
		std::string const& _contractName,
		awst::Contract& _contractNode,
		std::vector<solidity::frontend::FunctionDefinition const*> const& _functions);
	/// Function-pointer dispatch tables plus the recursive-Yul subroutine drain.
	void emitFunctionPointerDispatch(awst::Contract& _contractNode);
	void scopeStorageDispatchCalls(
		StorageRuntimePlan const& _storagePlan,
		awst::Contract& _contractNode);
	void warnEscapedErc1967Slots(awst::Contract const& _contractNode);
	void emitErc1967AdminGate(
		solidity::frontend::ContractDefinition const& _contract,
		awst::Contract& _contractNode);
	void emitUupsUpdateGate(
		solidity::frontend::ContractDefinition const& _contract,
		awst::Contract& _contractNode);
	/// Dedup key of a translated function: name, or name#id for true overloads.
	std::string translationKey(
		solidity::frontend::FunctionDefinition const& _func) const;
	/// Append `_method` followed by the modifier subroutines its build produced.
	void appendMethodWithModifierSubs(
		awst::Contract& _contractNode, awst::ContractMethod _method);

	/// Emit each demanded concrete base implementation once, closing over
	/// transitive requests discovered while translating its body.
	void emitSuperSubroutines(awst::Contract& _contractNode, std::string const& _contractName);

	/// If buildApprovalProgram detects box writes in the constructor,
	/// it populates this with an auto-generated __postInit method.
	/// Synthesise the deferred-constructor `__postInit` method (PostInitBuilder.cpp).
	// ── buildApprovalProgram phases (ApprovalProgramBuilder.cpp) ────────
	std::shared_ptr<awst::Expression> lowerStateInitializer(
		solidity::frontend::VariableDeclaration const& _var,
		awst::WType const* _target,
		awst::SourceLocation const& _loc);
	void emitSlotModeStateVarInit(
		solidity::frontend::VariableDeclaration const& _var,
		std::vector<std::shared_ptr<awst::Statement>>& targetBody,
		awst::SourceLocation const& loc);
	void emitStateVarInitFor(
		solidity::frontend::ContractDefinition const& base,
		std::vector<std::shared_ptr<awst::Statement>>& targetBody,
		std::set<int64_t>& stateVarInitialized,
		awst::SourceLocation const& loc);
	void collectBoxArrayVars(
		solidity::frontend::ContractDefinition const& _contract,
		awst::SourceLocation const& loc);
	void emitCtorParamDecode(
		solidity::frontend::FunctionDefinition const& _constructor,
		std::shared_ptr<awst::Block> const& createBlock,
		bool needsPostInit,
		awst::SourceLocation const& loc);
	void bindBaseCtorArgs(
		solidity::frontend::FunctionDefinition const& baseCtor,
		std::vector<solidity::frontend::ASTPointer<solidity::frontend::Expression>> const& args,
		std::shared_ptr<awst::Block> const& createBlock);
	void emitConstructorPlan(
		solidity::frontend::ContractDefinition const& _contract,
		std::shared_ptr<awst::Block> const& createBlock,
		std::function<void(solidity::frontend::ContractDefinition const&,
			std::vector<std::shared_ptr<awst::Statement>>&)> const& emitStateVarInit);
	void emitTransientBlobInit(
		awst::Block& body, awst::SourceLocation const& loc);
	void emitMemoryBlobInit(
		awst::Block& body, awst::SourceLocation const& loc);

	void buildPostInitMethod(
		solidity::frontend::ContractDefinition const& _contract,
		std::string const& _contractName,
		awst::ContractMethod& method,
		std::shared_ptr<awst::Block> const& createBlock,
		std::function<void(solidity::frontend::ContractDefinition const&,
			std::vector<std::shared_ptr<awst::Statement>>&)> const& emitStateVarInit);

	std::optional<awst::ContractMethod> m_postInitMethod;
};

} // namespace puyasol::builder
