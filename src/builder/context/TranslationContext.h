#pragma once

/// @file Context.h
/// Solidity AST traversal state. Each scope is a flat view of its emitted
/// function's declaration bindings; block nesting carries the
/// effective unchecked flag, loop target, and modifier placeholder explicitly.

#include "awst/Node.h"
#include "builder/solc/SourceLocConvert.h"
#include "builder/types/TypeMapper.h"
#include "builder/types/ReturnWirePlan.h"

#include "builder/solc/SolcFwd.h"

#include <map>
#include <set>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <unordered_set>

namespace solidity::frontend
{
class Statement;
}

namespace puyasol::builder
{
namespace eb { class ContractContext; }
}

namespace puyasol::builder::sol_ast
{

/// Builds one fresh modifier-placeholder replacement block. A modifier may
/// contain several `_;` statements, so each expansion owns independent AWST nodes.
using PlaceholderFactory = std::function<std::shared_ptr<awst::Block>()>;

/// Modifier-lowering param remap entry: when a modifier is applied
/// multiple times in a single function, each instance's locals get a
/// unique mangled name with their original AWST type.
struct ParamRemap
{
	std::string name;
	awst::WType const* type;
};

/// Typed local storage-pointer alias (`T storage p = …`).
///
/// Shapes:
///   `mapping(K=>V) storage m = stateMap;` // MappingHolder (BytesConstant)
///   `T[] storage p = stateArr;`           // StateRead (StateGet)
///   `T storage e = container[i];`         // IndexedPath (IndexExpression)
///   `T storage f = s.field;`              // FieldPath (FieldExpression)
///   `(_, T storage e, _) = (...);`        // TupleSlice (TupleItemExpression)
///
/// The Kind tag makes the producer's intent explicit and gives consumers
/// a switch instead of a dynamic_cast ladder. expr must match the tag;
/// use the named factory methods to uphold that invariant.
struct StorageAlias
{
	enum class Kind
	{
		MappingHolder,   ///< BytesConstant — runtime mapping holder name
		StateRead,       ///< StateGet wrapping a state-var read (or, post
		                 ///<   `makeWritableTarget`, the bare BoxValueExpression /
		                 ///<   AppStateExpression)
		IndexedPath,     ///< IndexExpression into a state container
		FieldPath,       ///< FieldExpression onto a state-struct field
		TupleSlice,      ///< TupleItemExpression — destructured tuple element
	};

	Kind kind;
	std::shared_ptr<awst::Expression> expr;

	static StorageAlias mappingHolder(std::shared_ptr<awst::Expression> _e)
		{ return {Kind::MappingHolder, std::move(_e)}; }
	static StorageAlias stateRead(std::shared_ptr<awst::Expression> _e)
		{ return {Kind::StateRead, std::move(_e)}; }
	static StorageAlias indexedPath(std::shared_ptr<awst::Expression> _e)
		{ return {Kind::IndexedPath, std::move(_e)}; }
	static StorageAlias fieldPath(std::shared_ptr<awst::Expression> _e)
		{ return {Kind::FieldPath, std::move(_e)}; }
	static StorageAlias tupleSlice(std::shared_ptr<awst::Expression> _e)
		{ return {Kind::TupleSlice, std::move(_e)}; }

	/// The ONE shape→Kind ladder for binding a built argument as a storage
	/// alias (base-ctor args, modifier args, __postInit args — previously
	/// four verbatim lambda copies that could drift independently).
	static StorageAlias classify(std::shared_ptr<awst::Expression> _e)
	{
		if (dynamic_cast<awst::BytesConstant const*>(_e.get()))
			return mappingHolder(std::move(_e));
		if (dynamic_cast<awst::IndexExpression const*>(_e.get()))
			return indexedPath(std::move(_e));
		if (dynamic_cast<awst::FieldExpression const*>(_e.get()))
			return fieldPath(std::move(_e));
		if (dynamic_cast<awst::TupleItemExpression const*>(_e.get()))
			return tupleSlice(std::move(_e));
		return stateRead(std::move(_e));
	}
};

/// Declaration-ID bindings. Missing entries return a null pointer from find()
/// or a default value from get(); reads never insert into the table.
template<typename T>
class DeclBindings
{
public:
	T const* find(int64_t _id) const
	{
		auto it = m_values.find(_id);
		return it == m_values.end() ? nullptr : &it->second;
	}
	T get(int64_t _id) const
	{
		auto const* value = find(_id);
		return value ? *value : T{};
	}
	void set(int64_t _id, T _value) { m_values.insert_or_assign(_id, std::move(_value)); }
	void erase(int64_t _id) { m_values.erase(_id); }

private:
	std::unordered_map<int64_t, T> m_values;
};

/// Bindings for one emitted function frame (or the non-callable root scope).
/// Solc declaration IDs can recur when the same source body is lowered again;
/// only lexical blocks within a frame share these tables.
struct ScopeState
{
	/// Local `T storage p = …` aliases. Tag + expression; see StorageAlias.
	DeclBindings<StorageAlias> storageAliases;

