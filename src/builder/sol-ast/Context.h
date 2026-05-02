#pragma once

/// @file Context.h
/// Typed nested contexts for Solidity AST traversal.
///
/// Every scope level inherits from `Context` and carries a parent pointer.
/// The chain terminates at `TranslationContext` (parent == nullptr). Future
/// state migrations push scope-bound fields into the appropriate subclass
/// and override base-class accessors to terminate the lookup walk when the
/// subclass owns the answer.
///
///   TranslationContext  — per-contract: type mapper, source file, the
///                          ContractContext (low-level translation state).
///   FunctionContext     — per-function: params, return type, param bit
///                          widths (for inline assembly packing).
///   BlockContext        — per-block: enclosing loop (for continue/break),
///                          placeholder body (for modifier inlining), parent
///                          link for nesting.
///   LoopContext         — per-loop: forLoopPost (i++ to run before
///                          continue) or doWhileCondBreak (cond at bottom).
///                          Currently orthogonal to the parent chain;
///                          referenced via BlockContext::enclosingLoop.
///
/// Visitors are *transient* and take the **narrowest context** they need.
/// When entering a nested scope, we derive a new context with `nest()`,
/// `withLoop()`, or `withPlaceholder()`, then construct a new visitor with
/// it. Stack-allocated; no save-and-restore.

#include "awst/Node.h"

#include <libsolidity/ast/AST.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace puyasol::builder
{
class TypeMapper;
namespace eb { class ContractContext; }
}

namespace puyasol::builder::sol_ast
{

/// Modifier-inliner param remap entry: when a modifier is applied
/// multiple times in a single function, each instance's locals get a
/// unique mangled name with their original AWST type.
struct ParamRemap
{
	std::string name;
	awst::WType const* type;
};


/// Common base for every scope level. Holds an upward parent pointer so
/// scope-bound lookups can be resolved by walking the chain. Non-virtual
/// where possible; virtual destructor so `delete` of a base pointer works
/// once we start storing them through Context*.
class Context
{
public:
	virtual ~Context() = default;

	/// Walk one level up. Returns nullptr at the root (TranslationContext).
	Context* parent() const { return m_parent; }

	// ── Scope-bound state accessors ──────────────────────────────────
	// Default impls chain to the parent so callers can always ask the
	// innermost scope without caring which level owns the answer.
	// Subclasses override what they own to terminate the walk.

	/// True if any ancestor scope is inside an `unchecked { }` block.
	virtual bool isUnchecked() const
	{
		return m_parent && m_parent->isUnchecked();
	}

	/// Storage-pointer alias: `mapping(K=>V) storage m = m1; ...; m[k]`
	/// resolves `m` to the same expression as `m1`. Returns nullptr if no
	/// alias is bound for this decl in any enclosing block.
	virtual std::shared_ptr<awst::Expression> findStorageAlias(int64_t _declId) const
	{
		return m_parent ? m_parent->findStorageAlias(_declId) : nullptr;
	}

	/// Variable-name → AST decl ID for shadow detection. Returns 0 if the
	/// name isn't bound in any enclosing block. Inner-block bindings shadow
	/// outer ones (chain walk returns the innermost hit).
	virtual int64_t lookupVarId(std::string const& _name) const
	{
		return m_parent ? m_parent->lookupVarId(_name) : 0;
	}

	/// Function-pointer target: the FunctionDefinition that a local
	/// `function (…) returns (…)` variable currently points at. Returns
	/// nullptr if no binding exists in any enclosing block. Used by
	/// SolInternalCall to lower an indirect call as a direct callsub.
	virtual solidity::frontend::FunctionDefinition const* findFuncPtrTarget(
		int64_t _declId
	) const
	{
		return m_parent ? m_parent->findFuncPtrTarget(_declId) : nullptr;
	}

	/// Compile-time constant value for a local: e.g. `uint x = 5` is
	/// folded so later uses of `x` inline `5`. Sentinel return is 0,
	/// matched against `>0` checks at the read site.
	virtual unsigned long long findConstantLocal(int64_t _declId) const
	{
		return m_parent ? m_parent->findConstantLocal(_declId) : 0ULL;
	}

	/// Slot-based storage ref for a local declaration: when a `T storage`
	/// pointer is assigned a slot expression, the binding lets later index
	/// accesses through the local resolve to the slot directly.
	virtual std::shared_ptr<awst::Expression> findSlotStorageRef(
		int64_t _declId
	) const
	{
		return m_parent ? m_parent->findSlotStorageRef(_declId) : nullptr;
	}

	/// True if the enclosing function is a constructor body. Constructor-only
	/// behaviour (e.g. immutable writes via direct app-global init) gates on
	/// this flag.
	virtual bool isInConstructor() const
	{
		return m_parent && m_parent->isInConstructor();
	}

	/// Mapping-storage-pointer parameter: function-scoped binding from a
	/// param/return decl ID to its name (used as the box-key prefix at
	/// runtime). Returns empty string if the decl isn't a mapping-storage
	/// param in any enclosing function.
	virtual std::string findMappingKeyParam(int64_t _declId) const
	{
		return m_parent ? m_parent->findMappingKeyParam(_declId) : std::string{};
	}

