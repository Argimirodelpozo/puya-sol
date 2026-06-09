/// @file DataOps.cpp
/// Data operations: calldataload, resolveConstantYulValue, keccak256.

#include "builder/assembly/AssemblyBuilder.h"
#include "Logger.h"

#include <sstream>

namespace puyasol::builder
{

std::shared_ptr<awst::Expression> AssemblyBuilder::handleCalldataload(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	if (_args.size() != 1)
	{
		Logger::instance().error("calldataload requires 1 argument", _loc);
		return nullptr;
	}

	auto offset = resolveConstantOffset(_args[0]);
	if (!offset)
	{
		// Dynamic offset: read from the synthetic calldata blob if we
		// detected dynamic calldata access in this assembly block at
		// pre-translation. The blob mirrors EVM-ABI calldata layout
		// (selector + head + tail) for the function's params.
		if (m_useSyntheticCalldata)
		{
			auto blob = awst::makeVarExpression(CD_BLOB_VAR, awst::WType::bytesType(), _loc);
			auto offArg = offsetToUint64(_args[0], _loc);
			auto lenArg = awst::makeIntegerConstant("32", _loc);
			auto extractCall = awst::makeExtract3(std::move(blob), std::move(offArg), std::move(lenArg), _loc);
			return awst::makeAsBiguint(std::move(extractCall), _loc);
		}
		Logger::instance().error(
			"calldataload with non-constant offset not supported", _loc
		);
		return nullptr;
	}

	auto it = m_calldataMap.find(*offset);
	if (it != m_calldataMap.end())
	{
		auto const& elem = it->second;

		auto base = awst::makeVarExpression(elem.paramName, m_locals.count(elem.paramName)
			? m_locals[elem.paramName]
			: awst::WType::biguintType(), _loc);

		// For bytes/string parameters, calldataload reads 32 bytes at a relative offset.
		// Extract 32 bytes from the parameter and convert to biguint.
		if (elem.paramType
			&& (elem.paramType == awst::WType::bytesType()
				|| elem.paramType == awst::WType::stringType()))
		{
			// Use .find() (not operator[]) — a calldata param name is never a Yul
			// `let` constant, so operator[] would insert+return a spurious 0 entry,
			// mutating the map during a read. Absent ⇒ base offset 0 (relative ==
			// absolute), preserving the prior operator[] behaviour.
			auto lcIt = m_localConstants.find(elem.paramName);
			uint64_t paramBase = lcIt != m_localConstants.end() ? lcIt->second : 0;
			uint64_t relativeOffset = *offset - paramBase;

			auto offArg = awst::makeIntegerConstant(relativeOffset, _loc);

			auto lenArg = awst::makeIntegerConstant("32", _loc);

			auto extractCall = awst::makeExtract3(std::move(base), std::move(offArg), std::move(lenArg), _loc);
			auto cast = awst::makeAsBiguint(std::move(extractCall), _loc);
			return cast;
		}

		return accessFlatElement(std::move(base), elem.paramType, elem.flatIndex, _loc);
	}

	// HARD ERROR — stubbing 0 silently zeros a real input word (amount /
	// recipient / selector). Refuse to compile rather than substitute 0.
	Logger::instance().error(
		"calldataload at unresolvable offset " + std::to_string(*offset) +
		" is not supported on AVM — the calldata word can't be located, so it "
		"would be stubbed as 0, silently zeroing a real input value.", _loc
	);
	auto zero = awst::makeBiguintConstant("0", _loc);
	return zero;
}

std::optional<uint64_t> AssemblyBuilder::resolveConstantYulValue(
	solidity::yul::Expression const& _expr
)
{
	if (auto const* lit = std::get_if<solidity::yul::Literal>(&_expr))
	{
		if (lit->kind == solidity::yul::LiteralKind::Number)
		{
			auto const& val = lit->value.value();
			try
			{
				std::ostringstream oss;
				oss << val;
				return std::stoull(oss.str());
			}
			catch (...)
			{
				return std::nullopt;
			}
		}
	}

	// Check identifiers against local constants and external constants
	if (auto const* id = std::get_if<solidity::yul::Identifier>(&_expr))
	{
		std::string name = id->name.str();

		// Handle .offset / .length suffix on calldata parameter references
		// e.g., _pubSignals.offset → calldata byte offset of _pubSignals
		auto dotPos = name.rfind('.');
		if (dotPos != std::string::npos)
		{
			std::string suffix = name.substr(dotPos + 1);
			std::string baseName = name.substr(0, dotPos);
			if (suffix == "offset")
			{
				auto it = m_localConstants.find(baseName);
				if (it != m_localConstants.end())
					return it->second;
			}
			else if (suffix == "length")
			{
				// .length for calldata arrays: element count * 32
				// For bytes/string: not known at compile time
				// Return nullopt for now (handled dynamically if needed)
			}
		}

		auto it = m_localConstants.find(name);
		if (it != m_localConstants.end())
			return it->second;

		auto cit = m_constants.find(name);
		if (cit != m_constants.end())
		{
			try
			{
				return std::stoull(cit->second);
			}
			catch (...)
			{
				return std::nullopt;
			}
		}
	}

	// Handle function calls: add, sub, mul, mload
	if (auto const* call = std::get_if<solidity::yul::FunctionCall>(&_expr))
	{
		std::string name = getFunctionName(call->functionName);
		if (call->arguments.size() == 2)
		{
			auto left = resolveConstantYulValue(call->arguments[0]);
			auto right = resolveConstantYulValue(call->arguments[1]);
			if (left && right)
			{
				if (name == "add")
					return *left + *right;
				if (name == "sub")
					return *left - *right;
				if (name == "mul")
					return *left * *right;
			}
		}

		// mload(offset) → look up the constant value stored at that memory offset
		if (name == "mload" && call->arguments.size() == 1)
		{
			auto offset = resolveConstantYulValue(call->arguments[0]);
			if (offset)
			{
				// Check if we tracked a constant stored at this offset
				std::ostringstream oss;
				oss << "mem_0x" << std::hex << *offset;
				auto cit = m_localConstants.find(oss.str());
				if (cit != m_localConstants.end())
					return cit->second;
			}
		}
	}

	return std::nullopt;
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleKeccak256(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	if (_args.size() != 2)
	{
		Logger::instance().error("keccak256 requires 2 arguments (offset, length)", _loc);
		return nullptr;
	}

	auto length = resolveConstantOffset(_args[1]);

	// Check for struct (WTuple) parameter FIRST, before constant offset resolution.
	// initializeCalldataMap stores calldata offsets in m_localConstants, which makes
	// struct parameters resolve to false-positive "constant" memory offsets.
	// If the offset expression is a VarExpression with WTuple type, always use the
	// struct hash path regardless of whether the offset resolves to a constant.
	auto const* varExprForTuple = dynamic_cast<awst::VarExpression const*>(_args[0].get());
	if (varExprForTuple && length)
	{
		auto it = m_locals.find(varExprForTuple->name);
		if (it != m_locals.end() && it->second && it->second->kind() == awst::WTypeKind::WTuple)
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

	if (!offset && length)
	{
		// Offset is a variable — check if it references a struct (WTuple) parameter.
		// Pattern: keccak256(structVar, numFields*32) hashes struct fields concatenated.
		auto const* varExpr = dynamic_cast<awst::VarExpression const*>(_args[0].get());
		if (varExpr)
		{
			auto it = m_locals.find(varExpr->name);
			if (it != m_locals.end() && it->second && it->second->kind() == awst::WTypeKind::WTuple)
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

						// keccak256 the concatenated bytes
						auto keccak = awst::makeKeccak256(std::move(data), _loc);
						return awst::makeAsBiguint(std::move(keccak), _loc);
					}
				}
			}
		}

		// With blob model, read directly from the memory blob for dynamic offsets
		if (length)
		{
			// extract3(__evm_memory, offset, length) → keccak256
			auto offsetU64 = offsetToUint64(_args[0], _loc);

			auto lenConst = awst::makeIntegerConstant(*length, _loc);

			auto data = awst::makeExtract3(memoryVar(_loc), std::move(offsetU64), std::move(lenConst), _loc);
			auto keccak = awst::makeKeccak256(std::move(data), _loc);
			return awst::makeAsBiguint(std::move(keccak), _loc);
		}

		Logger::instance().error("keccak256 with non-constant offset/length not supported", _loc);
		return nullptr;
	}

