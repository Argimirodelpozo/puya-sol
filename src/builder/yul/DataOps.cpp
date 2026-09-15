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
	if (!checkArity(_args, 1, "calldataload", _loc))
		return nullptr;

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
	if (!checkArity(_args, 2, "keccak256", _loc, "offset, length"))
		return nullptr;

	auto length = resolveConstantOffset(_args[1]);

	// Check for WTuple FIRST: initializeCalldataMap stores calldata offsets in
	// m_frame.localConstants, causing struct params to resolve as false-positive constants.
	auto const* varExprForTuple = dynamic_cast<awst::VarExpression const*>(_args[0].get());
	if (varExprForTuple && length)
	{
		auto it = m_frame.locals.find(varExprForTuple->name);
		if (it != m_frame.locals.end() && it->second && it->second->kind() == awst::WTypeKind::WTuple)
		{
			auto const* tupleType = dynamic_cast<awst::WTuple const*>(it->second);
			if (tupleType)
			{
				int numFields = static_cast<int>(tupleType->types().size());
				int expectedLen = numFields * 32;
				if (static_cast<int>(*length) == expectedLen)
				{
					// Concatenate all struct fields, each padded to 32 bytes
					std::shared_ptr<awst::Expression> data;
					for (int i = 0; i < numFields; ++i)
					{
						auto field = awst::makeTupleItem(_args[0], i, tupleType->types()[static_cast<size_t>(i)], _loc);

						auto padded = padTo32Bytes(std::move(field), _loc);

						if (!data)
							data = std::move(padded);
						else
							data = awst::makeConcat(std::move(data), std::move(padded), _loc);
					}

					auto keccak = awst::makeKeccak256(std::move(data), _loc);
					return awst::makeAsBiguint(std::move(keccak), _loc);
				}
			}
		}
	}

	auto offset = resolveConstantOffset(_args[0]);

	// COMPILE-TIME keccak over known memory content: `mstore(0, <const>);
	// keccak256(0, 0x20)` is solc's slot-derivation idiom (array data slots).
	// handleMstore records constant stores in m_frame.localConstants["mem_0x.."];
	// hash the known 32-byte word HERE (zero opcodes) so the derived slot
	// becomes a constant the SlotRoute machinery routes — never a runtime
	// keccak for storage routing (project hashing policy).
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

	if (!offset && length)
		return awst::makeAsBiguint(awst::makeKeccak256(
			readMemRangeDyn(_args[0], _args[1], _loc, m_frame.pendingStatements), _loc), _loc);

	if (offset && !length)
	{
		// Constant offset, dynamic length.
		// Pattern: keccak256(begin, add(paramLen, 0x20)) from deriveMapping(string/bytes).
		// Hashes param_bytes ++ padTo32(last mstored value).
		for (auto const& [cdOffset, elem] : m_frame.calldataMap)
		{
			if (*offset == cdOffset + 0x20 && m_frame.lastMstoreValue)
			{
				auto paramType = m_frame.locals.find(elem.paramName);
				auto const* paramWtype = (paramType != m_frame.locals.end() && paramType->second)
					? paramType->second : awst::WType::bytesType();
				auto paramVar = awst::makeVarExpression(elem.paramName, paramWtype, _loc);

				std::shared_ptr<awst::Expression> paramBytes;
				if (paramVar->wtype != awst::WType::bytesType())
				{
					auto cast = awst::makeAsBytes(std::move(paramVar), _loc);
					paramBytes = std::move(cast);
				}
				else
					paramBytes = std::move(paramVar);

				auto slotPadded = padTo32Bytes(m_frame.lastMstoreValue, _loc);

				auto concat = awst::makeConcat(std::move(paramBytes), std::move(slotPadded), _loc);
				auto keccak = awst::makeKeccak256(std::move(concat), _loc);
				return awst::makeAsBiguint(std::move(keccak), _loc);
			}
		}
	}

	if (!offset || !length)
	{
		// The shared reader owns page stitching, capacity and zero-length
		// semantics, including an unused offset wider than uint64.
		return awst::makeAsBiguint(awst::makeKeccak256(
			readMemRangeDyn(_args[0], _args[1], _loc, m_frame.pendingStatements), _loc), _loc);
	}

	int numSlots = static_cast<int>(*length / 0x20);
	if (*length == 0)
	{
		auto emptyBytes = awst::makeBytesConstant({}, _loc, awst::BytesEncoding::Unknown);
		auto keccak = awst::makeKeccak256(std::move(emptyBytes), _loc);
		return awst::makeAsBiguint(std::move(keccak), _loc);
	}

	// If offset falls in m_frame.calldataMap (e.g. Yul optimizer elided abi_encode buffer copy
	// for a struct param like PoolKey), extract fields and pad each to 32 bytes.
	auto firstSlotIt = m_frame.calldataMap.find(*offset);
	if (firstSlotIt != m_frame.calldataMap.end())
	{
		auto const& elem = firstSlotIt->second;
		auto const* structType = dynamic_cast<awst::ARC4Struct const*>(elem.paramType);
		// Whole-word lengths only: the per-field 32-byte padding below assumes
		// a word-aligned buffer shape.
		if (structType && *length % 0x20 == 0
			&& numSlots == static_cast<int>(structType->fields().size()))
		{
			auto base = awst::makeVarExpression(elem.paramName, m_frame.locals.count(elem.paramName)
				? m_frame.locals[elem.paramName] : elem.paramType, _loc);
			auto structBytes = awst::makeAsBytes(base, _loc);
			std::shared_ptr<awst::Expression> data;
			int fieldByteOffset = 0;
			for (auto const& [fieldName, fieldType]: structType->fields())
			{
				int fieldSize = computeARC4ByteSize(fieldType);
				auto extract = awst::makeExtract3(structBytes,
					awst::makeIntegerConstant(fieldByteOffset, _loc),
					awst::makeIntegerConstant(fieldSize, _loc), _loc);
				auto padded = padTo32Bytes(awst::makeAsBiguint(std::move(extract), _loc), _loc);
				data = !data ? std::move(padded) : awst::makeConcat(std::move(data), std::move(padded), _loc);
				fieldByteOffset += fieldSize;
			}
			return awst::makeAsBiguint(awst::makeKeccak256(std::move(data), _loc), _loc);
		}
	}

	// Hash the EXACT length: the old concatSlots(numSlots) form silently
	// truncated an unaligned length to whole words (keccak256(0x84, 0x30)
	// hashed 32 bytes — wrong-but-plausible digests for packed-encoding
	// idioms). Slot-routed (M7): offsets ≥ SLOT_SIZE read the right slot.
	return awst::makeAsBiguint(
		awst::makeKeccak256(readMemRangeDirect(m_typeMapper,
			awst::makeIntegerConstant(*offset, _loc),
			static_cast<int>(*length), _loc), _loc), _loc);
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
	if (_args.size() != static_cast<size_t>(2 + _numTopics))
	{
		Logger::instance().error("log" + std::to_string(_numTopics) + " requires "
			+ std::to_string(2 + _numTopics) + " arguments (offset, length, "
			+ std::to_string(_numTopics) + " topics)", _loc);
		return;
	}

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
	if (!checkArity(_args, 3, "returndatacopy", _loc, "destOffset, offset, size"))
		return;
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

			std::shared_ptr<awst::Expression> payload;
			if (lenC)
			{
				// Constant length: exact multi-slot range read.
				payload = readMemRangeDirect(m_typeMapper,
					offsetToUint64(_args[0], _loc),
					static_cast<int>(std::stoull(lenC->value)), _loc);
			}
			else
			{
				// Dynamic length (`revert(ptr, sub(end, ptr))` — Error(string)
				// tails). A loggable payload is <= 1024 bytes (AVM total-log
				// cap), so it straddles AT MOST one slot boundary: read the
				// in-slot part, and when len overruns the slot, concat the
				// remainder from slot+1 — one log either way.
				int revId = awst::NameGen::next("DataOps.revertSlice");
				std::string offN = "__rev_off_" + std::to_string(revId);
				std::string lenN = "__rev_len_" + std::to_string(revId);
				_out.push_back(awst::makeAssignmentStatement(
					awst::makeVarExpression(offN, awst::WType::uint64Type(), _loc),
					offsetToUint64(_args[0], _loc), _loc));
				_out.push_back(awst::makeAssignmentStatement(
					awst::makeVarExpression(lenN, awst::WType::uint64Type(), _loc),
					offsetToUint64(_args[1], _loc), _loc));
				auto offR = [&]() {
					return awst::makeVarExpression(offN, awst::WType::uint64Type(), _loc);
				};
				auto lenR = [&]() {
					return awst::makeVarExpression(lenN, awst::WType::uint64Type(), _loc);
				};
				auto ss = [&]() {
					return awst::makeIntegerConstant(static_cast<uint64_t>(SLOT_SIZE), _loc);
				};
				auto slotE = [&]() {
					return awst::makeUInt64BinOp(
						offR(), awst::UInt64BinaryOperator::FloorDiv, ss(), _loc);
				};
				auto subE = [&]() {
					return awst::makeUInt64BinOp(
						offR(), awst::UInt64BinaryOperator::Mod, ss(), _loc);
				};
				auto loadsAt = [&](std::shared_ptr<awst::Expression> slot) {
					auto lc = awst::makeIntrinsicCall("loads", awst::WType::bytesType(), _loc);
					lc->stackArgs.push_back(std::move(slot));
					return lc;
				};
				// avail = SLOT_SIZE - off%SLOT_SIZE
				auto availE = [&]() {
					return awst::makeUInt64BinOp(
						ss(), awst::UInt64BinaryOperator::Sub, subE(), _loc);
				};
				auto fits = awst::makeNot(awst::makeNumericCompare(
					availE(), awst::NumericComparison::Lt, lenR(), _loc), _loc);
				auto whole = awst::makeExtract3(loadsAt(slotE()), subE(), lenR(), _loc);
				auto part1 = awst::makeExtract3(loadsAt(slotE()), subE(), availE(), _loc);
				auto part2 = awst::makeExtract3(
					loadsAt(awst::makeUInt64BinOp(slotE(),
						awst::UInt64BinaryOperator::Add,
						awst::makeIntegerConstant("1", _loc), _loc)),
					awst::makeIntegerConstant("0", _loc),
					awst::makeUInt64BinOp(lenR(),
						awst::UInt64BinaryOperator::Sub, availE(), _loc), _loc);
				auto spliced = awst::makeConcat(std::move(part1), std::move(part2), _loc);
				payload = awst::makeConditional(std::move(fits),
					std::move(whole), std::move(spliced), awst::WType::bytesType(), _loc);
			}
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