	/// Modifier-inliner param remap: when the same modifier is applied
	/// multiple times in a single function, each instance's locals get a
	/// unique mangled name. Returns nullptr if no remap is in effect.
	virtual ParamRemap const* findParamRemap(int64_t _declId) const
	{
		return m_parent ? m_parent->findParamRemap(_declId) : nullptr;
	}

	/// `super.X()` resolution: pre-computed per the contract's MRO so a
	/// call expression with target decl ID can route to the right base
	/// implementation. Returns empty string if not bound.
	virtual std::string findSuperTarget(int64_t _declId) const
	{
		return m_parent ? m_parent->findSuperTarget(_declId) : std::string{};
	}

protected:
	explicit Context(Context* _parent): m_parent(_parent) {}

	Context* m_parent;
};

/// Top-level translation context: per-contract state we share across
/// every function and statement. Wraps ContractContext (which still holds
/// the lower-level shared state like storage layout, library function IDs,
/// scope mappings); future cleanup can flatten more of that down here.
struct TranslationContext: Context
{
	eb::ContractContext& exprBuilder;
	TypeMapper& typeMapper;
	std::string sourceFile;

	/// Modifier-inliner param remaps. Lives at the translation level
	/// because modifier-body translation re-enters `ContractBuilder::buildBlock`
	/// (which creates a fresh FunctionContext parented to `*this`) while
	/// the remap is in effect — the chain walk from inside the modifier
	/// body reaches `TranslationContext` but not the outer FunctionContext.
	std::map<int64_t, ParamRemap> paramRemaps;

	/// `super.X()` MRO resolution map: AST decl ID → mangled super name.
	/// Set up per-function before its body is translated; cleared between
	/// function bodies. Lives at the translation level so inner buildBlock
	/// recursions (e.g. modifier inlining) can still see the bindings.
	std::unordered_map<int64_t, std::string> superTargetNames;

	TranslationContext(
		eb::ContractContext& _exprBuilder,
		TypeMapper& _typeMapper,
		std::string _sourceFile
	)
		: Context(nullptr),
		  exprBuilder(_exprBuilder),
		  typeMapper(_typeMapper),
		  sourceFile(std::move(_sourceFile))
	{}

	ParamRemap const* findParamRemap(int64_t _declId) const override
	{
		auto it = paramRemaps.find(_declId);
		return it != paramRemaps.end() ? &it->second : nullptr;
	}

	std::string findSuperTarget(int64_t _declId) const override
	{
		auto it = superTargetNames.find(_declId);
		return it != superTargetNames.end() ? it->second : std::string{};
	}

	awst::SourceLocation makeLoc(solidity::langutil::SourceLocation const& _sl) const
	{
		awst::SourceLocation loc;
		loc.file = sourceFile;
		loc.line = _sl.start >= 0 ? _sl.start : 0;
		loc.endLine = _sl.end >= 0 ? _sl.end : 0;
		return loc;
	}

	awst::SourceLocation makeLoc(int _start, int _end) const
	{
		awst::SourceLocation loc;
		loc.file = sourceFile;
		loc.line = _start >= 0 ? _start : 0;
		loc.endLine = _end >= 0 ? _end : 0;
		return loc;
	}
};

/// Function-level context: signature info needed to translate the body.
struct FunctionContext: Context
{
	TranslationContext& tr;
	std::vector<std::pair<std::string, awst::WType const*>> params;
	awst::WType const* returnType = nullptr;
	std::map<std::string, unsigned> paramBitWidths;

	/// True iff this function is a constructor body (or is being inlined
	/// into one). Set by ApprovalProgramBuilder around constructor inlining.
	bool inConstructor = false;

	/// Mapping-storage-pointer locals: function params (or returns) typed
	/// `mapping(K=>V) storage` carry their name as a runtime bytes value
	/// — `r[k]` resolves to a box-access prefixed by `r`'s holder name.
	std::map<int64_t, std::string> mappingKeyParams;


	FunctionContext(
		TranslationContext& _tr,
		std::vector<std::pair<std::string, awst::WType const*>> _params,
		awst::WType const* _returnType,
		std::map<std::string, unsigned> _paramBitWidths
	)
		: Context(&_tr),
		  tr(_tr),
		  params(std::move(_params)),
		  returnType(_returnType),
		  paramBitWidths(std::move(_paramBitWidths))
	{}

	bool isInConstructor() const override { return inConstructor; }

	std::string findMappingKeyParam(int64_t _declId) const override
	{
		auto it = mappingKeyParams.find(_declId);
		if (it != mappingKeyParams.end())
			return it->second;
		return m_parent ? m_parent->findMappingKeyParam(_declId) : std::string{};
	}
};

/// Loop-level context: control-flow targets for continue inside this loop.
/// `forLoopPost` is the post-step (e.g., `i++`) to splice in before
/// `LoopContinue`. `doWhileCondBreak` is the bottom-of-body condition check
/// for do/while. At most one of the two is set.
///
/// Currently *not* in the parent chain — referenced laterally through
/// `BlockContext::enclosingLoop`. We can weave it in later if loop-local
/// state ever needs scope lookup.
struct LoopContext
{
	std::shared_ptr<awst::Statement> forLoopPost;
	std::shared_ptr<awst::Statement> doWhileCondBreak;
};

/// Block/scope-level context: nesting chain, enclosing loop (for
/// continue/break), modifier placeholder body (for `_;` inlining).
struct BlockContext: Context
{
	FunctionContext& fn;
	BlockContext* outer = nullptr;
	LoopContext const* enclosingLoop = nullptr;
	std::shared_ptr<awst::Block> placeholderBody;