	if (offset && !length)
	{
		// Offset is constant but length is dynamic.
		// Pattern: keccak256(begin, add(paramLength, 0x20)) from deriveMapping(string/bytes).
		// This hashes: param_bytes + last_mstored_32bytes (slot value).
		// Check if offset = calldataParam + 0x20 (pointing to string data area).
		for (auto const& [cdOffset, elem] : m_calldataMap)
		{
			if (*offset == cdOffset + 0x20 && m_lastMstoreValue)
			{
				// Build: keccak256(concat(param_bytes, padTo32(lastMstoreValue)))
				auto paramType = m_locals.find(elem.paramName);
				auto const* paramWtype = (paramType != m_locals.end() && paramType->second)
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

				auto slotPadded = padTo32Bytes(m_lastMstoreValue, _loc);

				auto concat = awst::makeConcat(std::move(paramBytes), std::move(slotPadded), _loc);
				auto keccak = awst::makeKeccak256(std::move(concat), _loc);
				return awst::makeAsBiguint(std::move(keccak), _loc);
			}
		}
	}

	if (!offset || !length)
	{
		// Either offset, length, or both are runtime values. AVM's keccak256
		// opcode accepts any-length bytes, so we just read the slice from the
		// EVM memory blob via runtime extract3 and hash it. Solady's EIP-712
		// typed-data hashing (PermissionedRamp.witnessed{Wrap,Unwrap}) builds
		// the hash buffer dynamically and hits this path.
		auto offsetU64 = offset
			? std::static_pointer_cast<awst::Expression>(awst::makeIntegerConstant(*offset, _loc))
			: offsetToUint64(_args[0], _loc);
		auto lengthU64 = length
			? std::static_pointer_cast<awst::Expression>(awst::makeIntegerConstant(*length, _loc))
			: offsetToUint64(_args[1], _loc);

		auto data = awst::makeExtract3(memoryVar(_loc), std::move(offsetU64), std::move(lengthU64), _loc);
		auto keccak = awst::makeKeccak256(std::move(data), _loc);
		return awst::makeAsBiguint(std::move(keccak), _loc);
	}

