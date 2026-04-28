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
/// Memory model: EVM linear memory is simulated using AVM scratch slots 0-4.
/// Each slot holds a bytes blob of up to 4096 bytes, giving 20KB total addressable space.
/// mload/mstore translate to extract3/replace3 on the blob, supporting dynamic offsets.
/// The blob is loaded into a local variable at assembly block start and flushed at block end.
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
		std::map<std::string, std::string> const& _storageSlotVars = {}
	);

	/// Extract function name string from a Yul FunctionName variant.
	/// Works for both Identifier (user-defined) and BuiltinName (opcode) variants.
	static std::string getFunctionName(solidity::yul::FunctionName const& _name);

	// ── Memory blob constants ──────────────────────────────────────────

	/// Scratch slots reserved for EVM memory simulation.
	/// Slots 0-4, each holding up to 4096 bytes = 20KB total.
	static constexpr int MEMORY_SLOT_FIRST = 0;
	static constexpr int MEMORY_SLOT_LAST = 4;
	static constexpr int MEMORY_SLOT_COUNT = MEMORY_SLOT_LAST - MEMORY_SLOT_FIRST + 1;
	static constexpr int SLOT_SIZE = 4096;

	/// Scratch slot reserved for EIP-1153 transient storage.
	/// Holds a 4096-byte zeroed blob; persists across callsub within one app
	/// call, cleared implicitly when the next app call starts (scratch slots
	/// are per-txn), matching Solidity's per-transaction transient semantics.
	static constexpr int TRANSIENT_SLOT = 5;

	/// Get the set of scratch slots to reserve on the Contract node.
	static std::vector<int> reservedScratchSlots();

	/// Emit AWST statements that advance the EVM free-memory-pointer (FMP)
	/// stored at scratch-slot 0, offset 0x40 by `_size` bytes. Used to mirror
	/// EVM allocation semantics for `T memory t;` locals and for memory-typed
	/// return parameters, so contracts that read mload(0x40) see the expected
	/// advance. `_uniqueId` namespaces a temporary local for the blob handle.
	static std::vector<std::shared_ptr<awst::Statement>> emitFreeMemoryBump(
		int _size, awst::SourceLocation const& _loc, int _uniqueId);

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

	/// Yul sstore(slot, value): raw EVM storage write — stub as no-op.
	void handleSstore(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
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

	// ── Individual precompile handlers ─────────────────────────────────

	/// 0x01: ecRecover — ECDSA public key recovery + keccak256 → address
	void handleEcRecover(
		uint64_t _inputOffset, uint64_t _inputSize,
		uint64_t _outputOffset, uint64_t _outputSize,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);

	/// 0x02: SHA-256 hash
	void handleSha256Precompile(
		uint64_t _inputOffset, uint64_t _inputSize,
		uint64_t _outputOffset, uint64_t _outputSize,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);

	/// 0x05: ModExp — modular exponentiation via square-and-multiply loop
	void handleModExp(
		uint64_t _inputOffset, uint64_t _inputSize,
		uint64_t _outputOffset, uint64_t _outputSize,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);

	/// 0x04: Identity (memory copy)
	void handleIdentityPrecompile(
		uint64_t _inputOffset, uint64_t _inputSize,
		uint64_t _outputOffset, uint64_t _outputSize,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);

	/// 0x06: BN254 ecAdd
	void handleEcAdd(
		uint64_t _inputOffset, uint64_t _outputOffset,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);

	/// 0x07: BN254 ecMul
	void handleEcMul(
		uint64_t _inputOffset, uint64_t _outputOffset,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);

	/// 0x08: BN254 ecPairing
	void handleEcPairing(
		uint64_t _inputOffset, uint64_t _inputSize,
		uint64_t _outputOffset,
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

	/// Flush the local __evm_memory variable to scratch slot(s).
	/// Called at assembly block end and before return statements.
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

	/// Store a bytes/biguint/bool result into the memory blob at a given offset.
	void storeResultToMemory(
		std::shared_ptr<awst::Expression> _result,
		uint64_t _outputOffset, int _outputSlots,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out,
		bool _isBoolResult = false
	);

	/// Try to extract a constant integer value from a Yul expression.
	std::optional<uint64_t> resolveConstantYulValue(
		solidity::yul::Expression const& _expr
	);

	/// Lower a non-precompile Yul `call`/`staticcall` to an Algorand inner app
	/// call. The address argument is treated as an Algorand application id
	/// using puya-sol's address convention (\x00*24 ++ itob(app_id)). The
	/// calldata region is materialized as raw bytes and passed in
	/// ApplicationArgs[0]. On return, if outSize > 0, itxn LastLog is copied
	/// into the caller's memory blob at outOffset.
	void handleAppCall(
		solidity::yul::FunctionCall const& _call,
		std::string const& _assignTarget,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out,
		bool _isCall
	);

	/// Try to match mload(add(add(bytes_param, 32), offset)) pattern.
	/// Detects reads from bytes memory parameters with variable offset
	/// and translates to extract3(param, offset, 32) instead of blob access.
	std::shared_ptr<awst::Expression> tryHandleBytesMemoryRead(
		solidity::yul::Expression const& _addrExpr,
		awst::SourceLocation const& _loc
	);

	/// Try to match mload(bytes_var) pattern — Solidity's idiom for
	/// reading the 32-byte length header that precedes a bytes memory
	/// variable's payload in EVM memory. AVM has no length header, so
	/// this resolves to len(bytes_var) widened to uint256.
	std::shared_ptr<awst::Expression> tryHandleBytesMemoryLength(
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

	// ── Memory blob model ──────────────────────────────────────────────

	/// Name of the local bytes variable used as memory staging area within
	/// an assembly block. Loaded from scratch at block start, flushed at end.
	static constexpr char const* MEMORY_VAR = "__evm_memory";

	/// Initialize the memory blob: load from scratch, write params into it.
	void initializeMemoryBlob(
		std::vector<std::pair<std::string, awst::WType const*>> const& _params,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);

	/// Build an expression reading the __evm_memory local variable.
	std::shared_ptr<awst::Expression> memoryVar(awst::SourceLocation const& _loc);

	/// Assign a new value to the __evm_memory local variable.
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
