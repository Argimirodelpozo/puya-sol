/// @file DataOps.cpp
/// Data operations: calldataload, resolveConstantYulValue, keccak256.

#include "builder/yul/AssemblyBuilder.h"
#include "builder/codec/ByteSlice.h"
#include "builder/lowering/itxn/ApplicationCall.h"
#include "builder/solc/SolcFacts.h"
#include "awst/NameGen.h"
#include "Logger.h"
#include <libsolutil/Keccak256.h>

#include <sstream>
#include <limits>
// yul nodes BY VALUE (the AST aliases are std::variant, which needs
// complete types). Kept out of AssemblyBuilder.h so only the TUs that
// actually instantiate them pay the ~223k lines.
#include <libyul/AST.h>
#include <libyul/Dialect.h>
// solc AST nodes used completely (dynamic_cast / member access); the hub
// headers only forward-declare them now.
#include <libsolidity/ast/AST.h>

namespace puyasol::builder
{

std::shared_ptr<awst::Expression> AssemblyBuilder::handleCalldataload(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{

	// Once a block needs the synthetic EVM-calldata view, ALL loads in that
	// block must read that view. This includes constant offsets: a constant can
	// point into a dynamic tail, and the head word of a dynamically encoded
	// parameter is its offset rather than the parameter's decoded value. The
	// old split sent those two shapes through m_frame.calldataMap and either rejected
	// them or returned the wrong word.
	if (m_frame.useSyntheticCalldata)
	{
		return awst::makeAsBiguint(readPaddedBytes(
			awst::makeVarExpression(CD_BLOB_VAR, awst::WType::bytesType(), _loc),
			_args[0], awst::makeIntegerConstant("32", _loc), _loc), _loc);
	}

	auto offset = resolveConstantOffset(_args[0]);
	if (!offset)
	{
		Logger::instance().error(
			"calldataload with non-constant offset not supported", _loc
		);
		return nullptr;
	}

	auto it = m_frame.calldataMap.find(*offset);
	if (it != m_frame.calldataMap.end())
	{
		auto const& elem = it->second;

		auto base = awst::makeVarExpression(elem.paramName, m_frame.locals.count(elem.paramName)
			? m_frame.locals[elem.paramName]
			: awst::WType::biguintType(), _loc);

		// bytes/string: calldataload reads 32 bytes at a relative offset.
		if (elem.paramType
			&& (elem.paramType == awst::WType::bytesType()
				|| elem.paramType == awst::WType::stringType()))
		{
			// Use find() not operator[]: operator[] would insert a spurious 0 entry.
			auto lcIt = m_frame.localConstants.find(elem.paramName);
			uint64_t paramBase = lcIt != m_frame.localConstants.end() ? lcIt->second : 0;
			uint64_t relativeOffset = *offset - paramBase;

			auto offArg = awst::makeIntegerConstant(relativeOffset, _loc);

			auto lenArg = awst::makeIntegerConstant("32", _loc);

			auto extractCall = awst::makeExtract3(std::move(base), std::move(offArg), std::move(lenArg), _loc);
			auto cast = awst::makeAsBiguint(std::move(extractCall), _loc);
			return cast;
		}

		// STATIC AGGREGATE param: navigate the solc structure to the WORD-index
		// leaf and emit its EVM word (bytesN left-aligned, signed sign-extended)
		// — decoupled from ARC4-flat indexing (the bytes4[2] map bug).
		if (auto const* solT = calldataSolType(elem.paramName);
			solTypeUsable(solT)
			&& (dynamic_cast<solidity::frontend::ArrayType const*>(solT)
				|| dynamic_cast<solidity::frontend::StructType const*>(solT)))
		{
			auto [leafVal, leafSol] =
				accessEvmLeaf(std::move(base), elem.paramType, solT, elem.flatIndex, _loc);
			return awst::makeAsBiguint(
				evmCalldataWord(std::move(leafVal), leafSol, _loc), _loc);
		}

		// VALUE-TYPE param (single word): raw value, EVM-widened if the leaf
		// diverges from the zero-padded native (signed / bytesN).
		auto value = accessFlatElement(std::move(base), elem.paramType, elem.flatIndex, _loc);
		if (auto const* leaf = calldataSolLeaf(elem.paramName, elem.flatIndex);
			leafNeedsEvmWord(leaf))
			return awst::makeAsBiguint(
				evmCalldataWord(std::move(value), leaf, _loc), _loc);
		return value;
	}

	// Stubbing 0 would silently zero a real input word; hard-error instead.
	Logger::instance().error(
		"calldataload at unresolvable offset " + std::to_string(*offset) +
		" is not supported on AVM — the calldata word can't be located, so it "
		"would be stubbed as 0, silently zeroing a real input value.", _loc
	);
	auto zero = awst::makeBiguintConstant("0", _loc);
	return zero;
}

std::shared_ptr<awst::Expression> AssemblyBuilder::readPaddedBytes(
	std::shared_ptr<awst::Expression> _bytes,
	std::shared_ptr<awst::Expression> _offset,
	std::shared_ptr<awst::Expression> _length, awst::SourceLocation const& _loc)
{
	return builder::readPaddedBytes(m_typeMapper, std::move(_bytes), std::move(_offset),
		offsetToUint64(std::move(_length), _loc), _loc);
}

std::optional<std::string> AssemblyBuilder::resolveConstantYulWord(
	solidity::yul::Expression const& _expr)
{
	if (!m_context->dialect) return std::nullopt;
	return SolcFacts::yulConstantValue(_expr, *m_context->dialect,
		[this](solidity::yul::Identifier const& id) -> std::optional<std::string> {
			auto name = resolveVarRef(id);
			if (auto it = m_context->constants.find(name); it != m_context->constants.end())
				return it->second;
			if (m_frame.calldataParamNames.count(name)) return std::nullopt;
			if (auto it = m_frame.localWideConstants.find(name); it != m_frame.localWideConstants.end())
				return it->second;
			if (auto it = m_frame.localConstants.find(name); it != m_frame.localConstants.end())
				return std::to_string(it->second);
			return std::nullopt;
		});
}

std::optional<uint64_t> AssemblyBuilder::resolveConstantYulValue(
	solidity::yul::Expression const& _expr)
{
	if (auto constant = resolveConstantYulWord(_expr))
	{
		solidity::u256 value{*constant};
		if (value <= std::numeric_limits<uint64_t>::max())
			return static_cast<uint64_t>(value);
	}
	// Contents of target scratch memory are our facts, not solc constants.
	if (auto const* call = std::get_if<solidity::yul::FunctionCall>(&_expr);
		call && getFunctionName(call->functionName) == "mload" && call->arguments.size() == 1)
		if (auto offset = resolveConstantYulValue(call->arguments[0]))
		{
			std::ostringstream key;
			key << "mem_0x" << std::hex << *offset;
			if (auto it = m_frame.localConstants.find(key.str()); it != m_frame.localConstants.end())
				return it->second;
		}
	return std::nullopt;
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleKeccak256(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{

	auto length = resolveConstantOffset(_args[1]);

	auto offset = resolveConstantOffset(_args[0]);

	// COMPILE-TIME keccak over known memory content: `mstore(0, <const>);
	// keccak256(0, 0x20)` is solc's slot-derivation idiom (array data slots).
	// handleMstore records constant stores in m_frame.localConstants["mem_0x.."];
	// hash the known 32-byte word HERE (zero opcodes), preserving the full
	// derived slot without paying for a runtime hash.
	if (offset && length && *length == 32)
	{
		std::ostringstream memKey;
		memKey << "mem_0x" << std::hex << *offset;
		auto memIt = m_frame.localConstants.find(memKey.str());
		if (memIt != m_frame.localConstants.end())
		{
			solidity::bytes word(32, 0);
			uint64_t v = memIt->second;
			for (int i = 0; i < 8; ++i)
				word[31 - i] = static_cast<uint8_t>(v >> (8 * i));
			auto k = solidity::u256(solidity::util::keccak256(word));
			return awst::makeIntegerConstant(k.str(), _loc, awst::WType::biguintType());
		}
	}

	// Memory and calldata offsets have no shared provenance. Hash the actual
	// bounded range, preserving the full word until checked narrowing.
	return awst::makeAsBiguint(awst::makeKeccak256(
		readMemRangeDyn(_args[0], _args[1], _loc, m_frame.pendingStatements), _loc), _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::returndataBytes(
	awst::SourceLocation const& _loc
)
{
	return ApplicationCall::returnData(m_typeMapper, _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleReturndatasize(
	awst::SourceLocation const& _loc
)
{
	// Prefix-stripped length; returned as uint64 (consumer coerces when needed).
	return awst::makeLen(returndataBytes(_loc), _loc);
}

void AssemblyBuilder::handleLog(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	int _numTopics,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	// logN(offset, length, topic1, …, topicN) — EVM event emission. The AVM `log`
	// op has no topic structure, so flatten to ONE log:
	//   topic1 ++ … ++ topicN ++ memory[offset : offset+length]
	// Topics pass through as-is (topic0 is typically the keccak event-signature
	// hash), each padded to 32 bytes. Length must be constant (as for keccak256/
	// return); offset may be runtime. NB: this mirrors the raw `log` op used by
	// high-level `emit` — asm logN carries no event signature, so it can't route
	// through the ARC-28 Emit path (no arc56 registration → oracle won't decode it).
	auto lenConst = resolveConstantOffset(_args[1]);
	if (!lenConst)
	{
		Logger::instance().error("log" + std::to_string(_numTopics)
			+ " with a non-constant data length has no AVM translation", _loc);
		return;
	}
	if (*lenConst > 8192) // guard against readMemRangeDirect unrolling millions of words
	{
		Logger::instance().error("log" + std::to_string(_numTopics)
			+ " data length " + std::to_string(*lenConst) + " exceeds the 8192-byte cap", _loc);
		return;
	}

	// Topics first (EVM order), each 32 bytes.
	std::shared_ptr<awst::Expression> logBytes;
	for (int i = 0; i < _numTopics; ++i)
	{
		auto topic = padTo32Bytes(ensureBiguint(_args[2 + i], _loc), _loc);
		logBytes = logBytes ? awst::makeConcat(std::move(logBytes), std::move(topic), _loc)
			: std::move(topic);
	}

	// Then the data slice memory[offset : offset+length].
	if (*lenConst > 0)
	{
		auto off = awst::makeEvalOnce(offsetToUint64(_args[0], _loc), _loc);
		auto data = readMemRangeDirect(m_typeMapper, std::move(off), static_cast<int>(*lenConst), _loc);
		logBytes = logBytes ? awst::makeConcat(std::move(logBytes), std::move(data), _loc)
			: std::move(data);
	}

	if (!logBytes) // log0(_, 0): empty log
		logBytes = awst::makeBytesConstant({}, _loc);

	drainPendingStatements(_out);
	auto logCall = awst::makeIntrinsicCall("log", awst::WType::voidType(), _loc);
	logCall->stackArgs.push_back(std::move(logBytes));
	_out.push_back(awst::makeExpressionStatement(std::move(logCall), _loc));
}

void AssemblyBuilder::emitReturndatacopy(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	// Copy size bytes of returndata[offset..] into memory at destOffset.
	// extract3 reverts on OOB, matching EVM returndatacopy semantics.
	// returndataBytes strips the ARC4 return prefix (M8) so offsets index the
	// EVM-shaped payload, consistent with returndatasize().
	auto destOff = _args[0];
	auto srcOff = offsetToUint64(_args[1], _loc);
	auto size = offsetToUint64(_args[2], _loc);

	auto slice = awst::makeExtract3(returndataBytes(_loc), std::move(srcOff), std::move(size), _loc);
	// writeMemWordDyn is length-driven (replace3 writes len(slice) bytes), so it
	// handles the slot-0/slot-1+ conditional and bounds assert for the full slice.
	writeMemRangeDyn(std::move(destOff), std::move(slice), _loc, _out);
}

void AssemblyBuilder::handleRevert(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	// revert(off, len): EVM returns memory[off..off+len) as the revert data.
	// Lower as log(payload) + assert(false) — the project's revert-data
	// convention (the harness reads the failing txn's last log via simulate).
	// `revert(0, 0)` / missing args keep the bare assert (empty revert data).
	if (_args.size() == 2 && _args[0] && _args[1])
	{
		auto const* lenC = dynamic_cast<awst::IntegerConstant const*>(_args[1].get());
		bool constZeroLen = lenC && lenC->value == "0";
		// AVM caps logs at 1024 bytes — an oversize constant payload can't be
		// delivered; keep the bare assert instead of pathological codegen.
		bool constOversize = lenC && !constZeroLen
			&& (lenC->value.size() > 4 || std::stoull(lenC->value) > 1024);
		if (!constZeroLen && !constOversize)
		{

			auto payload = readMemRangeDyn(_args[0], _args[1], _loc, _out);
			auto logCall = awst::makeIntrinsicCall("log", awst::WType::voidType(), _loc);
			logCall->stackArgs.push_back(std::move(payload));
			_out.push_back(awst::makeExpressionStatement(std::move(logCall), _loc));
		}
	}
	// Non-explicit: the LOG carries the user-visible contract; the assert is
	// plumbing puya's TEAL passes may strip when unreachable (see the
	// explicit-assert accounting trap around emitArc4ReturnHalt).
	auto failAssert = awst::makeAssert(awst::makeFalse(_loc), _loc, "revert");
	failAssert->isExplicit = false;
	_out.push_back(awst::makeExpressionStatement(std::move(failAssert), _loc));
	// Mark halted: assert(false) is unconditional; trailing blob writeback
	// would be unreachable — puya's IR validator rejects it.
	m_frame.haltEmitted = true;
}



// ─── Precompile helper methods ──────────────────────────────────────────────


} // namespace puyasol::builder
