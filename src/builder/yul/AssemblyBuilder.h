#pragma once

#include <optional>

#include "awst/Node.h"
#include "builder/types/ReturnWirePlan.h"
#include "builder/target/ScratchLayout.h"
#include "builder/codec/SelectorSemantics.h"
#include "builder/types/TypeMapper.h"

#include <liblangutil/EVMVersion.h>
// ASTForward.h declares every yul node this header names (all by pointer or
// reference), so libyul/AST.h would add ~223k preprocessed lines for nothing.
// DebugData is the one thing AST.h was also supplying: makeLoc takes a
// DebugData::ConstPtr, and a NESTED typedef needs the definition. Its own
// header is 61k lines against AST.h + Dialect.h at 354k, so the swap is a net
// ~293k-line saving for each of the 28 TUs that include this file.
#include <liblangutil/DebugData.h>
#include <libyul/ASTForward.h>
// ASTAnnotations.h has to stay: InlineAssemblyAnnotation::ExternalIdentifierInfo
// is a NESTED type held BY VALUE in m_context->externalRefs, and nested types cannot be
// forward-declared.
#include <libsolidity/ast/ASTAnnotations.h>

namespace solidity::yul
{
/// Only ever named as `Dialect const&` here; the definition belongs to the .cpp
/// files that actually call into it.
class Dialect;
}