	int numSlots = static_cast<int>(*length / 0x20);
	if (*length == 0)
	{
		// keccak256("") = 0xc5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470
		// Hash empty bytes
		auto emptyBytes = awst::makeBytesConstant({}, _loc, awst::BytesEncoding::Unknown);
		auto keccak = awst::makeKeccak256(std::move(emptyBytes), _loc);
		return awst::makeAsBiguint(std::move(keccak), _loc);
	}
	if (numSlots <= 0)
	{
		// Non-zero length but less than 32 bytes — partial slot
		// Read from the memory blob and truncate to exact length
		Logger::instance().warning("keccak256 with sub-32-byte input, using partial slot", _loc);
		{
			auto offsetConst = awst::makeIntegerConstant(*offset, _loc);

			auto lenConst = awst::makeIntegerConstant(*length, _loc);

			auto data = awst::makeExtract3(memoryVar(_loc), std::move(offsetConst), std::move(lenConst), _loc);
			auto keccak = awst::makeKeccak256(std::move(data), _loc);
			return awst::makeAsBiguint(std::move(keccak), _loc);
		}
		// Check if offset = calldataParam + 0x20 (string/bytes data region)
		// Pattern: keccak256(add(param, 0x20), mload(param)) hashes string data
		for (auto const& [cdOffset, elem] : m_calldataMap)
		{
			if (*offset == cdOffset + 0x20)
			{
				// Found: offset points to the string data area of a calldata parameter.
				// On AVM, the parameter IS the string bytes. Hash them directly.
				// The parameter might be bytes or biguint — need bytes for keccak
				auto paramType = m_locals.find(elem.paramName);
				auto const* paramWtype = (paramType != m_locals.end() && paramType->second)
					? paramType->second : awst::WType::bytesType();
				auto paramVar = awst::makeVarExpression(elem.paramName, paramWtype, _loc);

				std::shared_ptr<awst::Expression> hashInput;
				if (paramVar->wtype != awst::WType::bytesType())
				{
					auto cast = awst::makeAsBytes(std::move(paramVar), _loc);
					hashInput = std::move(cast);
				}
				else
					hashInput = std::move(paramVar);

				auto keccak = awst::makeKeccak256(std::move(hashInput), _loc);
				return awst::makeAsBiguint(std::move(keccak), _loc);
			}
		}
		// HARD ERROR — hashing 32 zero bytes yields a deterministic but WRONG
		// digest that poisons commitments / EIP-712 / Merkle leaves / mapping
		// keys. Refuse to compile rather than emit a wrong hash.
		Logger::instance().error(
			"keccak256 over a sub-32-byte length at an unresolvable memory offset "
			"is not supported on AVM — the actual bytes can't be recovered, so the "
			"hash would be computed over 32 zero bytes instead, a deterministic but "
			"wrong digest.", _loc);
		// Stub so AWST building completes; the error above aborts before bytecode.
		auto keccak = awst::makeKeccak256(awst::makeBzero(32, _loc), _loc);
		return awst::makeAsBiguint(std::move(keccak), _loc);
	}

