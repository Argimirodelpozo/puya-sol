#pragma once

#include "awst/Node.h"
#include "builder/TypeMapper.h"

#include <libyul/AST.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace puyasol::builder
{

/// Translates Yul inline assembly blocks to AWST nodes.
/// Supports the subset of Yul used by poseidon-solidity and Groth16 verifiers:
/// literals, identifiers, let/assignment, mulmod, addmod, add, mul, mod, sub,
/// mload, mstore, return, iszero, eq, lt, gt, and, or, not, shl, shr, gas,
/// staticcall (with BN256 precompile pattern matching), and user-defined
/// assembly functions.
class AssemblyTranslator
{
public:
	AssemblyTranslator(
		TypeMapper& _typeMapper,
		std::string const& _sourceFile,
		std::string const& _contextName
	);

	/// Translate a Yul Block into AWST statements.
	/// @param _block         The Yul block to translate
	/// @param _params        Function parameters (name, type) for memory-based access
	/// @param _returnType    Expected return type of the enclosing function
	/// @param _constants     External constant values (name → decimal string)
	std::vector<std::shared_ptr<awst::Statement>> translateBlock(
		solidity::yul::Block const& _block,
		std::vector<std::pair<std::string, awst::WType const*>> const& _params,
		awst::WType const* _returnType,
		std::map<std::string, std::string> const& _constants = {}
	);

private:
	// ── Expression translation ──────────────────────────────────────────

	std::shared_ptr<awst::Expression> translateExpression(
		solidity::yul::Expression const& _expr
	);
	std::shared_ptr<awst::Expression> translateFunctionCall(
		solidity::yul::FunctionCall const& _call
	);
	std::shared_ptr<awst::Expression> translateLiteral(
		solidity::yul::Literal const& _lit
	);
	std::shared_ptr<awst::Expression> translateIdentifier(
		solidity::yul::Identifier const& _id
	);

	// ── Statement translation ───────────────────────────────────────────

	void translateStatement(
		solidity::yul::Statement const& _stmt,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);
	void translateVariableDeclaration(
		solidity::yul::VariableDeclaration const& _decl,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);
	void translateAssignment(
		solidity::yul::Assignment const& _assign,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);
	void translateExpressionStatement(
		solidity::yul::ExpressionStatement const& _stmt,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);
	void translateFunctionDefinition(
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
	std::shared_ptr<awst::Expression> handleSload(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc
	);

	/// Handle calldataload: reads 32 bytes from calldata at a given offset.
	/// Maps to reading elements from calldata array parameters.
	std::shared_ptr<awst::Expression> handleCalldataload(
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc
	);

	/// Handle keccak256(offset, length): hash memory region.
	/// Reads memory slots and applies keccak256.
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

	// ── Precompile helper methods ──────────────────────────────────────

	/// Read a biguint value from a memory slot, or return zero if not found.
	std::shared_ptr<awst::Expression> readMemSlot(
		uint64_t _offset,
		awst::SourceLocation const& _loc
	);

	/// Pad a biguint expression to exactly 32 zero-padded big-endian bytes.
	std::shared_ptr<awst::Expression> padTo32Bytes(
		std::shared_ptr<awst::Expression> _expr,
		awst::SourceLocation const& _loc
	);

	/// Concatenate N consecutive 32-byte-padded memory slots into one bytes value.
	std::shared_ptr<awst::Expression> concatSlots(
		uint64_t _baseOffset, int _startSlot, int _count,
		awst::SourceLocation const& _loc
	);

	/// Look up or create a memory variable name for an offset.
	std::string getOrCreateMemoryVar(
		uint64_t _offset,
		awst::SourceLocation const& _loc
	);

	/// Store a bytes/biguint/bool result into output memory slots.
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

	/// Try to match mload(add(add(bytes_param, 32), offset)) pattern.
	/// Detects reads from bytes memory parameters with variable offset
	/// and translates to extract3(param, offset, 32) instead of returning 0.
	std::shared_ptr<awst::Expression> tryHandleBytesMemoryRead(
		solidity::yul::Expression const& _addrExpr,
		awst::SourceLocation const& _loc
	);

	// ── Memory model ────────────────────────────────────────────────────

	struct MemorySlot
	{
		std::string varName;
		bool isParam = false;
		int paramIndex = -1;
	};

	/// Maps memory offsets (0x00, 0x20, 0x80, 0xa0, ...) to variables.
	std::map<uint64_t, MemorySlot> m_memoryMap;

	/// Initialize memory map from function parameters.
	/// Array parameter elements are mapped at 0x80 + i*0x20.
	void initializeMemoryMap(
		std::vector<std::pair<std::string, awst::WType const*>> const& _params
	);

	/// Try to resolve a constant memory offset from an expression.
	/// Returns nullopt if the expression is not a compile-time constant.
	std::optional<uint64_t> resolveConstantOffset(
		std::shared_ptr<awst::Expression> const& _expr
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

	/// Access a flat element from a (possibly nested) array parameter.
	std::shared_ptr<awst::Expression> accessFlatElement(
		std::shared_ptr<awst::Expression> _base,
		awst::WType const* _type,
		int _flatIndex,
		awst::SourceLocation const& _loc
	);

	// ── Variable tracking ───────────────────────────────────────────────

	std::map<std::string, awst::WType const*> m_locals;

	/// Tracks local variables with known compile-time constant uint64 values.
	/// Used to resolve dynamic memory offsets and calldata accesses.
	std::map<std::string, uint64_t> m_localConstants;

	/// External constants (Solidity constant variables referenced in assembly).
	/// Maps name -> decimal string value.
	std::map<std::string, std::string> m_constants;

	// ── Assembly function support ───────────────────────────────────────

	/// Collected assembly function definitions (populated during first pass).
	std::map<std::string, solidity::yul::FunctionDefinition const*> m_asmFunctions;

	/// Handle a call to a user-defined assembly function by inlining it.
	std::shared_ptr<awst::Expression> handleUserFunctionCall(
		std::string const& _name,
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out
	);

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

	/// Build an AWST BigUIntBinaryOperation node.
	std::shared_ptr<awst::Expression> makeBigUIntBinOp(
		std::shared_ptr<awst::Expression> _left,
		awst::BigUIntBinaryOperator _op,
		std::shared_ptr<awst::Expression> _right,
		awst::SourceLocation const& _loc
	);

	TypeMapper& m_typeMapper;
	std::string m_sourceFile;
	std::string m_contextName;
	awst::WType const* m_returnType = nullptr;

	/// The array parameter expression (for mload-based access).
	std::string m_arrayParamName;
	awst::WType const* m_arrayParamType = nullptr;
	int m_arrayParamSize = 0;

	/// Owns dynamically-created WType objects (e.g. WTuple for ecdsa_pk_recover).
	std::vector<std::unique_ptr<awst::WType>> m_ownedTypes;
};

} // namespace puyasol::builder
