#pragma once

#include "awst/Node.h"
#include "builder/sol-types/TypeMapper.h"

#include <liblangutil/EVMVersion.h>
#include <libyul/AST.h>
#include <libyul/ASTForward.h>
#include <libsolidity/ast/ASTAnnotations.h>

#include <functional>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace puyasol::builder
{

// Set the compile-target EVM version for getFunctionName's BuiltinHandle resolution.
// Called once at startup from main.cpp after compiler.setEVMVersion.
void setCompileEVMVersion(solidity::langutil::EVMVersion _v);


/// Builds AWST nodes from Yul inline assembly blocks.
///
/// Translates EVM Yul opcodes to equivalent AVM operations using biguint arithmetic
/// (EVM uint256 ↔ AVM biguint), a scratch-slot-backed memory blob, and calldata mapping.
///
/// Memory model: EVM linear memory is simulated using AVM scratch slots
/// MEMORY_SLOT_FIRST..MEMORY_SLOT_LAST (default 0-4 = 20KB; raise with
/// --evm-memory-slots N). Each slot holds a bytes blob of up to 4096 bytes.
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
///   - MemoryHelpers.cpp        — readMemSlot, padTo32Bytes, concatSlots, storeResultToMemory
///   - MemoryOps.cpp            — mload, mstore, handleReturn, tryHandleBytesMemoryRead
///   - PrecompileDispatch.cpp   — Routes call/staticcall to specific precompile handlers
///   - PrecompileHandlers.cpp   — ecAdd, ecMul, ecPairing, ecRecover, sha256, modExp, identity
///   - StatementOps.cpp         — Yul statement translation: let, :=, expression stmts, functions
class AssemblyBuilder
{
public:
	AssemblyBuilder(
		TypeMapper& _typeMapper,
		std::string const& _sourceFile,
		std::string const& _contextName
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
		enum class Kind { Scalar, ArrayRoot, ArrayData };
		Kind kind = Kind::Scalar;
		std::string varName;
		awst::WType const* wtype = nullptr;   ///< Scalar: the var's wtype
		std::string dataBase;                 ///< ArrayData: decimal K (region base)
	};

	/// Exact-slot routes (decimal slot string → route) + data regions
	/// ([K, K+2^32) element windows). See SlotRoute.
	void setSlotRoutes(
		std::map<std::string, SlotRoute> _exact, std::vector<SlotRoute> _regions)
	{
		m_slotRoutes = std::move(_exact);
		m_slotDataRegions = std::move(_regions);
	}

	/// True when the block emitted an unconditional halt at top level
	/// (branch-local halts not counted — translateSwitch/If save+restore the flag).
	bool haltEmitted() const { return m_haltEmitted; }
	/// When true, EVM `return(o,s)` lowers as a program halt (internal/private frame).
	/// For public/external functions it lowers as a subroutine return (caller continues).
	void setFrameIsProgram(bool _v) { m_frameIsProgram = _v; }

	std::vector<std::shared_ptr<awst::Statement>> buildBlock(
		solidity::yul::Block const& _block,
		std::vector<std::pair<std::string, awst::WType const*>> const& _params,
		awst::WType const* _returnType,
		std::map<std::string, std::string> const& _constants = {},
		std::map<std::string, unsigned> const& _paramBitWidths = {},
		std::map<std::string, std::string> const& _storageSlotVars = {},
		std::map<std::string, BoxKeyedSlot> const& _boxKeyedStructSlots = {},
		std::map<std::string, std::string> const& _blobOffsetVars = {},
		std::map<std::string, std::string> const& _structRefSlotLocals = {},
		std::map<std::string, StateVarSlot> const& _stateVarSlots = {},
		std::map<solidity::yul::Identifier const*,
			solidity::frontend::InlineAssemblyAnnotation::ExternalIdentifierInfo> const& _externalRefs = {},
		std::function<std::string(solidity::frontend::VariableDeclaration const&)> _declName = {},
		/// Number of leading _params that are the function's real CALLDATA args (the rest are
		/// external refs / return vars appended by SolInlineAssembly). The synthetic calldata
		/// blob + offset map are built from ONLY these — using the full augmented list inflates
		/// the EVM-ABI head and breaks .offset/.length. Default = all (back-compat).
		size_t _numCalldataParams = ~size_t(0)
	);

	/// Extract function name from a Yul FunctionName (Identifier or BuiltinName).
	static std::string getFunctionName(solidity::yul::FunctionName const& _name);

	/// AWST name for a Yul external ref: locals + fn-ptr .selector/.address → mangled via
	/// _declName; state vars/constants/.slot/.offset/.length → bare Yul name.
	/// Shared by resolveVarRef and SolInlineAssembly augmentedParams keying.
	static std::string externalRefAwstName(
		solidity::frontend::InlineAssemblyAnnotation::ExternalIdentifierInfo const& _info,
		std::string const& _bareName,
		std::function<std::string(solidity::frontend::VariableDeclaration const&)> const& _declName);

	// ── Memory blob constants ──────────────────────────────────────────

	/// Scratch slots for EVM memory. Default 5 (0..4 = 20KB).
	/// Raise via `--evm-memory-slots N` for memory-hungry contracts
	/// (UltraHonk verify needs ~32 slots / 128KB for FrLib.invert / shplemini).
	/// LAST is a runtime static so raising it for one compile doesn't bloat others.
	static constexpr int MEMORY_SLOT_FIRST = 0;
	static inline int MEMORY_SLOT_LAST = 4;
	static constexpr int SLOT_SIZE = 4096;

	/// Scratch slot for EIP-1153 transient storage. 4096-byte zeroed blob; persists across
	/// callsub within one app call; cleared per-txn (matches Solidity transient semantics).
	static constexpr int TRANSIENT_SLOT = 5;

	/// Scratch slots for the AVM.sol `Scratch` library (flash-accounting deltas; later
	/// group txns read them via gload). Reserved so puya's allocator never reuses them.
	/// Placed above memory/transient blobs; callers must use [FIRST, LAST] (0–5 off-limits).
	static constexpr int FLASH_SCRATCH_FIRST = 6;
	static constexpr int FLASH_SCRATCH_LAST = 15;

	/// Get the set of scratch slots to reserve on the Contract node.
	static std::vector<int> reservedScratchSlots();

	/// Share the enclosing FUNCTION's seeded-calldata-pointer set across this
	/// function's per-block AssemblyBuilders (each block constructs a fresh
	/// builder). initCalldataPointerLocals seeds each dynamic calldata param's
	/// (__cd_off_x, __cd_len_x) locals only if the param is not yet in the set —
	/// so a pointer write from an earlier block survives into later blocks.
	/// Mirrors setFrameIsProgram. Nullable (freestanding uses seed every block).
	void setSeededCalldataPointers(std::set<std::string>* _seeded)
	{
		m_seededCalldataPointers = _seeded;
	}


	/// Base names of dynamic-CALLDATA pointer vars referenced by this block
	/// (from the refs' declarations — covers calldata return vars / locals whose
	/// suffixed refs register under the dotted name so m_locals misses the base).
	void setCalldataPointerNames(std::set<std::string> _names)
	{
		m_calldataPointerNames = std::move(_names);
	}

	/// STATIC calldata pointers (structs / fixed arrays) referenced by this block:
	/// their bare Yul name reads/writes __cd_off_<name> (the byte offset of their
	/// data in __cd_blob) — `s := s2` / `s := 0x24` / `s := t` semantics.
	void setCalldataStaticPtrNames(std::set<std::string> _names)
	{
		m_calldataStaticPtrNames = std::move(_names);
	}

	/// Advance the FMP (scratch-slot 0, offset 0x40) by `_size` bytes.
	/// Mirrors EVM allocation semantics for `T memory t;` locals so mload(0x40) is correct.
	/// `_uniqueId` namespaces the temporary blob-handle local.
	static std::vector<std::shared_ptr<awst::Statement>> emitFreeMemoryBump(
		int _size, awst::SourceLocation const& _loc, int _uniqueId);

	/// Read a 32-byte EVM-memory word at a DYNAMIC offset via direct scratch
	/// (`extract3(loads(off/SLOT_SIZE), off%SLOT_SIZE, 32)`). Static so sol-ast can call it.
	static std::shared_ptr<awst::Expression> readMemWordDirect(
		std::shared_ptr<awst::Expression> _offset,
		awst::SourceLocation const& _loc
	);

	/// Read `_byteLen` bytes at a DYNAMIC offset by concatenating successive 32-byte words
	/// (slot-routed via readMemWordDirect). For materialising a small (<=SLOT_SIZE)
	/// aggregate value from the blob. `_byteLen` assumed 32-aligned; trimmed if not.
	static std::shared_ptr<awst::Expression> readMemRangeDirect(
		std::shared_ptr<awst::Expression> _offset,
		int _byteLen,
		awst::SourceLocation const& _loc
	);

	/// Write a 32-byte word at a DYNAMIC offset via direct scratch
	/// (`stores(slot, replace3(loads(slot), sub, value))`).
	static void writeMemWordDirect(
		std::shared_ptr<awst::Expression> _offset,
		std::shared_ptr<awst::Expression> _value32,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);

	/// Assert a 32-byte access at `_off` stays within the modeled blob. Reverts clearly
	/// on overflow rather than silently corrupting non-memory scratch or hitting an
	/// opaque AVM error (slot>255). Raise `--evm-memory-slots` if more memory is needed.
	static std::shared_ptr<awst::Statement> memBoundsAssert(
		std::shared_ptr<awst::Expression> _off,
		awst::SourceLocation const& _loc
	);

private:
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

	/// Yul tload(slot): load from transient storage → global state read.
	std::shared_ptr<awst::Expression> handleTload(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc
	);

	/// Yul tstore(slot, value): store to transient storage → global state write.
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
	/// (by EVM byte range) and the existing box for other fields, then writes back.
	/// Only slot 0 (bare `.slot`) handled; others fall through to the numeric-slot path.
	void handleBoxKeyedStructSlotStore(
		std::shared_ptr<awst::BoxValueExpression> const& _slotBox,
		std::shared_ptr<awst::Expression> const& _packed,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
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
		awst::SourceLocation const& _loc,
		awst::WType const* _origType = nullptr
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

	/// EVM returndatasize() → len(itxn LastLog) as uint64.
	std::shared_ptr<awst::Expression> handleReturndatasize(
		awst::SourceLocation const& _loc
	);

	/// EVM returndatacopy: copy `size` bytes of itxn LastLog from `offset` to `destOffset`.
	void emitReturndatacopy(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
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

	/// Runtime-address call/staticcall → inner app call (address-encoded app id);
	/// splits EVM calldata into args[0]=selector(4B) + args[1]=body.
	/// Solady's SafeTransferLib.safeTransferFrom and similar take this path.
	void handleAppCall(
		solidity::yul::FunctionCall const& _call,
		std::string const& _assignTarget,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out,
		bool _isCall
	);

	// ── Individual precompile handlers ─────────────────────────────────

	/// 0x01: ecRecover — ECDSA public key recovery + keccak256 → address
	void handleEcRecover(
		uint64_t _inputOffset, uint64_t _inputSize,
		uint64_t _outputOffset, uint64_t _outputSize,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);

	// All other precompiles use runtime-offset variants; dispatch wraps constants as IntegerConstant.
	// handleEcRecover keeps its constant-only path (no dynamic-offset test exists).

	/// 0x02: SHA-256 hash
	void handleEcAddRT(
		std::shared_ptr<awst::Expression> _inputOffset,
		std::shared_ptr<awst::Expression> _outputOffset,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);
	void handleEcMulRT(
		std::shared_ptr<awst::Expression> _inputOffset,
		std::shared_ptr<awst::Expression> _outputOffset,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);
	void handleEcPairingRT(
		std::shared_ptr<awst::Expression> _inputOffset,
		std::shared_ptr<awst::Expression> _inputSize,
		std::shared_ptr<awst::Expression> _outputOffset,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);
	void handleSha256PrecompileRT(
		std::shared_ptr<awst::Expression> _inputOffset,
		std::shared_ptr<awst::Expression> _inputSize,
		std::shared_ptr<awst::Expression> _outputOffset,
		std::shared_ptr<awst::Expression> _outputSize,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);
	void handleIdentityPrecompileRT(
		std::shared_ptr<awst::Expression> _inputOffset,
		std::shared_ptr<awst::Expression> _inputSize,
		std::shared_ptr<awst::Expression> _outputOffset,
		std::shared_ptr<awst::Expression> _outputSize,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);
	void handleModExpRT(
		std::shared_ptr<awst::Expression> _inputOffset,
		std::shared_ptr<awst::Expression> _inputSize,
		std::shared_ptr<awst::Expression> _outputOffset,
		std::shared_ptr<awst::Expression> _outputSize,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
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

	/// No-op now (slot 0 in direct scratch); retained as a splitter sync hook.
	void flushMemoryToScratch(
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);

	/// Read 32 bytes from the blob at a constant offset → biguint.
	std::shared_ptr<awst::Expression> readMemSlot(
		uint64_t _offset,
		awst::SourceLocation const& _loc
	);

	/// Read a 32-byte word at a CONSTANT offset (slot = offset/SLOT_SIZE); slot 0 via
	/// memoryVar(), others via loads(slot). Straddles stitched via concat. Returns bytes.
	std::shared_ptr<awst::Expression> readMemWordConst(
		uint64_t _offset,
		awst::SourceLocation const& _loc
	);

	/// Write a 32-byte word at a CONSTANT offset; slot 0 via assignMemoryVar(),
	/// others load-modify-stored in scratch. Straddles split across adjacent slots.
	void writeMemWordConst(
		uint64_t _offset,
		std::shared_ptr<awst::Expression> _value32,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);

	/// Read a 32-byte word at a DYNAMIC offset; offset < SLOT_SIZE reads slot 0,
	/// otherwise loads(offset/SLOT_SIZE). Straddles stitch two slots. Returns bytes.
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

	/// Pad a biguint expression to exactly 32 zero-padded big-endian bytes.
	std::shared_ptr<awst::Expression> padTo32Bytes(
		std::shared_ptr<awst::Expression> _expr,
		awst::SourceLocation const& _loc
	);

	/// Read a contiguous region from the blob (single extract3).
	std::shared_ptr<awst::Expression> concatSlots(
		uint64_t _baseOffset, int _startSlot, int _count,
		awst::SourceLocation const& _loc
	);

	/// Runtime-offset variant of concatSlots (base offset is an Expression).
	std::shared_ptr<awst::Expression> concatSlotsRT(
		std::shared_ptr<awst::Expression> _baseOffset, int _startSlot, int _count,
		awst::SourceLocation const& _loc
	);

	/// Store a bytes/biguint/bool result into the memory blob at a given offset.
	void storeResultToMemory(
		std::shared_ptr<awst::Expression> _result,
		uint64_t _outputOffset, int _outputSlots,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out,
		bool _isBoolResult = false
	);

	/// Runtime-offset variant of storeResultToMemory.
	void storeResultToMemoryRT(
		std::shared_ptr<awst::Expression> _result,
		std::shared_ptr<awst::Expression> _outputOffset, int _outputSlots,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out,
		bool _isBoolResult = false
	);

	/// Try to extract a constant integer value from a Yul expression.
	std::optional<uint64_t> resolveConstantYulValue(
		solidity::yul::Expression const& _expr
	);

	/// Match mload(add(add(bytes_param, 32), offset)) → extract3(param, offset, 32).
	std::shared_ptr<awst::Expression> tryHandleBytesMemoryRead(
		solidity::yul::Expression const& _addrExpr,
		awst::SourceLocation const& _loc
	);

	/// Match mstore(add(bytes_var, 32), value) → variable assignment (no blob write).
	bool tryHandleBytesMemoryWrite(
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

	/// Vestigial local name (slot 0 now lives directly in scratch).
	static constexpr char const* MEMORY_VAR = "__evm_memory";

	void initializeMemoryBlob(
		std::vector<std::pair<std::string, awst::WType const*>> const& _params,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);

	/// Read EVM-memory slot 0 directly from scratch (loads(MEMORY_SLOT_FIRST)).
	std::shared_ptr<awst::Expression> memoryVar(awst::SourceLocation const& _loc);

	/// Write EVM-memory slot 0 directly to scratch (stores(MEMORY_SLOT_FIRST, value)).
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

	std::map<uint64_t, CalldataElement> m_calldataMap;

	void initializeCalldataMap(
		std::vector<std::pair<std::string, awst::WType const*>> const& _params
	);

	/// True when dynamic calldataload/copy/size detected; materialise __cd_blob.
	bool m_useSyntheticCalldata = false;
	std::set<std::string>* m_seededCalldataPointers = nullptr;
	std::set<std::string> m_calldataPointerNames;
	std::set<std::string> m_calldataStaticPtrNames;
	std::vector<std::pair<std::string, awst::WType const*>> m_calldataParams;
	static constexpr char const* CD_BLOB_VAR = "__cd_blob";

	/// True iff any calldataload/copy/size in the block has a non-constant offset.
	bool detectDynamicCalldataAccess(solidity::yul::Block const& _block);

	/// Emit `__cd_blob = selector ++ head ++ tail` from param locals.
	void buildSyntheticCalldataBlob(
		std::vector<std::pair<std::string, awst::WType const*>> const& _params,
		std::vector<std::shared_ptr<awst::Statement>>& _out,
		awst::SourceLocation const& _loc
	);

	/// True for a dynamic-ABI calldata param (bytes/string/dynamic array): its head in the
	/// EVM-ABI blob is a tail POINTER, so .offset/.length must be read at runtime from __cd_blob.
	bool isDynamicCalldataType(awst::WType const* _type) const;

	/// Runtime .offset / .length of a dynamic calldata param, read byte-addressed from __cd_blob:
	/// headWord = u64@(headPos); .offset = headWord + 36 (4 selector + 32 length word);
	/// .length = u64@(headWord + 4). _headPos = the param's head byte position (m_localConstants).
	std::shared_ptr<awst::Expression> calldataDynOffset(uint64_t _headPos, awst::SourceLocation const& _loc);
	std::shared_ptr<awst::Expression> calldataDynLength(uint64_t _headPos, awst::SourceLocation const& _loc);

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

	/// True after a halt (return/revert): skip trailing flush + coercions (else puya: unreachable).
	bool m_haltEmitted = false;
	bool m_frameIsProgram = false;

	std::map<std::string, awst::WType const*> m_locals;
	/// Locals upgraded uint64→biguint; maps name to original type for block-end coercion.
	std::map<std::string, awst::WType const*> m_upgradedLocals;

	/// Solidity param bit-widths (uint16→16); used to truncate values on block exit.
	std::map<std::string, unsigned> m_paramBitWidths;

	/// Compile-time-constant uint64 values for locals; used to fold memory/calldata offsets.
	std::map<std::string, uint64_t> m_localConstants;

	/// Names of calldata PARAMS whose head byte-offset is stashed in m_localConstants (for the
	/// `.offset`/`.length` suffix + calldataMap paths). A BARE param name used as a value (e.g. as a
	/// memory offset `mstore(off, v)`) must resolve to its RUNTIME value, not that calldata-offset
	/// constant — so the bare-name constant resolvers skip these. (Solidity requires `.offset` for
	/// reference-type calldata, so no valid bare-aggregate-as-offset case exists.)
	std::set<std::string> m_calldataParamNames;

	/// Solidity `constant` vars referenced in assembly: name → decimal string.
	/// "__slot_"-prefixed values are storage-slot refs (see m_storageSlotVars).
	std::map<std::string, std::string> m_constants;

	/// "__slot_<varName>" → varName; drives sload/sstore storage translation.
	std::map<std::string, std::string> m_storageSlotVars;

	/// Dotted Yul name ("info.slot") → BoxKeyedSlot for box-struct sstore lowering.
	std::map<std::string, BoxKeyedSlot> m_boxKeyedStructSlots;

	/// Dotted yul name (`v.slot`) → scalar app-global state var, so sstore routes to
	/// the var's own app-global storage (not __dyn_storage). Populated by SolInlineAssembly.
	std::map<std::string, StateVarSlot> m_stateVarSlots;
	std::map<std::string, SlotRoute> m_slotRoutes;
	std::vector<SlotRoute> m_slotDataRegions;

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

	/// Assembly name → uint64 offset-var name for blob-backed aggregates.
	/// A reference resolves to the memory pointer (offset), not the value.
	std::map<std::string, std::string> m_blobOffsetVars;

	/// Dotted yul name (`ptr.slot`) → mangled biguint local holding a storage-ref
	/// slot handle. Lets `.slot` on a struct-storage-ref local resolve to the
	/// handle value instead of the (non-scalar) struct. Populated by SolInlineAssembly.
	std::map<std::string, std::string> m_structRefSlotLocals;

	/// solc's external refs for the current block (yul id ptr → {decl, suffix}).
	/// Pointer-keyed so a Yul-local shadowing an outer var isn't mis-resolved.
	std::map<solidity::yul::Identifier const*,
		solidity::frontend::InlineAssemblyAnnotation::ExternalIdentifierInfo> m_externalRefs;
	/// Resolves a VariableDeclaration to its AWST name (Context::awstVarName).
	std::function<std::string(solidity::frontend::VariableDeclaration const&)> m_declName;

	// ── Assembly function support ───────────────────────────────────────

	/// Collected assembly function definitions (populated during first pass).
	std::map<std::string, solidity::yul::FunctionDefinition const*> m_asmFunctions;

	/// Nesting depth of inlined Yul functions; >0 → `leave` emits LoopExit not Return.
	int m_inlineDepth = 0;

	/// Handle a call to a user-defined assembly function by inlining it.
	std::shared_ptr<awst::Expression> handleUserFunctionCall(
		std::string const& _name,
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);

	/// Recursive Yul functions (direct or transitive self-calls); emitted as Subroutines,
	/// not inlined, to avoid unbounded C++ compile-time recursion.
	std::set<std::string> m_recursiveYulFuncs;

	/// Recursive Yul function name → AWST SubroutineID for call-site references.
	std::map<std::string, std::string> m_yulFuncSubroutineIds;

	/// Per-call temp names for subroutine return values (one per return value).
	/// Decoupled from the function's own return-var names so recursive calls
	/// don't clobber the current frame. Empty when the last call was inlined.
	std::vector<std::string> m_yulSubReturnTemps;

	/// Active per-inline-call renames: a Yul user-fn's bare param/return names
	/// (x, y) → unique `__yul_<uid>_<name>`, so two functions sharing names (or
	/// nested/repeated inline calls) don't clobber each other's runtime vars.
	/// resolveVarRef applies this; the inline path saves/restores it per frame.
	std::map<std::string, std::string> m_yulInlineRenames;

	/// Emit a Subroutine for a recursive Yul function; push to pending sink.
	/// Supports 0/1 return values; rejects `leave`.
	void buildRecursiveYulSubroutine(
		solidity::yul::FunctionDefinition const& _funcDef,
		std::string const& _subroutineId,
		std::string const& _subroutineName
	);

public:
	/// Take all Subroutines emitted for recursive Yul functions since last reset.
	static std::vector<std::shared_ptr<awst::Subroutine>> takePendingSubroutines();
	/// Clear the pending-subroutines sink (once per contract build).
	static void resetPendingSubroutines();

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

	/// div/mod returning 0 for divisor=0 (EVM semantics; AVM would panic).
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

	/// Last mstore value; used by keccak256(begin, add(len,0x20)) pattern to append it.
	std::shared_ptr<awst::Expression> m_lastMstoreValue;

	TypeMapper& m_typeMapper;
	std::string m_sourceFile;
	std::string m_contextName;
	awst::WType const* m_returnType = nullptr;

	std::string m_arrayParamName;
	awst::WType const* m_arrayParamType = nullptr;
	int64_t m_arrayParamSize = 0;

	/// Expression-level side effects waiting to be prepended; drained by statement handlers.
	std::vector<std::shared_ptr<awst::Statement>> m_pendingStatements;

	/// For-loop post body; `continue` emits it before LoopContinue (Yul semantics).
	std::vector<solidity::yul::Statement> const* m_forLoopPost = nullptr;

};

} // namespace puyasol::builder