	// Check if the offset range falls within m_calldataMap (function parameters).
	// When keccak256 hashes a struct parameter (e.g., PoolKey), the Yul optimizer
	// may eliminate the abi_encode buffer copy, making the keccak256 offset point
	// directly to the calldata offset. We detect this and extract struct fields
	// from the ARC4-encoded parameter, padding each to 32 bytes for EVM ABI compat.
	auto firstSlotIt = m_calldataMap.find(*offset);
	if (firstSlotIt != m_calldataMap.end())
	{
		auto const& elem = firstSlotIt->second;
		auto const* structType = dynamic_cast<awst::ARC4Struct const*>(elem.paramType);
		if (structType && numSlots == static_cast<int>(structType->fields().size()))
		{
			// ARC4Struct parameter: extract each field from raw bytes, pad to 32 bytes
			auto base = awst::makeVarExpression(elem.paramName, m_locals.count(elem.paramName)
				? m_locals[elem.paramName]
				: elem.paramType, _loc);

			// Cast struct to raw bytes for field extraction
			auto structBytes = awst::makeAsBytes(base, _loc);

			std::shared_ptr<awst::Expression> data;
			int fieldByteOffset = 0;
			for (auto const& [fieldName, fieldType]: structType->fields())
			{
				int fieldSize = computeARC4ByteSize(fieldType);

				// extract3(structBytes, fieldByteOffset, fieldSize)
				auto offExpr = awst::makeIntegerConstant(fieldByteOffset, _loc);

				auto lenExpr = awst::makeIntegerConstant(fieldSize, _loc);

				auto extract = awst::makeExtract3(structBytes, std::move(offExpr), std::move(lenExpr), _loc);
				// Cast to biguint, then pad to 32 bytes
				auto asBiguint = awst::makeAsBiguint(std::move(extract), _loc);

				auto padded = padTo32Bytes(std::move(asBiguint), _loc);

				if (!data)
					data = std::move(padded);
				else
					data = awst::makeConcat(std::move(data), std::move(padded), _loc);
				fieldByteOffset += fieldSize;
			}

			auto keccak = awst::makeKeccak256(std::move(data), _loc);
			return awst::makeAsBiguint(std::move(keccak), _loc);
		}
	}

	// Concatenate all memory slots using extracted helper
	auto data = concatSlots(*offset, 0, numSlots, _loc);

	// Apply keccak256
	auto keccak = awst::makeKeccak256(std::move(data), _loc);

	// Convert bytes result to biguint (for Yul's uint256 type)
	return awst::makeAsBiguint(std::move(keccak), _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleReturndatasize(
	awst::SourceLocation const& _loc
)
{
	// EVM returndatasize(): the size of the most recent call's return-data
	// buffer. On AVM that buffer is the last inner transaction's last log
	// (`itxn LastLog`); its byte length is the return-data size. Returned as
	// uint64 (the natural type for a length; consumers coerce via
	// ensureBiguint only when they need a biguint).
	return awst::makeLen(awst::makeItxn("LastLog", awst::WType::bytesType(), _loc), _loc);
}

void AssemblyBuilder::emitReturndatacopy(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	if (_args.size() != 3)
	{
		Logger::instance().error(
			"returndatacopy requires 3 arguments (destOffset, offset, size)", _loc);
		return;
	}
	// EVM returndatacopy(destOffset, offset, size): copy `size` bytes from the
	// return-data buffer — on AVM the last inner transaction's last log
	// (`itxn LastLog`) — starting at `offset`, into memory at `destOffset`.
	// `extract3` reverts when offset+size exceeds the log length, which matches
	// EVM's out-of-range revert for returndatacopy.
	auto destOff = offsetToUint64(_args[0], _loc);
	auto srcOff = offsetToUint64(_args[1], _loc);
	auto size = offsetToUint64(_args[2], _loc);

	auto log = awst::makeItxn("LastLog", awst::WType::bytesType(), _loc);
	auto slice = awst::makeExtract3(std::move(log), std::move(srcOff), std::move(size), _loc);

	// writeMemWordDyn is length-driven (its replace3 writes len(slice) bytes,
	// not a fixed 32), so it copies the whole slice into the blob with the
	// slot-0/slot-1+ conditional and the memory-bounds assert.
	writeMemWordDyn(std::move(destOff), std::move(slice), _loc, _out);
}

void AssemblyBuilder::handleRevert(
	std::vector<std::shared_ptr<awst::Expression>> const& /* _args */,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	// revert(offset, length) — on AVM, assert(false, "revert")
	auto stmt = awst::makeExpressionStatement(awst::makeAssert(awst::makeFalse(_loc), _loc, "revert"), _loc);
	_out.push_back(std::move(stmt));
	// Mark this Yul block as halt-terminated so the assembly-block
	// epilog skips its `__evm_memory` writeback. assert(false) is a
	// hard halt; any trailing store would be unreachable code, which
	// puya's IR validator rejects (see AccessManager / LowLevelCall.bubbleRevert).
	m_haltEmitted = true;
}



// ─── Precompile helper methods ──────────────────────────────────────────────


} // namespace puyasol::builder