	/// Slot-based storage refs for local pointers (`T storage p = base[i]`).
	DeclBindings<std::shared_ptr<awst::Expression>> slotStorageRefs;

	/// Function param/return decl ID → its name as a runtime bytes value
	/// (used as the box-key prefix for a `mapping(K=>V) storage` param).
	DeclBindings<std::string> mappingKeyParams;

	/// Struct storage-ref param decl ID → the name of its companion uint64 OFFSET param
	/// (handle-model dual handle). Present only for "offset-convention" params (those that
	/// receive an array-element ref `f(arr[i])` somewhere): `s.field` ops then hit the element
	/// slice via box_replace/box_extract(key, offset+fieldOff). Absent → whole-box (offset 0).
	DeclBindings<std::string> structRefOffsets;

	/// >4096 B memory aggregate: decl ID → uint64 local for EVM-memory base
	/// offset (FMP at allocation). Lives in multi-slot blob; `t.field[i]`
	/// lowers to blob read/write at base + offset. See SolIndexAccess.
	DeclBindings<std::string> blobAggregates;

	/// Memory-aggregate alias (handle-model copy-elision): decl ID → the source
	/// expression it aliases. `T memory b = a` registers b→a (only when neither is
	/// later reassigned) so b's references resolve to a's local — memory→memory
	/// ALIASES (EVM) instead of copying. Resolved in SolIdentifier before the var read.
	DeclBindings<std::shared_ptr<awst::Expression>> memoryAliases;

	/// Memory aggregate locals used as Yul pointer values in inline assembly.
	/// Promoted to blob-backed (pre-scan in ContractBuilder::buildBlock).
	std::unordered_set<int64_t> assemblyAggregates;

	/// Modifier-lowering param remap: unique mangled names per invocation
	/// when the same modifier is applied multiple times. Set/erased by the
	/// modifier-chain builder.
	DeclBindings<ParamRemap> paramRemaps;

};

struct FunctionContext;

/// Non-owning scope view. A null function denotes translation outside a callable
/// (for example a state initializer). No view refers to an enclosing block.
struct Context
{
	ScopeState& bindings;
	FunctionContext* function = nullptr;
	bool unchecked = false;

	bool isUnchecked() const { return unchecked; }
	bool isInConstructor() const;
	std::set<std::string>* liveCalldataPointers() const;
	int64_t callableId() const;

	/// AWST local name: params keep bare name (ABI-facing); locals/catch params
	/// mangle to `name__<declId>` to prevent shadow collisions in the flat AWST frame.
	std::string awstVarName(solidity::frontend::VariableDeclaration const& _vd) const;
};

/// Per-contract services and the non-callable root scope.
struct TranslationContext
{
	eb::ContractContext& contractCtx;
	TypeMapper& typeMapper;
	std::string sourceFile;

	/// Initializers outside an emitted function have their own bindings.
	ScopeState scopeState_;
	Context scope{scopeState_};

	TranslationContext(
		eb::ContractContext& _contractCtx,
		TypeMapper& _typeMapper,
		std::string _sourceFile
	)
		: contractCtx(_contractCtx),
		  typeMapper(_typeMapper),
		  sourceFile(std::move(_sourceFile))
	{}

	// The root view refers to this object's bindings; keep its address stable.
	TranslationContext(TranslationContext const&) = delete;
	TranslationContext(TranslationContext&&) = delete;
	TranslationContext& operator=(TranslationContext const&) = delete;
	TranslationContext& operator=(TranslationContext&&) = delete;

	awst::SourceLocation makeLoc(solidity::langutil::SourceLocation const& _sl) const
	{
		return typeMapper.sourceMap().toAwstLoc(sourceFile, _sl);
	}
};

/// Function-level context: signature info needed to translate the body.
struct FunctionContext
{
	/// Entry referents of locally pointer-backed value parameters. Rebinding
	/// changes the live offset, never the value returned to the caller here.
	std::map<int64_t, std::string> originalMemoryParams;
	TranslationContext& tr;
	ScopeState bindings;
	Context scope{bindings, this};
	std::vector<std::pair<std::string, awst::WType const*>> params;
	awst::WType const* returnType = nullptr;
	std::map<std::string, unsigned> paramBitWidths;
	/// solc callable AST identity, used to scope synthesized helpers.
	int64_t callableId = 0;
	/// Declared solc param types by BARE name; feeds
	/// AssemblyBuilder's EVM-ABI calldata layout. Assigned after construction.
	std::map<std::string, solidity::frontend::Type const*> paramSolTypes;