	/// True iff this block is itself an `unchecked { }` block. The
	/// effective unchecked-status (this + any ancestor) is exposed via
	/// `isUnchecked()`, which walks the chain.
	bool unchecked = false;

	/// Storage-pointer aliases bound in this block's lexical scope.
	std::map<int64_t, std::shared_ptr<awst::Expression>> storageAliases;

	/// Variable-name → AST decl ID for shadowing checks. Inserts go to the
	/// current block (the innermost when reading via `lookupVarId`).
	std::map<std::string, int64_t> varNameToId;

	/// Function-pointer targets: local `function` variables → the
	/// FunctionDefinition they're bound to. Used to lower `f()` (where `f`
	/// is a local function pointer) as a direct callsub.
	std::map<int64_t, solidity::frontend::FunctionDefinition const*> funcPtrTargets;

	/// Folded compile-time constant value for a local declaration. Bound
	/// when a `T x = LITERAL` declaration is encountered.
	std::unordered_map<int64_t, unsigned long long> constantLocals;

	/// Slot-based storage refs for local pointers (`T storage p = base[i]`).
	std::map<int64_t, std::shared_ptr<awst::Expression>> slotStorageRefs;

	bool isUnchecked() const override
	{
		return unchecked || (m_parent && m_parent->isUnchecked());
	}

	std::shared_ptr<awst::Expression> findStorageAlias(int64_t _declId) const override
	{
		auto it = storageAliases.find(_declId);
		if (it != storageAliases.end())
			return it->second;
		return m_parent ? m_parent->findStorageAlias(_declId) : nullptr;
	}

	int64_t lookupVarId(std::string const& _name) const override
	{
		auto it = varNameToId.find(_name);
		if (it != varNameToId.end())
			return it->second;
		return m_parent ? m_parent->lookupVarId(_name) : 0;
	}

	solidity::frontend::FunctionDefinition const* findFuncPtrTarget(
		int64_t _declId
	) const override
	{
		auto it = funcPtrTargets.find(_declId);
		if (it != funcPtrTargets.end())
			return it->second;
		return m_parent ? m_parent->findFuncPtrTarget(_declId) : nullptr;
	}

	unsigned long long findConstantLocal(int64_t _declId) const override
	{
		auto it = constantLocals.find(_declId);
		if (it != constantLocals.end())
			return it->second;
		return m_parent ? m_parent->findConstantLocal(_declId) : 0ULL;
	}

	std::shared_ptr<awst::Expression> findSlotStorageRef(
		int64_t _declId
	) const override
	{
		auto it = slotStorageRefs.find(_declId);
		if (it != slotStorageRefs.end())
			return it->second;
		return m_parent ? m_parent->findSlotStorageRef(_declId) : nullptr;
	}

	BlockContext(
		FunctionContext& _fn,
		BlockContext* _outer,
		LoopContext const* _loop,
		std::shared_ptr<awst::Block> _placeholderBody
	)
		: Context(_outer ? static_cast<Context*>(_outer) : static_cast<Context*>(&_fn)),
		  fn(_fn),
		  outer(_outer),
		  enclosingLoop(_loop),
		  placeholderBody(std::move(_placeholderBody))
	{}

	/// Construct the top-level block (function body root).
	static BlockContext top(FunctionContext& _fn)
	{
		return {_fn, nullptr, nullptr, nullptr};
	}

	/// Derive a child block context — same enclosing loop & placeholder.
	BlockContext nest()
	{
		return {fn, this, enclosingLoop, placeholderBody};
	}

	/// Derive a context whose body is the body of `_loop`.
	BlockContext withLoop(LoopContext const& _loop)
	{
		BlockContext c = nest();
		c.enclosingLoop = &_loop;
		return c;
	}

	/// Derive a context for the body of a modifier-inlined function:
	/// `_;` placeholders splice in `_body`.
	BlockContext withPlaceholder(std::shared_ptr<awst::Block> _body)
	{
		BlockContext c = nest();
		c.placeholderBody = std::move(_body);
		return c;
	}

	// ── Convenience accessors (bridge to underlying ContractContext) ──

	eb::ContractContext& builderCtx() const { return fn.tr.exprBuilder; }
	TypeMapper& typeMapper() const { return fn.tr.typeMapper; }
	std::string const& sourceFile() const { return fn.tr.sourceFile; }
	awst::SourceLocation makeLoc(solidity::langutil::SourceLocation const& _sl) const
	{
		return fn.tr.makeLoc(_sl);
	}
};

} // namespace puyasol::builder::sol_ast