#include <functional>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace puyasol::builder
{

struct PreparedAssembly;
class TransientStorage;

/// Builds AWST nodes from Yul inline assembly blocks.
///
/// Translates EVM Yul opcodes to equivalent AVM operations using biguint arithmetic
/// (EVM uint256 ↔ AVM biguint), a scratch-slot-backed memory blob, and calldata mapping.
///
/// Memory model: EVM linear memory is simulated using AVM scratch slots.
/// mload/mstore translate to extract3/replace3 on the holding slot, supporting
/// dynamic offsets. Every slot (incl. slot 0) is read/written directly in
/// scratch via loads/stores — there is no __evm_memory local cache.
///
/// Implementation is split across multiple files by operation category:
///   - AssemblyBuilder.cpp      — Core: constructor, memory init, type coercion
///   - CoreTranslation.cpp      — Expression dispatch, literals, identifiers, function calls
///   - ArithmeticOps.cpp        — add, mul, mod, sub, mulmod, addmod, eq, lt, gt, and, or, not, xor
///   - BitwiseShiftOps.cpp      — shl, shr, div, byte, signextend, sload, gas, timestamp
///   - SignedOps.cpp             — sdiv, smod, slt, sgt, sar, tload, tstore, isNegative256, negate256
///   - DataOps.cpp              — calldataload, resolveConstantYulValue, keccak256
///   - MemoryHelpers.cpp        — readMemSlot, padTo32Bytes, concatSlotsRT, storeResultToMemory
///   - MemoryOps.cpp            — mload, mstore, handleReturn, tryHandleBytesMemoryRead
///   - PrecompileDispatch.cpp   — Routes call/staticcall to specific precompile handlers
///   - lowering/itxn/Precompile.cpp      — shared Solidity/Yul precompile algorithms
///   - StatementOps.cpp         — Yul statement translation: let, :=, expression stmts, functions
class AssemblyBuilder
{
public:
	AssemblyBuilder(
		TypeMapper& _typeMapper,
		std::string const& _sourceFile,
		std::string const& _contextName,
		bool _inConstructor = false
	);

	/// Box-keyed struct pointer surfaced via `.slot` (e.g. Uniswap V4 Pool.updateTick:
	/// `TickInfo storage info = self.ticks[tick]; sstore(info.slot, …)`).
	/// Unlike a numeric EVM slot, this aliases an ARC4 struct in a box; carries the
	/// box key + struct type so `sstore` can do a field-aware write (EVM packing → ARC4).
	struct BoxKeyedSlot
	{
		std::shared_ptr<awst::Expression> key; ///< box_key expression
		awst::WType const* structType = nullptr; ///< ARC4Struct stored in the box
	};

	/// A direct `.slot` reference to a scalar state var, for routing asm sstore/sload
	/// to that var's own storage (app-global) instead of the __dyn_storage blob.
	struct StateVarSlot
	{
		std::string varName;
		awst::WType const* wtype = nullptr;
	};

	/// Compile-time route for a CONSTANT storage slot number: connects raw-slot
	/// asm (sload/sstore at a folded constant) to the NAMED variable's real
	/// storage. Kinds mirror the EVM layout rules:
	///  - Scalar:    full-slot state var → its app-global.
	///  - ArrayRoot: dynamic array's root slot holds its LENGTH (read = element
	///               count; write = RESIZE the backing box).
	///  - ArrayData: the keccak256(root-slot) data region — slot K+i is element i
	///               (32-byte elements). The keccak is computed at COMPILE time
	///               (util::keccak256 in the C++ compiler, zero opcodes); routing
	///               is by constant comparison, never runtime hashing.
	struct SlotRoute
	{
		enum class Kind { Scalar, ArrayRoot, ArrayData, StructMemberArrayRoot };
		Kind kind = Kind::Scalar;
		std::string varName;
		awst::WType const* wtype = nullptr;   ///< Scalar: var's wtype; StructMemberArrayRoot: the STRUCT's ARC4Struct
		std::string dataBase;                 ///< ArrayData: decimal K (region base)
		std::string fieldName;                ///< StructMemberArrayRoot: the dyn-array member
		unsigned elementSize = 0;             ///< ArrayRoot/Data: fixed ARC4 element width; 0 means dynamic
	};

	/// Exact-slot routes (decimal slot string → route) + data regions
	/// ([K, K+2^32) element windows). See SlotRoute.
	void setSlotRoutes(
		std::map<std::string, SlotRoute> _exact, std::vector<SlotRoute> _regions)
	{
		prepareContext().slotRoutes = std::move(_exact);
		prepareContext().slotDataRegions = std::move(_regions);
	}

	/// Register signed intN (N<=64) locals whose bare Yul read must sign-extend
	/// to the canonical 256-bit word (see m_context->signedParamBits).
	void setSignedParamBits(std::map<std::string, unsigned> _m)
	{
		prepareContext().signedParamBits = std::move(_m);
	}

	void setReturnSolTypes(
		std::vector<solidity::frontend::Type const*> _types)
	{
		m_frame.returnSolTypes = std::move(_types);
	}

	/// ARC-4 router selector → Solidity selector mappings for msg.data-style
	/// synthetic calldata inside inline assembly.
	void setSelectorRoutes(std::vector<SelectorRoute> _routes)
	{
		prepareContext().selectorRoutes = std::move(_routes);
	}

	/// When true, EVM `return(o,s)` lowers as a program halt (internal/private frame).
	/// For public/external functions it lowers as a subroutine return (caller continues).
	void setFrameIsProgram(bool _v) { m_frame.frameIsProgram = _v; }
	/// The enclosing ABI method's return wire plan (null when returns stay
	/// native, e.g. modifier chains): `return(ptr, len)` values pass through it
	/// so they match the method's wire return type.
	void setReturnWirePlan(std::vector<builder::ReturnWireElem> const* _plan, bool _asmWrap)
	{
		m_frame.returnWirePlan = _plan;
		m_frame.returnAsmWrap = _asmWrap;
	}

	std::vector<std::shared_ptr<awst::Statement>> buildBlock(
		PreparedAssembly const& _assembly,
		std::vector<std::pair<std::string, awst::WType const*>> const& _params,
		awst::WType const* _returnType,
		std::map<std::string, std::string> const& _constants = {},
		std::map<std::string, unsigned> const& _paramBitWidths = {},
		std::map<std::string, std::string> const& _storageSlotVars = {},
		std::map<std::string, BoxKeyedSlot> const& _boxKeyedStructSlots = {},
		std::map<std::string, std::string> const& _blobOffsetVars = {},
		std::map<std::string, std::string> const& _structRefSlotLocals = {},
		std::map<std::string, StateVarSlot> const& _stateVarSlots = {},
		std::function<std::string(solidity::frontend::VariableDeclaration const&)> _declName = {},
		/// Number of leading _params that are the function's real CALLDATA args (the rest are
		/// external refs / return vars appended by SolInlineAssembly). The synthetic calldata
		/// blob + offset map are built from ONLY these — using the full augmented list inflates
		/// the EVM-ABI head and breaks .offset/.length. Default = all (back-compat).
		size_t _numCalldataParams = ~size_t(0)
	);

	/// Extract function name from a Yul FunctionName (Identifier or BuiltinName).
	std::string getFunctionName(solidity::yul::FunctionName const& _name) const;

	/// AWST name for a Yul external ref: locals, function members and calldata
	/// coordinates use _declName; state vars/constants/storage coordinates stay bare.
	/// Shared by resolveVarRef and SolInlineAssembly augmentedParams keying.
	static std::string externalRefAwstName(
		solidity::frontend::InlineAssemblyAnnotation::ExternalIdentifierInfo const& _info,
		std::string const& _bareName,
		std::function<std::string(solidity::frontend::VariableDeclaration const&)> const& _declName);

	// ── Memory scratch layout ──────────────────────────────────────────

	static constexpr int SLOT_SIZE = ScratchLayout::slotSize;

	/// Scratch slot for EIP-1153 transient storage: the slot right after the
	/// memory pages (layout: pages 0..N-1, transient N, flash N+1..N+10).
	/// 4096-byte zeroed blob; persists across callsub within one app call;
	/// cleared per-txn (matches Solidity transient semantics). Instance method
	/// because the number now depends on --evm-memory-slots; the historical
	/// FLASH_SCRATCH_* constants were consumed by nothing and are gone.
	int transientSlot() const { return scratchLayout().transientSlot(); }
	void setTransientStorage(TransientStorage const* _storage) { prepareContext().transientStorage = _storage; }
	void setWordBindings(std::map<std::string, std::string> bindings) { m_frame.wordShadow = std::move(bindings); }

	void setFunctionCalldata(bool present) { m_frame.functionCalldata = present; }
	void prepareCalldata(
		std::vector<std::pair<std::string, awst::WType const*>> const& params,
		std::vector<std::shared_ptr<awst::Statement>>& out,
		awst::SourceLocation const& loc, bool transactionInput);


	/// Base names of dynamic-CALLDATA pointer vars referenced by this block
	/// (from the refs' declarations — covers calldata return vars / locals whose
	/// suffixed refs register under the dotted name so m_frame.locals misses the base).
	void setCalldataPointerNames(std::set<std::string> _names)
	{
		m_frame.calldataPointerNames = std::move(_names);
	}

	/// STATIC calldata pointers (structs / fixed arrays) referenced by this block:
	/// their bare Yul name reads/writes __cd_off_<name> (the byte offset of their
	/// data in __cd_blob) — `s := s2` / `s := 0x24` / `s := t` semantics.
	void setCalldataStaticPtrNames(std::set<std::string> _names)
	{
		m_frame.calldataStaticPtrNames = std::move(_names);
	}

	/// Advance the FMP (configured first memory slot, offset 0x40) by `_size` bytes.
	/// Mirrors EVM allocation semantics for `T memory t;` locals so mload(0x40) is correct.
	/// `_uniqueId` namespaces the temporary blob-handle local.
	static std::vector<std::shared_ptr<awst::Statement>> emitFreeMemoryBump(
		ScratchLayout const& _scratch,
		int _size, awst::SourceLocation const& _loc, int _uniqueId);

	/// Bind `_offVar` to the current FMP and advance it by a runtime-sized,
	/// 32-byte-rounded region.  This is the common allocator for recursive EVM
	/// memory aggregates; emitBytesBlobAlloc additionally writes a length word.
	static std::vector<std::shared_ptr<awst::Statement>> emitMemoryAlloc(
		ScratchLayout const& _scratch,
		std::shared_ptr<awst::Expression> _sizeU64,
		std::string const& _offVar,
		int _uniqueId, awst::SourceLocation const& _loc);

	/// Allocate a `new bytes(len)` / `new string(len)` in the memory blob for asm use:
	/// binds `_offVar` (uint64) to the current FMP (the EVM pointer), writes the
	/// 32-byte length word at that offset, and bumps FMP by 32 + ceil(len/32)*32.
	/// `_lenU64` is the runtime length (uint64). The buffer's data lives at
	/// `_offVar + 32`, matching EVM string/bytes memory layout, so `add(buf, 32)` in
	/// asm points at the data and value-reads materialise [len word][data].
	static std::vector<std::shared_ptr<awst::Statement>> emitBytesBlobAlloc(
		TypeMapper& _typeMapper,
		std::shared_ptr<awst::Expression> _lenU64, std::string const& _offVar,
		int _uniqueId, awst::SourceLocation const& _loc);

	/// Read [off, off+len) from the multi-slot memory blob as a stack VALUE,
	/// stitching a SLOT_SIZE straddle. Expression-only (emits no statements),
	/// so it is usable from return-value positions. A stack value is at most
	/// one AVM element (SLOT_SIZE bytes) and therefore spans at most 2 slots;
	/// larger stack values are rejected explicitly.
	/// _offsetAlignMod32: the caller's proof of the offset's residue (see
	/// alignmentMod32). Residue 0 with a <=32-byte length means the access
	/// cannot cross a slot boundary, so the straddle arm is not emitted.
	/// Absent = assume unaligned, which is always correct, just larger.
	static std::shared_ptr<awst::Expression> readMemStackRange(
		ScratchLayout const& _scratch,
		std::shared_ptr<awst::Expression> _offset,
		std::shared_ptr<awst::Expression> _length,
		awst::SourceLocation const& _loc,
		std::optional<unsigned> _offsetAlignMod32 = std::nullopt);

	/// Read one word from shared scratch memory. Constants and proven-aligned
	/// offsets stay inline; other offsets call the shared cross-page reader.
	/// Both paths check the complete word's bounds. `_offset` is already uint64.
	static std::shared_ptr<awst::Expression> readMemWordDirect(
		TypeMapper& _typeMapper,
		std::shared_ptr<awst::Expression> _offset,
		awst::SourceLocation const& _loc,
		std::optional<unsigned> _offsetAlignMod32 = std::nullopt
	);

	/// Read `_byteLen` bytes at a DYNAMIC offset by concatenating successive 32-byte words
	/// (slot-routed via readMemWordDirect). For materialising a small (<=SLOT_SIZE)
	/// aggregate value from the blob. `_byteLen` assumed 32-aligned; trimmed if not.
	static std::shared_ptr<awst::Expression> readMemRangeDirect(
		TypeMapper& _typeMapper,
		std::shared_ptr<awst::Expression> _offset,
		int _byteLen,
		awst::SourceLocation const& _loc
	);

	/// Write a RUNTIME-LENGTH byte string into the blob starting at `_offU64`
	/// through the shared range writer, zero-padding only its final partial
	/// word. `_uniqueId` namespaces the pinned value.
	static void writeMemBytesDirect(
		TypeMapper& _typeMapper,
		std::shared_ptr<awst::Expression> _offU64,
		std::shared_ptr<awst::Expression> _bytesValue,
		int _uniqueId,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);

	/// Write one word, inline for constant/proven-aligned offsets and through
	/// the shared cross-page writer otherwise. Both paths check bounds.
	static void writeMemWordDirect(
		TypeMapper& _typeMapper,
		std::shared_ptr<awst::Expression> _offset,
		std::shared_ptr<awst::Expression> _value32,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out,
		std::optional<unsigned> _offsetAlignMod32 = std::nullopt
	);

	/// Write exactly one byte at a dynamic EVM-memory offset.  Unlike the word
	/// writer this remains valid at the last byte of a scratch slot/blob and is
	/// used for Solidity bytes/string element stores.
	static void writeMemByteDirect(
		ScratchLayout const& _scratch,
		std::shared_ptr<awst::Expression> _offset,
		std::shared_ptr<awst::Expression> _valueByte,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);

	/// Assert a 32-byte access at `_off` stays within the modeled blob. Reverts clearly
	/// on overflow rather than silently corrupting non-memory scratch or hitting an
	/// opaque AVM error (slot>255). Raise `--evm-memory-slots` if more memory is needed.
	static std::shared_ptr<awst::Statement> memBoundsAssert(
		ScratchLayout const& _scratch,
		std::shared_ptr<awst::Expression> _off,
		awst::SourceLocation const& _loc
	);

private:
	static std::shared_ptr<awst::Expression> checkedMemoryRangeOffset(
		ScratchLayout const& _scratch,
		std::shared_ptr<awst::Expression> _offset,
		std::shared_ptr<awst::Expression> _length,
		awst::SourceLocation const& _loc, bool _stackValue = true);
	static std::string memoryBufferSubroutine(
		TypeMapper& _typeMapper, bool _write, awst::SourceLocation const& _loc,
		bool _byteRange = false);
	static void writeMemRangeInline(
		ScratchLayout const& _scratch,
		std::shared_ptr<awst::Expression> _offset,
		std::shared_ptr<awst::Expression> _value32,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out,
		std::optional<unsigned> _offsetAlignMod32, bool _word = false);

	ScratchLayout const& scratchLayout() const
	{
		return m_typeMapper.profile().scratchLayout;
	}
	int memorySlotFirst() const { return scratchLayout().memoryFirst(); }
	int memorySlotCount() const { return scratchLayout().memoryCount(); }

	// ── Expression translation ──────────────────────────────────────────

	std::shared_ptr<awst::Expression> buildExpression(
		solidity::yul::Expression const& _expr
	);
	std::shared_ptr<awst::Expression> buildFunctionCall(
		solidity::yul::FunctionCall const& _call
	);
	std::shared_ptr<awst::Expression> buildLiteral(
		solidity::yul::Literal const& _lit
	);
	std::shared_ptr<awst::Expression> buildIdentifier(
		solidity::yul::Identifier const& _id
	);
	/// Evaluate right-to-left, capturing reads before a later operand can
	/// change their state. Only solc-proven movable expressions stay deferred.
	std::vector<std::shared_ptr<awst::Expression>> buildOperands(
		std::vector<solidity::yul::Expression const*> const& _operands,
		awst::SourceLocation const& _loc, std::vector<std::shared_ptr<awst::Statement>>& _out);
	std::vector<std::shared_ptr<awst::Expression>> buildCallOperands(
		solidity::yul::FunctionCall const& _call,
		std::vector<std::shared_ptr<awst::Statement>>& _out);

	/// The one way to name the outer Solidity var a Yul identifier references:
	/// mangled local name for registered externals, else the bare Yul name. Every
	/// assembly site naming an outer var must use this, not a raw name.str().
	std::string resolveVarRef(solidity::yul::Identifier const& _id) const;

	// ── Statement translation ───────────────────────────────────────────

	void buildStatement(
		solidity::yul::Statement const& _stmt,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);
	void buildVariableDeclaration(
		solidity::yul::VariableDeclaration const& _decl,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);
	void buildAssignment(
		solidity::yul::Assignment const& _assign,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);
	bool emitUserFunctionAssignment(solidity::yul::FunctionCall const& _call,
		std::vector<std::string> const& _targets, awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out);
	/// One plain-name Yul write with every Solidity-representation redirect
	/// (signed shadow, blob-backed pointer, static calldata pointer) plus
	/// target-typed coercion. Shared by single- and multi-var assignments.
	void emitPlainYulAssignment(
		std::string name,
		std::shared_ptr<awst::Expression> value,
		awst::SourceLocation const& loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);
	void buildExpressionStatement(
		solidity::yul::ExpressionStatement const& _stmt,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);
	void buildFunctionDefinition(
		solidity::yul::FunctionDefinition const& _def,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);

	// ── Control flow ────────────────────────────────────────────────────
	// (implementations in ControlFlowOps.cpp)

	void buildIfStatement(
		solidity::yul::If const& _node,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);
	void buildForLoop(
		solidity::yul::ForLoop const& _node,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);
	void buildBreakStatement(
		solidity::yul::Break const& _node,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);
	void buildContinueStatement(
		solidity::yul::Continue const& _node,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);
	void buildLeaveStatement(
		solidity::yul::Leave const& _node,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);
	void buildSwitchStatement(
		solidity::yul::Switch const& _node,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);

	// ── Builtin handlers ────────────────────────────────────────────────

	std::shared_ptr<awst::Expression> handleMulmod(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc
	);
	std::shared_ptr<awst::Expression> handleAddmod(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc
	);
	std::shared_ptr<awst::Expression> handleAdd(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc
	);
	std::shared_ptr<awst::Expression> handleMul(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc
	);
	std::shared_ptr<awst::Expression> handleExp(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc
	);
	std::shared_ptr<awst::Expression> handleMod(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc
	);
	std::shared_ptr<awst::Expression> handleMload(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc
	);
	void handleMstore(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);
	void handleMstore8(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);
	/// EVM `return(o,s)` halt: log(0x151f7c75 ++ ARC4(value)) + AVM `return 1`.
	void emitArc4ReturnHalt(
		std::shared_ptr<awst::Expression> _value,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out);

	void handleReturn(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);
	std::shared_ptr<awst::Expression> handleSub(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc
	);
	std::shared_ptr<awst::Expression> handleIszero(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc
	);
	std::shared_ptr<awst::Expression> handleEq(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc
	);
	std::shared_ptr<awst::Expression> handleLt(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc
	);
	std::shared_ptr<awst::Expression> handleGt(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc
	);
	std::shared_ptr<awst::Expression> handleAnd(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc
	);
	std::shared_ptr<awst::Expression> handleOr(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc
	);
	std::shared_ptr<awst::Expression> handleNot(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc
	);
	std::shared_ptr<awst::Expression> handleXor(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc
	);

	// ── ArithmeticOps shared helpers ────────────────────────────────────
	// Arity guard: logs error + returns false when _args doesn't hold exactly _n.
	bool checkArity(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		size_t _n, char const* _name, awst::SourceLocation const& _loc,
		char const* _hint = nullptr
	);
	// Drain pending statements [_from, end) into _out. Memory-bounds asserts and
	// inlined-fn side effects must precede the statement that consumes the expression.
	void drainPendingStatements(
		std::vector<std::shared_ptr<awst::Statement>>& _out, size_t _from = 0);
	std::shared_ptr<awst::Expression> makeYulCompare(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::NumericComparison _cmp, char const* _name,
		awst::SourceLocation const& _loc
	);
	std::shared_ptr<awst::Expression> makeYulBitwise(
		char const* _op,
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		char const* _name, awst::SourceLocation const& _loc
	);

	std::shared_ptr<awst::Expression> handleGas(
		awst::SourceLocation const& _loc
	);
	std::shared_ptr<awst::Expression> handleTimestamp(
		awst::SourceLocation const& _loc
	);
	std::shared_ptr<awst::Expression> handleSload(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc
	);

	// ── Special builtins (buildFunctionCall dispatch tables) ────────────
	// One handler per table row; hard-error stubs and mocked environment
	// values live here too so buildFunctionCall stays a pure dispatcher.
	// Arg-consuming rows of kArgsBuiltins:
	std::shared_ptr<awst::Expression> handleExtcodesize(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc);
	std::shared_ptr<awst::Expression> handleBalance(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc);
	std::shared_ptr<awst::Expression> handleClz(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc);
	std::shared_ptr<awst::Expression> handleReturndatacopy(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc);
	std::shared_ptr<awst::Expression> handleCalldatacopy(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc);
	// Nullary rows of kNullaryBuiltins (args ignored by the EVM builtin):
	std::shared_ptr<awst::Expression> handleAddress(awst::SourceLocation const& _loc);
	std::shared_ptr<awst::Expression> handleOrigin(awst::SourceLocation const& _loc);
	std::shared_ptr<awst::Expression> handleCaller(awst::SourceLocation const& _loc);
	std::shared_ptr<awst::Expression> handleBlockhash(awst::SourceLocation const& _loc);
	std::shared_ptr<awst::Expression> handleBlobhash(awst::SourceLocation const& _loc);
	std::shared_ptr<awst::Expression> handleDifficulty(awst::SourceLocation const& _loc);
	std::shared_ptr<awst::Expression> handlePrevrandao(awst::SourceLocation const& _loc);
	std::shared_ptr<awst::Expression> handleNumber(awst::SourceLocation const& _loc);
	std::shared_ptr<awst::Expression> handleSelfbalance(awst::SourceLocation const& _loc);
	std::shared_ptr<awst::Expression> handleCoinbase(awst::SourceLocation const& _loc);
	std::shared_ptr<awst::Expression> handleGasprice(awst::SourceLocation const& _loc);
	std::shared_ptr<awst::Expression> handleBasefee(awst::SourceLocation const& _loc);
	std::shared_ptr<awst::Expression> handleBlobbasefee(awst::SourceLocation const& _loc);
	std::shared_ptr<awst::Expression> handleChainid(awst::SourceLocation const& _loc);
	std::shared_ptr<awst::Expression> handleGaslimit(awst::SourceLocation const& _loc);
	std::shared_ptr<awst::Expression> handleCodesize(awst::SourceLocation const& _loc);
	std::shared_ptr<awst::Expression> handleExtcodehash(awst::SourceLocation const& _loc);
	std::shared_ptr<awst::Expression> handlePop(awst::SourceLocation const& _loc);
	std::shared_ptr<awst::Expression> handleTstoreExpr(awst::SourceLocation const& _loc);
	std::shared_ptr<awst::Expression> handleDelegatecall(awst::SourceLocation const& _loc);
	std::shared_ptr<awst::Expression> handleCreate2(awst::SourceLocation const& _loc);
	std::shared_ptr<awst::Expression> handleCalldatasize(awst::SourceLocation const& _loc);

	/// Yul div(a, b): unsigned integer floor division (biguint).
	std::shared_ptr<awst::Expression> handleDiv(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc
	);

	/// Yul shl(shift, value): logical left shift → value * 2^shift.
	std::shared_ptr<awst::Expression> handleShl(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc
	);

	/// Yul shr(shift, value): logical right shift → value / 2^shift.
	std::shared_ptr<awst::Expression> handleShr(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc
	);

	/// shl/shr shared lowering: value * 2^shift mod 2^256 (left) or
	/// value / 2^shift (right); 0 when shift ≥ 256 (EIP-145).
	std::shared_ptr<awst::Expression> buildLogicalShift(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		char const* _name, bool _left, awst::SourceLocation const& _loc
	);

	/// Yul byte(n, x): extract byte n from 32-byte big-endian value x.
	std::shared_ptr<awst::Expression> handleByte(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc
	);

	/// Yul signextend(b, x): sign-extend from byte b to 256 bits.
	std::shared_ptr<awst::Expression> handleSignextend(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc
	);

	/// Yul tload(slot): read a canonical word from transient scratch.
	std::shared_ptr<awst::Expression> handleTload(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc
	);

	/// Yul tstore(slot, value): write transient scratch and invalidate its address shadow.
	void handleTstore(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);

	/// Yul sdiv(a, b): signed division (two's complement).
	std::shared_ptr<awst::Expression> handleSdiv(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc
	);

	/// Yul smod(a, b): signed modulo (two's complement).
	std::shared_ptr<awst::Expression> handleSmod(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc
	);

	/// sdiv/smod shared lowering: |a| op |b| with the sign re-applied
	/// (div: sign(a) XOR sign(b); mod: sign(a)); x/0 = x%0 = 0.
	std::shared_ptr<awst::Expression> buildSignedDivMod(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		char const* _name, bool _isDiv, awst::SourceLocation const& _loc
	);

	/// Yul slt(a, b): signed less-than (two's complement).
	std::shared_ptr<awst::Expression> handleSlt(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc
	);

	/// Yul sgt(a, b): signed greater-than (two's complement).
	std::shared_ptr<awst::Expression> handleSgt(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc
	);

	/// Yul sar(shift, value): arithmetic right shift (preserves sign).
	std::shared_ptr<awst::Expression> handleSar(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc
	);

	/// Yul sstore(slot, value): EVM storage write via __storage_write, or — when
	/// the slot aliases a box-keyed ARC4 struct — a field-aware box write.
	void handleSstore(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);

	/// Lower `sstore(structRef.slot, packedWord)` for an ARC4 struct in a box (slot 0).
	/// EVM packs fields into one 256-bit slot; rebuilds the box bytes from `_packed`
	/// and the existing box for other fields. Fixed integers are mapped by byte range;
	/// Solidity's byte-lane bools are mapped to ARC-4's consecutive packed bool bits.
	/// Only slot 0 (bare `.slot`) handled; others fall through to the numeric-slot path.
	void handleBoxKeyedStructSlotStore(
		std::shared_ptr<awst::BoxValueExpression> const& _slotBox,
		std::shared_ptr<awst::Expression> const& _packed,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);

	/// `sload(s.slot)` on a box-keyed struct slot: read the EVM slot-0 packed
	/// word from the ARC4 box (inverse of handleBoxKeyedStructSlotStore).
	std::shared_ptr<awst::Expression> handleBoxKeyedStructSlotLoad(
		std::shared_ptr<awst::BoxValueExpression> const& _slotBox,
		awst::SourceLocation const& _loc
	);

	/// Lower `sstore(v.slot, value)` where `v` is a scalar app-global state var by
	/// writing `v`'s own app-global state (so a later high-level read of `v` sees it),
	/// instead of the generic __dyn_storage blob. Returns true if handled; false
	/// (e.g. not a tracked scalar app-global var) falls through to handleSstore.
	bool tryHandleStateVarSstore(
		solidity::yul::FunctionCall const& _call,
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out);

	/// sload(v.slot) on a scalar app-global state var → read v's own storage
	/// (mirrors tryHandleStateVarSstore). nullptr → fall back to __dyn_storage.
	std::shared_ptr<awst::Expression> tryHandleStateVarSload(
		solidity::yul::FunctionCall const& _call,
		awst::SourceLocation const& _loc);

	/// 2^shift via setbit(bzero(32), 255-shift, 1) (no bexp opcode on AVM).
	std::shared_ptr<awst::Expression> buildPowerOf2(
		std::shared_ptr<awst::Expression> _shift,
		awst::SourceLocation const& _loc
	);

	/// True when value's sign bit is set (bit 255 for biguint, bit 63 for uint64).
	/// _origType is the pre-ensureBiguint type; nullptr → biguint (256-bit).
	std::shared_ptr<awst::Expression> isNegative256(
		std::shared_ptr<awst::Expression> _val,
		awst::SourceLocation const& _loc
	);

	/// Negate a 256-bit two's complement value: ~x + 1 (mod 2^256).
	std::shared_ptr<awst::Expression> negate256(
		std::shared_ptr<awst::Expression> _val,
		awst::SourceLocation const& _loc
	);

	/// Read 32 bytes from calldata (maps to array param elements).
	std::shared_ptr<awst::Expression> handleCalldataload(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc
	);

	/// Hash memory region via keccak256(offset, length).
	std::shared_ptr<awst::Expression> handleKeccak256(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc
	);

	/// Complete result bytes captured at the latest modeled external call.
	/// Application results require the ARC4 return prefix; event logs are not
	/// data. returndatasize / returndatacopy / output copies use this buffer.
	std::shared_ptr<awst::Expression> returndataBytes(
		awst::SourceLocation const& _loc
	);

	/// EVM returndatasize() → len(returndataBytes()) as uint64.
	std::shared_ptr<awst::Expression> handleReturndatasize(
		awst::SourceLocation const& _loc
	);

	/// EVM returndatacopy: copy `size` result-buffer bytes from `offset` to `destOffset`.
	void emitReturndatacopy(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);

	/// Handle logN(offset, length, topic1..topicN): flatten topics ++ memory data
	/// into a single AVM `log`. _numTopics is 0..4.
	void handleLog(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		int _numTopics,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);

	/// Handle revert(offset, length): fail the transaction.
	void handleRevert(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);

	// ── Precompile dispatch ────────────────────────────────────────────

	/// Route call/staticcall to the matching precompile handler (_isCall: 7 args vs 6).
	void handlePrecompileCall(
		solidity::yul::FunctionCall const& _call,
		std::string const& _assignTarget,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out,
		bool _isCall
	);


	// ── Memory blob helpers ──────────────────────────────────────────

	/// Load blob from scratch slot (slot = _slot index, not byte offset).
	std::shared_ptr<awst::Expression> loadMemoryBlob(
		awst::SourceLocation const& _loc,
		int _slot = 0
	);

	/// Emit a store of the blob back to the scratch slot.
	void storeMemoryBlob(
		std::shared_ptr<awst::Expression> _blob,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out,
		int _slot = 0
	);

	/// Read 32 bytes from the blob at a constant offset → biguint.
	std::shared_ptr<awst::Expression> readMemSlot(
		uint64_t _offset,
		awst::SourceLocation const& _loc
	);

	/// Read a 32-byte word through the shared checked scratch-memory helper.
	std::shared_ptr<awst::Expression> readMemWordConst(
		uint64_t _offset,
		awst::SourceLocation const& _loc
	);

	/// Write a 32-byte word through the shared checked scratch-memory helper.
	void writeMemWordConst(
		uint64_t _offset,
		std::shared_ptr<awst::Expression> _value32,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);

	/// Read a 32-byte word at a DYNAMIC offset; offset < SLOT_SIZE reads slot 0,
	/// otherwise loads(offset/SLOT_SIZE). Straddles stitch two slots. Returns bytes.
	/// Bounds checks live in the read expression, including inside loops/branches.
	std::shared_ptr<awst::Expression> readMemWordDyn(
		std::shared_ptr<awst::Expression> _offset,
		awst::SourceLocation const& _loc
	);

	/// Write a 32-byte word at a DYNAMIC offset via `stores(slot, replace3(loads(slot), sub, value))`.
	void writeMemWordDyn(
		std::shared_ptr<awst::Expression> _offset,
		std::shared_ptr<awst::Expression> _value32,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);

	/// Gather [off, off+len) across scratch slots at RUNTIME offset+length
	/// (word-loop of readMemWordDyn). Emits statements into _out; returns a
	/// bytes VarExpression holding exactly len bytes (a snapshot: later
	/// writes to the region cannot alias it).
	std::shared_ptr<awst::Expression> readMemRangeDyn(
		std::shared_ptr<awst::Expression> _offset,
		std::shared_ptr<awst::Expression> _length,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);

	/// Scatter a bytes VALUE to [off, off+len(value)) across scratch slots at
	/// a RUNTIME offset, with mcopy's tail-keep so the word-loop writer never
	/// clobbers past the end.
	void writeMemRangeDyn(
		std::shared_ptr<awst::Expression> _offset,
		std::shared_ptr<awst::Expression> _value,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);

	/// Pad a biguint expression to exactly 32 zero-padded big-endian bytes.
	std::shared_ptr<awst::Expression> padTo32Bytes(
		std::shared_ptr<awst::Expression> _expr,
		awst::SourceLocation const& _loc
	);


	/// Try to extract a constant integer value from a Yul expression.
	std::optional<uint64_t> resolveConstantYulValue(
		solidity::yul::Expression const& _expr
	);
	std::optional<std::string> resolveConstantYulWord(
		solidity::yul::Expression const& _expr);
	/// EVM-style immutable buffer read: clamp before narrowing, then zero-pad
	/// only the missing suffix (not the entire source buffer).
	std::shared_ptr<awst::Expression> readPaddedBytes(
		std::shared_ptr<awst::Expression> _bytes,
		std::shared_ptr<awst::Expression> _offset,
		std::shared_ptr<awst::Expression> _length,
		awst::SourceLocation const& _loc);

	/// Match mload(add(add(bytes_param, 32), offset)) → extract3(param, offset, 32).
	std::shared_ptr<awst::Expression> tryHandleBytesMemoryRead(
		solidity::yul::Expression const& _addrExpr,
		awst::SourceLocation const& _loc
	);

	/// A Yul pointer expression addressing a bytes/string memory local's DATA region:
	/// add(m, O) → dataOff = O−32 (O skips the length word; constant O < 32 does NOT
	/// match — that's a length-word write, left to the generic path), or
	/// add(add(m, 32), k) → dataOff = k. Both shapes commuted too.
	struct BytesDataPtrMatch
	{
		std::string name;
		awst::WType const* type;
		std::shared_ptr<awst::Expression> dataOff; // uint64 byte index into the data
		/// add(local, O) absolute offset when O may address the 32-byte length
		/// header. Null for already-normalized data pointers.
		std::shared_ptr<awst::Expression> absoluteOff;
	};
	std::optional<BytesDataPtrMatch> matchBytesMemoryDataPtr(
		solidity::yul::Expression const& _addr,
		awst::SourceLocation const& _loc, bool _evaluate = true
	);

	/// Guarded in-place write of `_value32` at `_m.dataOff`:
	/// m = (off < len) ? replace3(m, off, slice) : m, slice truncated to len−off.
	void emitGuardedBytesDataWrite(
		BytesDataPtrMatch _m,
		std::shared_ptr<awst::Expression> _value32,
		int _sliceLen, // 32 = mstore (MSB-first), 1 = mstore8 (low byte)
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);

	/// Inverse of emitGuardedBytesDataWrite: read a 32-byte word at `_m.dataOff`
	/// from the bytes/string VALUE local (mirrors the write's backing so an asm
	/// mstore then mload of the same `new bytes` buffer round-trips). Bounds-safe:
	/// bytes at/past len read as 0 (EVM mload of fresh/adjacent memory).
	std::shared_ptr<awst::Expression> emitGuardedBytesDataRead(
		BytesDataPtrMatch _m,
		awst::SourceLocation const& _loc
	);

	/// Match mstore(<data ptr>, value) → guarded word write on the var (no blob write).
	/// mstore(<bare bytes/string local>, n): EVM length-word write = resize.
	bool tryHandleBytesMemoryLengthWrite(
		solidity::yul::FunctionCall const& _call,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out);

	bool tryHandleBytesMemoryWrite(
		solidity::yul::FunctionCall const& _call,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);

	/// Match mstore8(<data ptr>, value) → guarded one-byte write on the var.
	bool tryHandleBytesMemoryWrite8(
		solidity::yul::FunctionCall const& _call,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);

	/// Match mcopy(add(add(bytes_var, 0x20), dstOff), …) → replace3/extract3 on the var.
	/// Returns true if matched; false falls through to generic mcopy handler.
	bool tryHandleBytesMemoryMcopy(
		solidity::yul::FunctionCall const& _call,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);

	// ── Memory blob model ──────────────────────────────────────────────

	void initializeMemoryBlob(
		std::vector<std::pair<std::string, awst::WType const*>> const& _params,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);

	/// Read EVM-memory slot 0 directly from the configured first scratch slot.
	std::shared_ptr<awst::Expression> memoryVar(awst::SourceLocation const& _loc);

	/// Write EVM-memory slot 0 directly to the configured first scratch slot.
	void assignMemoryVar(
		std::shared_ptr<awst::Expression> _value,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);

	std::optional<uint64_t> resolveConstantOffset(
		std::shared_ptr<awst::Expression> const& _expr
	);

	std::shared_ptr<awst::Expression> offsetToUint64(
		std::shared_ptr<awst::Expression> _offset,
		awst::SourceLocation const& _loc
	);

	// ── Calldata model ──────────────────────────────────────────────────

	struct CalldataElement
	{
		std::string paramName;
		int flatIndex = 0;
		awst::WType const* paramType = nullptr;
	};


	void initializeCalldataMap(
		std::vector<std::pair<std::string, awst::WType const*>> const& _params
	);

	static constexpr char const* CD_BLOB_VAR = "__cd_blob";

public:
	void setCalldataSolTypes(std::map<std::string, solidity::frontend::Type const*> _m)
	{
		prepareContext().calldataSolTypes = std::move(_m);
	}

	/// Struct storage-ref params passed as a box-key handle (bytes) whose body
	/// uses `param.slot` in asm: name → ARC4 struct wtype. `param.slot` resolves
	/// to a BoxValueExpression over the param's box key. See asm-slot-storage-ref-param.
	void setBoxKeyStructParams(std::map<std::string, awst::WType const*> _m)
	{
		prepareContext().boxKeyStructParams = std::move(_m);
	}

private:

	solidity::frontend::Type const* calldataSolType(std::string const& _name) const
	{
		auto it = m_context->calldataSolTypes.find(_name);
		return it == m_context->calldataSolTypes.end() ? nullptr : it->second;
	}

	/// EVM-ABI head size of one calldata param: solc's calldataHeadSize when
	/// the declared type is known (statics inline their FULL encoded size in
	/// the head — `f(uint8[3] a, uint b)` puts b at 0x64, not 0x24), else the
	/// legacy flat-count heuristic.
	uint64_t calldataHeadSizeOf(std::string const& _name, awst::WType const* _type);

	/// One EVM-ABI 32-byte word for a scalar leaf value (sign extension for
	/// signed ints, left-alignment for bytesN — driven by the solc leaf type).
	std::shared_ptr<awst::Expression> evmCalldataWord(
		std::shared_ptr<awst::Expression> _value,
		solidity::frontend::Type const* _solLeaf,
		awst::SourceLocation const& _loc);

	/// The solc scalar leaf type at flat index `_i` of calldata param `_name`
	/// (statics flatten in EVM head order); nullptr when unknown.
	solidity::frontend::Type const* calldataSolLeaf(std::string const& _name, int _i);

	/// The raw ARC4 value + solc type of the `_wordIndex`-th EVM head word of
	/// a static aggregate, navigating the SOLC structure DIRECTLY (no head
	/// reconstruction). Same word granularity as emitEvmHeadWords; used by the
	/// constant-offset calldataload map hit.
	/// True when a declared solc param type is usable for EVM-ABI layout math
	/// (value type, or a calldata-located reference). Shared by the blob + map.
	static bool solTypeUsable(solidity::frontend::Type const* _t);

	std::pair<std::shared_ptr<awst::Expression>, solidity::frontend::Type const*>
	accessEvmLeaf(
		std::shared_ptr<awst::Expression> _value,
		awst::WType const* _wtype,
		solidity::frontend::Type const* _solType,
		int _wordIndex,
		awst::SourceLocation const& _loc);

	/// True when the leaf type changes the word VALUE vs the raw zero-padded
	/// native value (signed ints sign-extend; bytesN left-aligns).
	static bool leafNeedsEvmWord(solidity::frontend::Type const* _solLeaf);

	/// True iff any calldataload/copy/size in the block has a non-constant offset.
	bool detectDynamicCalldataAccess(solidity::yul::Block const& _block);

	/// Emit `__cd_blob = selector ++ head ++ tail` from param locals.
	void buildSyntheticCalldataBlob(
		std::vector<std::pair<std::string, awst::WType const*>> const& _params,
		std::vector<std::shared_ptr<awst::Statement>>& _out,
		awst::SourceLocation const& _loc
	);

	/// True when the WType's ARC4 encoding contains a dynamic component. Declared
	/// solc types remain authoritative where available; this recursive fallback
	/// also covers fixed arrays/structs containing dynamic members.
	bool isDynamicCalldataType(awst::WType const* _type) const;

	/// Runtime .offset / .length of a dynamically-encoded calldata param. solc's
	/// outer type determines whether its tail begins with a length word (T[]) or
	/// directly with tuple/fixed-array heads (struct / T[][N]).
	std::shared_ptr<awst::Expression> calldataDynOffset(
		uint64_t _headPos, solidity::frontend::Type const* _solType,
		awst::SourceLocation const& _loc);
	std::shared_ptr<awst::Expression> calldataDynLength(
		uint64_t _headPos, solidity::frontend::Type const* _solType,
		awst::SourceLocation const& _loc);

	/// Seed mutable `__cd_off_<name>` / `__cd_len_<name>` (offset,length) locals for each dynamic
	/// calldata param from the blob, so `.offset`/`.length` read them and `x.offset := V` can
	/// repoint x into __cd_blob (a value read becomes extract3(__cd_blob, off, len)).
	void initCalldataPointerLocals(
		std::vector<std::shared_ptr<awst::Statement>>& _out, awst::SourceLocation const& _loc);

	static int computeFlatElementCount(awst::WType const* _type);
	static int computeARC4ByteSize(awst::WType const* _type);
	std::shared_ptr<awst::Expression> accessFlatElement(
		std::shared_ptr<awst::Expression> _base,
		awst::WType const* _type,
		int _flatIndex,
		awst::SourceLocation const& _loc
	);

	// ── Variable tracking ───────────────────────────────────────────────

	/// Encode a frame return value through m_frame.returnWirePlan (spills go to _out).
	std::shared_ptr<awst::Expression> encodeFrameReturn(
		std::shared_ptr<awst::Expression> _value,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out);

	/// Drop all "mem_0x<off>" content constants. Called on memory
	/// writes that can't be tracked precisely (non-constant mstore offset, mstore8,
	/// mcopy, calldatacopy, returndatacopy, precompile output) and when entering/
	/// leaving if/switch/for translation (entries from a conditionally-executed body
	/// must not survive it; entries from before a loop must not fold inside it).
	void invalidateMemConstants();

	/// True when this Yul builtin writes EVM memory in a way the "mem_0x*"
	/// content tracker cannot model precisely (so every entry must be
	/// dropped). Shared by the expression and statement translation paths so
	/// the two can't drift. `mstore` is excluded: it tracks per-offset itself.
	static bool builtinClobbersMemory(std::string const& _name);

	/// Try to lower sload/sstore at a compile-time-CONSTANT slot directly to the
	/// named variable's storage (see SlotRoute). Returns the read expression /
	/// true when routed; nullptr / false to fall through to __storage_read/write.
	std::shared_ptr<awst::Expression> tryRouteConstSlotLoad(
		std::shared_ptr<awst::Expression> const& _slot,
		awst::SourceLocation const& _loc);
	bool tryRouteConstSlotStore(
		std::shared_ptr<awst::Expression> const& _slot,
		std::shared_ptr<awst::Expression> const& _value,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out);

	// ── Assembly function support ───────────────────────────────────────

	/// Call a Yul subroutine, or inline a helper requiring the Solidity return frame.
	std::shared_ptr<awst::Expression> handleUserFunctionCall(
		solidity::yul::FunctionCall const& _call,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);

	/// Emit a reachable Yul function with an isolated local scope, shared scratch
	/// memory, and an explicit calldata argument when solc's graph requires it.
	void buildYulSubroutine(
		solidity::yul::FunctionDefinition const& _funcDef,
		std::string const& _subroutineId,
		std::string const& _subroutineName
	);
	void emitYulSubroutineReturn(
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out);

private:

	// ── Utilities ───────────────────────────────────────────────────────

	awst::SourceLocation makeLoc(
		solidity::langutil::DebugData::ConstPtr const& _debugData
	);

	/// Coerce to biguint (Yul: all values are uint256); no-op if already biguint.
	std::shared_ptr<awst::Expression> ensureBiguint(
		std::shared_ptr<awst::Expression> _expr,
		awst::SourceLocation const& _loc
	);

	/// sload/sstore slot args only: like ensureBiguint, but a non-scalar slot
	/// expression (unmodeled `.slot` ref, e.g. a struct-member array alias)
	/// hard-errors with a slot-specific diagnostic. Historically this passed
	/// through unchecked and puya zero-init'd it — a silent wrong-slot write
	/// (struct_delete_storage_with_array only "passed" by luck).
	std::shared_ptr<awst::Expression> ensureBiguintSlotArg(
		std::shared_ptr<awst::Expression> _expr,
		awst::SourceLocation const& _loc
	);

	/// Coerce biguint/uint64 to bool (non-zero = true).
	std::shared_ptr<awst::Expression> ensureBool(
		std::shared_ptr<awst::Expression> _expr,
		awst::SourceLocation const& _loc
	);

	std::shared_ptr<awst::Expression> makeBigUIntBinOp(
		std::shared_ptr<awst::Expression> _left,
		awst::BigUIntBinaryOperator _op,
		std::shared_ptr<awst::Expression> _right,
		awst::SourceLocation const& _loc
	);

	std::shared_ptr<awst::Expression> makeTwoPow256(awst::SourceLocation const& _loc);

	/// Wrap mod 2^256 (EVM integer semantics).
	std::shared_ptr<awst::Expression> wrapMod256(
		std::shared_ptr<awst::Expression> _expr,
		awst::SourceLocation const& _loc
	);

	/// Offset's residue mod 32 when provable, else nullopt ("assume unaligned").
	/// A scratch slot is a multiple of 32, so a 32-byte access at residue r has
	/// at least 32-r bytes before the slot boundary: residue 0 can never
	/// straddle, and the second-slot arm is dead code. Sound under 64-bit
	/// wraparound because 32 divides 2^64. Deliberately has no division or
	/// right-shift rule — halving an aligned value is not aligned.
	std::optional<unsigned> alignmentMod32(awst::Expression const& _offset) const;


	/// True when a div/mod divisor is a compile-time non-zero constant: the EVM
	/// zero-divisor guard, and the three basic blocks its ternary costs, are then
	/// dead weight.
	bool divisorIsKnownNonZero(awst::Expression const& _divisor) const;

	std::shared_ptr<awst::Expression> safeDivMod(
		std::shared_ptr<awst::Expression> _left,
		awst::BigUIntBinaryOperator _op,
		std::shared_ptr<awst::Expression> _right,
		awst::SourceLocation const& _loc
	);

	/// Extract last 8 bytes then btoi; guards against b&/b|/b^ padding >8-byte biguints.
	std::shared_ptr<awst::Expression> safeBtoi(
		std::shared_ptr<awst::Expression> _biguintExpr,
		awst::SourceLocation const& _loc
	);


	TypeMapper& m_typeMapper;

	/// Configuration and prepared solc facts are shared read-only after
	/// registration. Host routing belongs here; Yul stack locals never do.
	struct Context
	{
		std::vector<SelectorRoute> selectorRoutes;

		std::vector<std::pair<std::string, awst::WType const*>> calldataParams;

		/// Declared solc types of the function's calldata params, by BARE name
		/// The EVM-ABI head layout (calldataHeadSize) and
		/// value widening (sign extension, static-aggregate leaf words) derive
		/// from these; absent entries fall back to the WType-based heuristics.
		std::map<std::string, solidity::frontend::Type const*> calldataSolTypes;

		TransientStorage const* transientStorage = nullptr;

		std::map<std::string, awst::WType const*> boxKeyStructParams;

		/// Solidity param bit-widths (uint16→16); used to truncate values on block exit.
		std::map<std::string, unsigned> paramBitWidths;

		/// SIGNED intN (N<=64) Solidity locals referenced in this asm block, name→bits.
		/// Their uint64-backed 64-bit-TC value is NOT the Yul word (an EVM identifier
		/// IS the full 256-bit word: int64 -1 = 0xFF..FF, so `bytes2(v)` takes 0xFFFF
		/// from the top, and `shr(128, z)` after `z := ...` sees real high bits).
		/// Wider signed (64<N<256) are biguint-backed canonical already — not registered.
		std::map<std::string, unsigned> signedParamBits;

		/// solc's SSAValueTracker view of the current block: single-assignment Yul
		/// locals bound to a NUMBER literal, ORIGINAL name → full-width decimal.
		/// localConstants is uint64 and silently drops anything wider (poseidon's
		/// BN254 field prime), which kept every mulmod's divide-by-zero guard alive:
		/// 816 guards chained ~2500 basic blocks, and puya's SSA reader recursed
		/// past its stack limit walking them.
		std::map<std::string, std::string> yulConstantValues;

		std::map<std::string, unsigned> yulArgumentAlignments;

		/// Yul locals that are the target of ANY `:=` assignment anywhere in the current
		/// assembly block (incl. nested blocks/loops and user function bodies, by ORIGINAL
		/// name). Such locals never enter localConstants: the fold is flow-insensitive,
		/// so a reassigned local's initializer constant would go stale (`let p := 0x80 …
		/// p := add(p, 0x20)` folded every mstore(p, …) to offset 0x80).
		std::set<std::string> reassignedLocals;

		/// Solidity `constant` vars referenced in assembly: name → decimal string.
		/// "__slot_"-prefixed values are storage-slot refs (see storageSlotVars).
		std::map<std::string, std::string> constants;

		/// "__slot_<varName>" → varName; drives sload/sstore storage translation.
		std::map<std::string, std::string> storageSlotVars;

		/// Dotted Yul name ("info.slot") → BoxKeyedSlot for box-struct sstore lowering.
		std::map<std::string, BoxKeyedSlot> boxKeyedStructSlots;

		/// Dotted yul name (`v.slot`) → scalar app-global state var, so sstore routes to
		/// the var's own app-global storage (not __dyn_storage). Populated by SolInlineAssembly.
		std::map<std::string, StateVarSlot> stateVarSlots;

		std::map<std::string, SlotRoute> slotRoutes;

		std::vector<SlotRoute> slotDataRegions;

		/// Dotted yul name (`ptr.slot`) → mangled biguint local holding a storage-ref
		/// slot handle. Lets `.slot` on a struct-storage-ref local resolve to the
		/// handle value instead of the (non-scalar) struct. Populated by SolInlineAssembly.
		std::map<std::string, std::string> structRefSlotLocals;

		/// solc's external refs for the current block (yul id ptr → {decl, suffix}).
		/// Pointer-keyed so a Yul-local shadowing an outer var isn't mis-resolved.
		std::map<solidity::yul::Identifier const*,
			solidity::frontend::InlineAssemblyAnnotation::ExternalIdentifierInfo> externalRefs;

		/// Resolves a VariableDeclaration to its AWST name (Context::awstVarName).
		std::function<std::string(solidity::frontend::VariableDeclaration const&)> declName;

		/// Collected assembly function definitions (populated during first pass).
		std::map<std::string, solidity::yul::FunctionDefinition const*> asmFunctions;

		/// solc-disambiguated Yul function name → AWST SubroutineID.
		std::map<std::string, std::string> yulFuncSubroutineIds;

		std::set<std::string> yulCalldataFunctions;

		std::set<std::string> yulMemoryWritingFunctions;

		solidity::yul::Dialect const* dialect = nullptr;

		std::string sourceFile;

		std::string contextName;

		bool inConstructor = false;
	};

	/// Every outlined Yul function starts with a value-initialized frame.
	/// Scratch memory, return data and transient storage are not frame locals.
	struct Frame
	{
		std::map<uint64_t, CalldataElement> calldataMap;

		/// True when dynamic calldataload/copy/size detected; materialise __cd_blob.
		bool useSyntheticCalldata = false;

		bool functionCalldata = false;

		std::set<std::string> calldataPointerNames;

		std::set<std::string> calldataStaticPtrNames;

		/// True after a halt (return/revert): skip trailing coercions (else puya: unreachable).
		bool haltEmitted = false;

		bool frameIsProgram = false;

		std::vector<builder::ReturnWireElem> const* returnWirePlan = nullptr;

		bool returnAsmWrap = false;

		std::map<std::string, awst::WType const*> locals;

		/// Narrow Solidity carriers retain the complete Yul word inside a block.
		/// The epilogue adapts the word back to the high-level representation.
		std::map<std::string, std::string> wordShadow;

		/// Compile-time-constant uint64 values for locals; used to fold memory/calldata offsets.
		/// SOUNDNESS: only single-assignment locals may be recorded (reassignedLocals gates the
		/// `let` recording); "mem_0x<off>" content keys are invalidated on any non-constant or
		/// unresolvable memory write and at control-flow boundaries (invalidateMemConstants).
		std::map<std::string, uint64_t> localConstants;

		/// Yul locals let-bound to an EIP-1967 slot constant (decimal value).
		/// Folded at every bare reference so Erc1967Lowering::classify fires on
		/// `let s := _ADMIN_SLOT; sstore(s, v)` — the OZ ERC1967Utils body shape.
		/// Same single-assignment gating as localConstants; ONLY the three 1967
		/// slots are recorded, so nothing else changes lowering. The recording
		/// let emits NO store (all references fold), so a magic constant
		/// SURVIVING in the AWST marks a genuine runtime escape
		/// (Erc1967Lowering::warnEscapedSlotConstants).
		std::map<std::string, std::string> localSlotConstants;

		/// The same values re-keyed to the MANGLED local name the AWST carries
		/// (inline-expanded frames rename), so a divisor VarExpression resolves.
		std::map<std::string, std::string> localWideConstants;

		/// Names of calldata PARAMS whose head byte-offset is stashed in localConstants (for the
		/// `.offset`/`.length` suffix + calldataMap paths). A BARE param name used as a value (e.g. as a
		/// memory offset `mstore(off, v)`) must resolve to its RUNTIME value, not that calldata-offset
		/// constant — so the bare-name constant resolvers skip these. (Solidity requires `.offset` for
		/// reference-type calldata, so no valid bare-aggregate-as-offset case exists.)
		std::set<std::string> calldataParamNames;

		/// Assembly name → uint64 offset-var name for blob-backed aggregates.
		/// A reference resolves to the memory pointer (offset), not the value.
		std::map<std::string, std::string> blobOffsetVars;

		/// Nesting depth of inlined Yul functions; >0 → `leave` emits LoopExit not Return.
		int inlineDepth = 0;

		/// Active inlined-function leave flag. Nested loops propagate this flag so
		/// leave exits the synthetic function wrapper, not merely the nearest loop.
		std::string yulLeaveFlag;

		/// Non-null only inside an outlined function; `leave` returns these values.
		solidity::yul::FunctionDefinition const* yulSubroutine = nullptr;

		/// Per-call temp names for subroutine return values (one per return value).
		/// Decoupled from the function's own return-var names so recursive calls
		/// don't clobber the current frame. Also used by the inline fallback.
		std::vector<std::string> yulSubReturnTemps;

		/// Active per-inline-call renames: a Yul user-fn's bare param/return names
		/// (x, y) → unique `__yul_<uid>_<name>`, so two functions sharing names (or
		/// nested/repeated inline calls) don't clobber each other's runtime vars.
		/// resolveVarRef applies this; the inline path saves/restores it per frame.
		std::map<std::string, std::string> yulInlineRenames;

		/// Yul locals whose bound value is provably 32-aligned (single-assignment
		/// only, same gate as localWideConstants).
		std::set<std::string> alignedLocals;

		awst::WType const* returnType = nullptr;

		std::vector<solidity::frontend::Type const*> returnSolTypes;

		/// Expression-level side effects waiting to be prepended; drained by statement handlers.
		std::vector<std::shared_ptr<awst::Statement>> pendingStatements;

		/// For-loop post body; `continue` emits it before LoopContinue (Yul semantics).
		std::vector<solidity::yul::Statement> const* forLoopPost = nullptr;
	};

	Context& prepareContext()
	{
		if (!m_preparingContext)
		{
			m_preparingContext = std::make_shared<Context>(*m_context);
			m_context = m_preparingContext;
		}
		return *m_preparingContext;
	}
	AssemblyBuilder(TypeMapper& _types, std::shared_ptr<Context const> _context)
		: m_typeMapper(_types), m_context(std::move(_context)) {}
	AssemblyBuilder(AssemblyBuilder const&) = delete;
	AssemblyBuilder& operator=(AssemblyBuilder const&) = delete;

	std::shared_ptr<Context> m_preparingContext;
	std::shared_ptr<Context const> m_context;
	Frame m_frame;

};

} // namespace puyasol::builder