	/// Declared Solidity return components. Assembly `return(start,size)` uses
	/// these to recursively decode the EVM ABI region into the method's AWST
	/// return value instead of recognizing individual aggregate shapes.
	std::vector<solidity::frontend::Type const*> returnSolTypes;

	/// Struct storage-ref params passed as a box-key handle (bytes) because the
	/// body uses `param.slot` in asm (solady storage-lib idiom). name → the ARC4
	/// struct wtype, so `param.slot` resolves to a BoxValueExpression over the
	/// param's box key. Assigned after construction (buildFreestandingSubroutine).
	std::map<std::string, awst::WType const*> boxKeyStructParams;

	/// True while translating the constructor's own body. Constructor modifier
	/// methods configure their frame and return conventions independently.
	bool inConstructor = false;

	/// Internal/private function: assembly `return(o,s)` exits the whole program.
	/// Public/external functions are their own frame (AssemblyBuilder::setFrameIsProgram).
	bool frameIsProgram = false;

	/// Build-time ABI return encoding (fable-review-2 D2). When set, SolReturnStatement
	/// encodes each `return` value to its ABI wire type as it builds the statement —
	/// instead of walking the finished body after translation. The function builder
	/// populates these for non-modifier ABI-boundary methods before
	/// translating the body; `returnWirePlan` is per return element (see ReturnWirePlan.h),
	/// `returnAsmWrap` requests the `% 2^N` wrap asm bodies need (Yul is unchecked).
	bool encodeReturnsAtBuildTime = false;
	bool returnAsmWrap = false;
	std::vector<ReturnWireElem> returnWirePlan;

	/// Calldata params whose mutable (__cd_off_x, __cd_len_x) pointer locals are
	/// LIVE — seeded at an assembly block's entry or written via `x.offset := V`.
	/// Shared across the function's per-block AssemblyBuilders (else every block
	/// would re-seed from the canonical blob, clobbering an earlier block's write —
	/// calldata_offset_read_write) AND consulted by value reads of the param
	/// (SolIdentifier / the implicit-return synth read `extract3(__cd_blob, off,
	/// len)` instead of the decoded param). The function context outlives buildBlock,
	/// so no external mirror is needed.
	std::set<std::string> seededCalldataPointers;

	FunctionContext(
		TranslationContext& _tr,
		std::vector<std::pair<std::string, awst::WType const*>> _params,
		awst::WType const* _returnType,
		std::map<std::string, unsigned> _paramBitWidths
	)
		: tr(_tr),
		  params(std::move(_params)),
		  returnType(_returnType),
		  paramBitWidths(std::move(_paramBitWidths))
	{}

	/// Bind a source function using its solc declaration, physical boundary plan,
	/// and actual emitted arguments (wire-remapped for ABI methods).
	FunctionContext(TranslationContext& _tr,
		solidity::frontend::FunctionDefinition const& _function,
		std::vector<awst::SubroutineArgument> const& _args,
		awst::WType const* _returnType);

	// The scope view refers to this function; construct it in its final location.
	FunctionContext(FunctionContext const&) = delete;
	FunctionContext(FunctionContext&&) = delete;
	FunctionContext& operator=(FunctionContext const&) = delete;
	FunctionContext& operator=(FunctionContext&&) = delete;
};

/// Fresh lowering of the for-post or do/while test, before continue and at
/// fallthrough. Each expansion owns its AWST nodes and SingleEvaluation IDs.
struct LoopContext
{
	std::function<std::shared_ptr<awst::Statement>()> continuePrefix;
};

/// Block-local control flow and a flat view of the enclosing function.
struct BlockContext
{
	FunctionContext& fn;
	Context scope;
	LoopContext const* enclosingLoop = nullptr;
	PlaceholderFactory placeholderBody;

	BlockContext(
		FunctionContext& _fn,
		LoopContext const* _loop = nullptr,
		PlaceholderFactory _placeholderBody = {},
		bool _unchecked = false
	)
		: fn(_fn),
		  scope{_fn.scope.bindings, &_fn, _unchecked},
		  enclosingLoop(_loop),
		  placeholderBody(std::move(_placeholderBody))
	{}

	/// A child inherits lexical state and shares declaration-identity bindings.
	BlockContext nest() const
	{
		return {fn, enclosingLoop, placeholderBody, scope.unchecked};
	}

	BlockContext withLoop(LoopContext const& _loop) const
	{
		return {fn, &_loop, placeholderBody, scope.unchecked};
	}

	// ── Convenience accessors (bridge to underlying ContractContext) ──

	eb::ContractContext& builderCtx() const { return fn.tr.contractCtx; }
	TypeMapper& typeMapper() const { return fn.tr.typeMapper; }
	std::string const& sourceFile() const { return fn.tr.sourceFile; }
	awst::SourceLocation makeLoc(solidity::langutil::SourceLocation const& _sl) const
	{
		return fn.tr.makeLoc(_sl);
	}
};

} // namespace puyasol::builder::sol_ast
