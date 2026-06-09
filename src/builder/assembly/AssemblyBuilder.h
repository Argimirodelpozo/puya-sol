#pragma once

#include "awst/Node.h"
#include "builder/sol-types/TypeMapper.h"

#include <liblangutil/EVMVersion.h>
#include <libyul/AST.h>
#include <libyul/ASTForward.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace puyasol::builder
{

// Record the compile-target EVM version so AssemblyBuilder::getFunctionName
// can resolve BuiltinHandle through the dialect that parsed the Yul AST.
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

	/// A box-keyed struct storage pointer surfaced into assembly via `.slot`
	/// (e.g. `TickInfo storage info = self.ticks[tick]; sstore(info.slot, …)`,
	/// as in Uniswap V4 Pool.updateTick). Unlike a state-var slot — a numeric
	/// EVM slot — this aliases an ARC4 struct living in a box. We carry the box
	/// key and struct type so `info.slot` resolves to that box and `sstore`
	/// performs a field-aware box write (EVM slot packing → ARC4 fields).
	struct BoxKeyedSlot
	{
		std::shared_ptr<awst::Expression> key; ///< box_key expression
		awst::WType const* structType = nullptr; ///< ARC4Struct stored in the box
	};

	/// Translate a Yul Block into AWST statements.
	/// @param _block         The Yul block to translate
	/// @param _params        Function parameters (name, type) for memory-based access
	/// @param _returnType    Expected return type of the enclosing function
	/// @param _constants     External constant values (name → decimal string)
	std::vector<std::shared_ptr<awst::Statement>> buildBlock(
		solidity::yul::Block const& _block,
		std::vector<std::pair<std::string, awst::WType const*>> const& _params,
		awst::WType const* _returnType,
		std::map<std::string, std::string> const& _constants = {},
		std::map<std::string, unsigned> const& _paramBitWidths = {},
		std::map<std::string, std::string> const& _storageSlotVars = {},
		std::map<std::string, BoxKeyedSlot> const& _boxKeyedStructSlots = {},
		std::map<std::string, std::string> const& _blobOffsetVars = {}
	);

	/// Extract function name string from a Yul FunctionName variant.
	/// Works for both Identifier (user-defined) and BuiltinName (opcode) variants.
	static std::string getFunctionName(solidity::yul::FunctionName const& _name);

	// ── Memory blob constants ──────────────────────────────────────────

	/// Scratch slots reserved for EVM memory simulation. Default 5 slots
	/// (0..4 = 20KB). Raise via `--evm-memory-slots N` (=> LAST = N-1) for
	/// memory-hungry contracts — the UltraHonk verify needs ~32 slots (128KB) for
	/// FrLib.invert's free-memory pointer (~30KB+ by shplemini). LAST is a
	/// RUNTIME static (not constexpr): the default keeps every other contract's
	/// preamble + cross-piece carry at 5 slots, so raising it for one compile is
	/// zero size-regression for the rest of the suite.
	static constexpr int MEMORY_SLOT_FIRST = 0;
	static inline int MEMORY_SLOT_LAST = 4;
	static constexpr int SLOT_SIZE = 4096;

	/// Scratch slot reserved for EIP-1153 transient storage.
	/// Holds a 4096-byte zeroed blob; persists across callsub within one app
	/// call, cleared implicitly when the next app call starts (scratch slots
	/// are per-txn), matching Solidity's per-transaction transient semantics.
	static constexpr int TRANSIENT_SLOT = 5;

	/// Scratch slots exposed to Solidity via the AVM.sol `Scratch` library
	/// (Scratch.store/loadSelf/load -> stores/loads/gloadss). Used for
	/// group-scoped flash-accounting deltas (a later group txn reads an earlier
	/// txn's slot via gload). Reserved so puya's temp allocator never reuses
	/// them, and placed ABOVE the memory/transient blobs so they don't clobber
	/// EVM memory. Scratch callers must use slots in [FIRST, LAST]; 0-5 are
	/// off-limits.
	static constexpr int FLASH_SCRATCH_FIRST = 6;
	static constexpr int FLASH_SCRATCH_LAST = 15;

	/// Get the set of scratch slots to reserve on the Contract node.
	static std::vector<int> reservedScratchSlots();

	/// Emit AWST statements that advance the EVM free-memory-pointer (FMP)
	/// stored at scratch-slot 0, offset 0x40 by `_size` bytes. Used to mirror
	/// EVM allocation semantics for `T memory t;` locals and for memory-typed
	/// return parameters, so contracts that read mload(0x40) see the expected
	/// advance. `_uniqueId` namespaces a temporary local for the blob handle.
	static std::vector<std::shared_ptr<awst::Statement>> emitFreeMemoryBump(
		int _size, awst::SourceLocation const& _loc, int _uniqueId);

	/// Read a 32-byte EVM-memory word at a DYNAMIC offset using DIRECT scratch
	/// (`extract3(loads(off / SLOT_SIZE), off % SLOT_SIZE, 32)`) — no cached
	/// __evm_memory local. For the plain-Solidity large-aggregate path, which
	/// runs outside inline-assembly blocks. STATIC so sol-ast builders can call it.
	static std::shared_ptr<awst::Expression> readMemWordDirect(
		std::shared_ptr<awst::Expression> _offset,
		awst::SourceLocation const& _loc
	);

	/// Read `_byteLen` bytes from the multi-slot blob at a DYNAMIC offset by
	/// concatenating successive 32-byte words (each slot-routed via
	/// readMemWordDirect). For materialising a small (<=SLOT_SIZE) memory
	/// aggregate VALUE (struct/static-array) out of the blob. `_byteLen` is
	/// assumed 32-aligned (ABI static aggregates); trimmed if not.
	static std::shared_ptr<awst::Expression> readMemRangeDirect(
		std::shared_ptr<awst::Expression> _offset,
		int _byteLen,
		awst::SourceLocation const& _loc
	);

	/// Write a 32-byte word (`_value32`, exactly 32 bytes) at a DYNAMIC offset
	/// using DIRECT scratch (`stores(slot, replace3(loads(slot), sub, value))`).
	static void writeMemWordDirect(
		std::shared_ptr<awst::Expression> _offset,
		std::shared_ptr<awst::Expression> _value32,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);

	/// Build a statement asserting a 32-byte memory access at `_off` (uint64)
	/// stays within the modeled scratch blob. Reverts clearly when a contract's
	/// EVM memory exceeds the allocated slots — instead of silently corrupting
	/// non-memory scratch (slot in (LAST,255]) or an opaque AVM error (slot>255).
	/// Raise `--evm-memory-slots` if a contract legitimately needs more memory.
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

	/// Lower `sstore(structRef.slot, packedWord)` where `structRef` aliases an
	/// ARC4 struct living in a box (slot 0 of the struct). EVM packs several
	/// fields into one 256-bit slot; rebuild the struct's box bytes taking the
	/// written slot's fields from `_packed` (by EVM byte range) and the rest
	/// from the existing box value, then write it back. Correct because every
	/// packed field is byte-aligned. Only slot 0 (bare `.slot`, no offset) is
	/// handled; other forms fall through to the numeric-slot path.
	void handleBoxKeyedStructSlotStore(
		std::shared_ptr<awst::BoxValueExpression> const& _slotBox,
		std::shared_ptr<awst::Expression> const& _packed,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);

	/// Build a 2^shift power expression using setbit(bzero(32), 255-shift, 1).
	std::shared_ptr<awst::Expression> buildPowerOf2(
		std::shared_ptr<awst::Expression> _shift,
		awst::SourceLocation const& _loc
	);

	/// Check if a value is "negative" in two's complement.
	/// For biguint: checks bit 255. For uint64: checks bit 63.
	/// _origType is the type before ensureBiguint conversion; nullptr defaults to biguint (256-bit).
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

	/// Handle calldataload: reads 32 bytes from calldata at a given offset.
	/// Maps to reading elements from calldata array parameters.
	std::shared_ptr<awst::Expression> handleCalldataload(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc
	);

	/// Handle keccak256(offset, length): hash memory region.
	/// Reads memory blob and applies keccak256.
	std::shared_ptr<awst::Expression> handleKeccak256(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc
	);

	/// Handle returndatasize(): returns 0 (no return data concept on AVM).
	std::shared_ptr<awst::Expression> handleReturndatasize(
		awst::SourceLocation const& _loc
	);

	/// Handle revert(offset, length): fail the transaction.
	void handleRevert(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);

	// ── Precompile dispatch ────────────────────────────────────────────

	/// Unified handler for call/staticcall to EVM precompile addresses.
	/// @param _isCall  true for `call` (7 args), false for `staticcall` (6 args)
	void handlePrecompileCall(
		solidity::yul::FunctionCall const& _call,
		std::string const& _assignTarget,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out,
		bool _isCall
	);

	/// Lowering for runtime-address `call`/`staticcall` — emits an inner
	/// app call against the address-encoded app id, splitting the EVM
	/// calldata into args[0]=selector(4B) + args[1]=body. Solady's
	/// SafeTransferLib.safeTransferFrom and similar take this path.
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

	// All other precompile handlers are runtime-offset variants below;
	// the dispatch wraps constant offsets as IntegerConstant nodes and
	// calls them. Only `handleEcRecover` keeps a constant-only path
	// because no test exercises a dynamic-offset call yet.

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

	/// Build an expression that loads the memory blob from scratch slot for
	/// a given byte offset. Returns: load(slot) where slot = offset / 4096.
	/// For constant offsets, uses immediate-arg `load`; otherwise `loads`.
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

	/// Flush the slot-0 blob to scratch. Now effectively a no-op — slot 0 is read
	/// and written directly in scratch (no __evm_memory local cache). Retained as
	/// a sync hook; emitted at assembly block end and before return statements.
	void flushMemoryToScratch(
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);

	/// Read 32 bytes from the memory blob at a constant byte offset.
	/// Returns a biguint expression.
	std::shared_ptr<awst::Expression> readMemSlot(
		uint64_t _offset,
		awst::SourceLocation const& _loc
	);

	/// Read a 32-byte EVM-memory word at a CONSTANT byte offset, routed to the
	/// scratch slot that holds it (slot = offset / SLOT_SIZE) and read directly
	/// via loads(slot) (slot 0 goes through memoryVar()). Words that straddle a
	/// slot boundary are stitched via concat. Returns bytes (32 bytes).
	std::shared_ptr<awst::Expression> readMemWordConst(
		uint64_t _offset,
		awst::SourceLocation const& _loc
	);

	/// Write a 32-byte EVM-memory word (`_value32`, exactly 32 bytes) at a
	/// CONSTANT byte offset. Each slot is load-modify-stored in scratch
	/// (stores(slot, replace3(loads(slot), sub, value))); slot 0 goes through
	/// assignMemoryVar(). Straddling words are split across the two adjacent slots.
	void writeMemWordConst(
		uint64_t _offset,
		std::shared_ptr<awst::Expression> _value32,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);

	/// Read a 32-byte EVM-memory word at a DYNAMIC (runtime) byte offset.
	/// Routes to the scratch slot via `loads(offset / SLOT_SIZE)` (the
	/// offset < SLOT_SIZE case reads slot 0); straddling words stitch two slots.
	/// Returns bytes (32 bytes).
	std::shared_ptr<awst::Expression> readMemWordDyn(
		std::shared_ptr<awst::Expression> _offset,
		awst::SourceLocation const& _loc
	);

	/// Write a 32-byte EVM-memory word (`_value32`, exactly 32 bytes) at a
	/// DYNAMIC byte offset via `stores(slot, replace3(loads(slot), sub, value))`
	/// into the scratch slot (offset / SLOT_SIZE).
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

	/// Read a contiguous region from the memory blob.
	/// Replaces the old concatSlots — now a single extract3 on the blob.
	std::shared_ptr<awst::Expression> concatSlots(
		uint64_t _baseOffset, int _startSlot, int _count,
		awst::SourceLocation const& _loc
	);

	/// Runtime-offset variant: same as concatSlots but the base offset
	/// is an Expression evaluated at runtime. Emits
	/// `extract3(__evm_memory, baseOffset + startSlot*32, count*32)`.
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

	/// Try to match mload(add(add(bytes_param, 32), offset)) pattern.
	/// Detects reads from bytes memory parameters with variable offset
	/// and translates to extract3(param, offset, 32) instead of blob access.
	std::shared_ptr<awst::Expression> tryHandleBytesMemoryRead(
		solidity::yul::Expression const& _addrExpr,
		awst::SourceLocation const& _loc
	);

	/// Try to match mstore(add(bytes_var, 32), value) pattern.
	/// Detects writes to the data region of a bytes/string memory variable
	/// and translates to a variable assignment instead of blob access.
	bool tryHandleBytesMemoryWrite(
		solidity::yul::FunctionCall const& _call,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);

	/// Try to match mcopy(add(add(bytes_var, 0x20), dstOff), add(add(bytes_var, 0x20), srcOff), len).
	/// Translates intra-buffer (and cross-buffer) bytes memory copies to
	/// replace3(dst_var, dstOff, extract3(src_var, srcOff, len)).
	/// Returns true and emits the replacement if the pattern matches; returns
	/// false to fall through to the generic mcopy handler.
	bool tryHandleBytesMemoryMcopy(
		solidity::yul::FunctionCall const& _call,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);

	// ── Memory blob model ──────────────────────────────────────────────

	/// Name of the local bytes variable used as memory staging area within
	/// an assembly block. Loaded from scratch at block start, flushed at end.
	static constexpr char const* MEMORY_VAR = "__evm_memory";

	/// Initialize the memory blob: load from scratch, write params into it.
	void initializeMemoryBlob(
		std::vector<std::pair<std::string, awst::WType const*>> const& _params,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);

	/// Build an expression reading EVM-memory slot 0 directly from scratch
	/// (`loads(MEMORY_SLOT_FIRST)`) — there is no `__evm_memory` local cache.
	std::shared_ptr<awst::Expression> memoryVar(awst::SourceLocation const& _loc);

	/// Store a new value into EVM-memory slot 0 directly in scratch
	/// (`stores(MEMORY_SLOT_FIRST, value)`).
	void assignMemoryVar(
		std::shared_ptr<awst::Expression> _value,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);

	/// Try to resolve a constant memory offset from an expression.
	/// Returns nullopt if the expression is not a compile-time constant.
	std::optional<uint64_t> resolveConstantOffset(
		std::shared_ptr<awst::Expression> const& _expr
	);

	/// Convert a biguint offset expression to uint64 for extract3/replace3.
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

	/// Maps EVM calldata byte offsets to parameter element references.
	std::map<uint64_t, CalldataElement> m_calldataMap;

	/// Initialize calldata map from function parameters.
	/// Computes the EVM calldata layout (4-byte selector + params).
	void initializeCalldataMap(
		std::vector<std::pair<std::string, awst::WType const*>> const& _params
	);

	/// Synthetic calldata-blob support — for Yul that does dynamic-offset
	/// calldataload/calldatacopy/calldatasize. We materialise a single
	/// `__cd_blob` bytes local at the start of the assembly block whose
	/// content matches Solidity's EVM-ABI calldata layout for the function
	/// (selector + head section + tail section). Then any
	/// calldataload(off) becomes `extract3(__cd_blob, off, 32)`.
	bool m_useSyntheticCalldata = false;
	std::vector<std::pair<std::string, awst::WType const*>> m_calldataParams;
	static constexpr char const* CD_BLOB_VAR = "__cd_blob";

	/// Walk the Yul block and detect any calldataload / calldatacopy /
	/// calldatasize whose offset is not a compile-time constant. Returns
	/// true iff at least one such site is found.
	bool detectDynamicCalldataAccess(solidity::yul::Block const& _block);

	/// Emit `__cd_blob = <selector ++ head ++ tail>` as AWST statements.
	/// Reads from the param locals; uses runtime concat / pad ops.
	void buildSyntheticCalldataBlob(
		std::vector<std::pair<std::string, awst::WType const*>> const& _params,
		std::vector<std::shared_ptr<awst::Statement>>& _out,
		awst::SourceLocation const& _loc
	);

	/// Compute the flat element count for an AWST type (handles nested arrays).
	static int computeFlatElementCount(awst::WType const* _type);

	/// Compute the encoded byte size of an ARC4 type.
	static int computeARC4ByteSize(awst::WType const* _type);

	/// Access a flat element from a (possibly nested) array parameter.
	std::shared_ptr<awst::Expression> accessFlatElement(
		std::shared_ptr<awst::Expression> _base,
		awst::WType const* _type,
		int _flatIndex,
		awst::SourceLocation const& _loc
	);

	// ── Variable tracking ───────────────────────────────────────────────

	/// Set when the assembly block emitted a terminating `return` or
	/// `revert` intrinsic. Tells the block-end handler to skip the
	/// trailing flushMemoryToScratch + local-upgrade coercions so puya
	/// doesn't flag them as unreachable code after the halt.
	bool m_haltEmitted = false;

	std::map<std::string, awst::WType const*> m_locals;
	/// Variables that were upgraded from uint64 to biguint within the assembly block.
	/// Maps variable name to original type so we can emit coercion back at block end.
	std::map<std::string, awst::WType const*> m_upgradedLocals;

	/// Solidity bit widths for parameters (e.g., uint16 → 16, uint32 → 32).
	/// Used to truncate assembly values back to the correct Solidity type width.
	std::map<std::string, unsigned> m_paramBitWidths;

	/// Tracks local variables with known compile-time constant uint64 values.
	/// Used to resolve dynamic memory offsets and calldata accesses.
	std::map<std::string, uint64_t> m_localConstants;

	/// External constants (Solidity constant variables referenced in assembly).
	/// Maps name -> decimal string value. Values starting with "__slot_" are
	/// storage slot references (see m_storageSlotVars).
	std::map<std::string, std::string> m_constants;

	/// Storage slot → variable name mapping for sload/sstore translation.
	/// When sstore is called with a constant whose value starts with "__slot_",
	/// the actual storage key is the variable name after the prefix.
	std::map<std::string, std::string> m_storageSlotVars;

	/// Box-keyed struct storage pointers (`info.slot` for a struct-in-box
	/// alias). Keyed on the dotted yul name ("info.slot"); the value carries
	/// the box key + struct type for the field-aware sstore lowering.
	std::map<std::string, BoxKeyedSlot> m_boxKeyedStructSlots;

	/// Assembly identifier name → blob-backed aggregate's uint64 offset-var name.
	/// A reference to such a name resolves to the memory pointer (offset), not the
	/// aggregate value. Populated by SolInlineAssembly from findBlobAggregate.
	std::map<std::string, std::string> m_blobOffsetVars;

	// ── Assembly function support ───────────────────────────────────────

	/// Collected assembly function definitions (populated during first pass).
	std::map<std::string, solidity::yul::FunctionDefinition const*> m_asmFunctions;

	/// Depth counter incremented while inlining a user-defined Yul function.
	/// When > 0, `leave` statements are translated as `LoopExit` (so they
	/// break out of the surrounding while-true loop wrapping the inlined
	/// body) instead of as a Solidity `return`, which would exit the outer
	/// function with no value and crash the puya backend.
	int m_inlineDepth = 0;

	/// Handle a call to a user-defined assembly function by inlining it.
	std::shared_ptr<awst::Expression> handleUserFunctionCall(
		std::string const& _name,
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);

	/// Names of Yul functions that directly or transitively call themselves
	/// within the current assembly block. For these we emit an AWST Subroutine
	/// instead of inlining, since inlining recursive calls recurses unboundedly
	/// at compile time.
	std::set<std::string> m_recursiveYulFuncs;

	/// Mapping from recursive Yul function name to the AWST SubroutineID used
	/// to reference its emitted Subroutine at call sites.
	std::map<std::string, std::string> m_yulFuncSubroutineIds;

	/// Build a root-level Subroutine node from a recursive Yul function and
	/// push it onto the pending sink. Only supports zero/one return values
	/// and rejects `leave` (would need return-with-value rewriting).
	void buildRecursiveYulSubroutine(
		solidity::yul::FunctionDefinition const& _funcDef,
		std::string const& _subroutineId,
		std::string const& _subroutineName
	);

public:
	/// Drain subroutines emitted for recursive Yul functions across all
	/// assembly blocks translated since the last reset.
	static std::vector<std::shared_ptr<awst::Subroutine>> takePendingSubroutines();

	/// Clear the pending-subroutines sink. Called once per contract build.
	static void resetPendingSubroutines();

private:

	// ── Utilities ───────────────────────────────────────────────────────

	awst::SourceLocation makeLoc(
		solidity::langutil::DebugData::ConstPtr const& _debugData
	);

	/// Coerce bool expressions to biguint (Yul semantics: all values are uint256).
	/// Returns the expression unchanged if it's already biguint.
	std::shared_ptr<awst::Expression> ensureBiguint(
		std::shared_ptr<awst::Expression> _expr,
		awst::SourceLocation const& _loc
	);

	/// Coerce biguint/uint64 expressions to bool (Yul semantics: non-zero = true).
	std::shared_ptr<awst::Expression> ensureBool(
		std::shared_ptr<awst::Expression> _expr,
		awst::SourceLocation const& _loc
	);

	/// Build an AWST BigUIntBinaryOperation node.
	std::shared_ptr<awst::Expression> makeBigUIntBinOp(
		std::shared_ptr<awst::Expression> _left,
		awst::BigUIntBinaryOperator _op,
		std::shared_ptr<awst::Expression> _right,
		awst::SourceLocation const& _loc
	);

	/// Create a constant 2^256 as a biguint expression.
	std::shared_ptr<awst::Expression> makeTwoPow256(awst::SourceLocation const& _loc);

	/// Wrap an expression modulo 2^256 (EVM wrapping semantics).
	std::shared_ptr<awst::Expression> wrapMod256(
		std::shared_ptr<awst::Expression> _expr,
		awst::SourceLocation const& _loc
	);

	/// EVM-safe div/mod: returns 0 when divisor is 0 (EVM semantics).
	std::shared_ptr<awst::Expression> safeDivMod(
		std::shared_ptr<awst::Expression> _left,
		awst::BigUIntBinaryOperator _op,
		std::shared_ptr<awst::Expression> _right,
		awst::SourceLocation const& _loc
	);

	/// Safe btoi: extract last 8 bytes before btoi to handle biguint > 8 bytes.
	/// AVM b&/b|/b^ pad shorter operands, producing results > 8 bytes even for
	/// small values. This pattern ensures btoi never overflows.
	std::shared_ptr<awst::Expression> safeBtoi(
		std::shared_ptr<awst::Expression> _biguintExpr,
		awst::SourceLocation const& _loc
	);

	/// Track the last mstore value for dynamic-length keccak256 patterns.
	/// When keccak256(begin, add(length, 0x20)) follows mstore(end, value),
	/// the extra 0x20 represents the appended mstore value.
	std::shared_ptr<awst::Expression> m_lastMstoreValue;

	TypeMapper& m_typeMapper;
	std::string m_sourceFile;
	std::string m_contextName;
	awst::WType const* m_returnType = nullptr;

	/// The array parameter name/type/size (for param initialization into blob).
	std::string m_arrayParamName;
	awst::WType const* m_arrayParamType = nullptr;
	int64_t m_arrayParamSize = 0;

	/// Pending statements emitted by expression-level code (e.g., inlined
	/// assembly function calls). Statement-level handlers drain these after
	/// evaluating expressions.
	std::vector<std::shared_ptr<awst::Statement>> m_pendingStatements;

	/// Current for-loop post statements — `continue` must emit these before LoopContinue.
	/// In Yul, `continue` jumps to the post expression, not the condition.
	std::vector<solidity::yul::Statement> const* m_forLoopPost = nullptr;

};

} // namespace puyasol::builder
