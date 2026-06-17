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
	if (!checkArity(_args, 1, "calldataload", _loc))
		return nullptr;

	auto offset = resolveConstantOffset(_args[0]);
	if (!offset)
	{
		// Dynamic offset: read from __cd_blob (EVM-ABI layout) when present.
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

		// bytes/string: calldataload reads 32 bytes at a relative offset.
		if (elem.paramType
			&& (elem.paramType == awst::WType::bytesType()
				|| elem.paramType == awst::WType::stringType()))
		{
			// Use find() not operator[]: operator[] would insert a spurious 0 entry.
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

	// Stubbing 0 would silently zero a real input word; hard-error instead.
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

	if (auto const* id = std::get_if<solidity::yul::Identifier>(&_expr))
	{
		std::string name = id->name.str();

		// Handle .offset/.length suffix: _pubSignals.offset → calldata byte offset.
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
				// .length for bytes/string not known at compile time; fall through.
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
	if (!checkArity(_args, 2, "keccak256", _loc, "offset, length"))
		return nullptr;

	auto length = resolveConstantOffset(_args[1]);

	// Check for WTuple FIRST: initializeCalldataMap stores calldata offsets in
	// m_localConstants, causing struct params to resolve as false-positive constants.
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
		// Variable offset: check for keccak256(structVar, numFields*32) pattern.
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
						std::shared_ptr<awst::Expression> data;
						for (int i = 0; i < numFields; ++i)
						{
							auto field = awst::makeTupleItem(_args[0], i, tupleType->types()[static_cast<size_t>(i)], _loc);
							auto padded = padTo32Bytes(std::move(field), _loc);
							data = !data ? std::move(padded) : awst::makeConcat(std::move(data), std::move(padded), _loc);
						}
						auto keccak = awst::makeKeccak256(std::move(data), _loc);
						return awst::makeAsBiguint(std::move(keccak), _loc);
					}
				}
			}
		}

		if (length)
		{
			auto offsetU64 = offsetToUint64(_args[0], _loc);
			auto data = awst::makeExtract3(memoryVar(_loc), std::move(offsetU64),
				awst::makeIntegerConstant(*length, _loc), _loc);
			return awst::makeAsBiguint(awst::makeKeccak256(std::move(data), _loc), _loc);
		}

		Logger::instance().error("keccak256 with non-constant offset/length not supported", _loc);
		return nullptr;
	}

	if (offset && !length)
	{
		// Constant offset, dynamic length.
		// Pattern: keccak256(begin, add(paramLen, 0x20)) from deriveMapping(string/bytes).
		// Hashes param_bytes ++ padTo32(last mstored value).
		for (auto const& [cdOffset, elem] : m_calldataMap)
		{
			if (*offset == cdOffset + 0x20 && m_lastMstoreValue)
			{
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
		// Runtime offset or length: read slice via extract3 then hash.
		// Solady EIP-712 (PermissionedRamp.witnessed*) hits this path.
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
		auto emptyBytes = awst::makeBytesConstant({}, _loc, awst::BytesEncoding::Unknown);
		auto keccak = awst::makeKeccak256(std::move(emptyBytes), _loc);
		return awst::makeAsBiguint(std::move(keccak), _loc);
	}
	if (numSlots <= 0)
	{
		// Non-zero but <32 bytes — partial slot; read exact length from blob.
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
		// Hashing 32 zero bytes gives a deterministic but wrong digest.
		Logger::instance().error(
			"keccak256 over a sub-32-byte length at an unresolvable memory offset "
			"is not supported on AVM — the actual bytes can't be recovered, so the "
			"hash would be computed over 32 zero bytes instead, a deterministic but "
			"wrong digest.", _loc);
		// Stub so AWST building completes; error aborts before bytecode.
		return awst::makeAsBiguint(awst::makeKeccak256(awst::makeBzero(32, _loc), _loc), _loc);
	}

	// If offset falls in m_calldataMap (e.g. Yul optimizer elided abi_encode buffer copy
	// for a struct param like PoolKey), extract fields and pad each to 32 bytes.
	auto firstSlotIt = m_calldataMap.find(*offset);
	if (firstSlotIt != m_calldataMap.end())
	{
		auto const& elem = firstSlotIt->second;
		auto const* structType = dynamic_cast<awst::ARC4Struct const*>(elem.paramType);
		if (structType && numSlots == static_cast<int>(structType->fields().size()))
		{
			auto base = awst::makeVarExpression(elem.paramName, m_locals.count(elem.paramName)
				? m_locals[elem.paramName] : elem.paramType, _loc);
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

	return awst::makeAsBiguint(
		awst::makeKeccak256(concatSlots(*offset, 0, numSlots, _loc), _loc), _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleReturndatasize(
	awst::SourceLocation const& _loc
)
{
	// itxn LastLog length; returned as uint64 (consumer coerces when needed).
	return awst::makeLen(awst::makeItxn("LastLog", awst::WType::bytesType(), _loc), _loc);
}

void AssemblyBuilder::emitReturndatacopy(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	if (!checkArity(_args, 3, "returndatacopy", _loc, "destOffset, offset, size"))
		return;
	// Copy size bytes of itxn LastLog[offset..] into memory at destOffset.
	// extract3 reverts on OOB, matching EVM returndatacopy semantics.
	auto destOff = offsetToUint64(_args[0], _loc);
	auto srcOff = offsetToUint64(_args[1], _loc);
	auto size = offsetToUint64(_args[2], _loc);

	auto log = awst::makeItxn("LastLog", awst::WType::bytesType(), _loc);
	auto slice = awst::makeExtract3(std::move(log), std::move(srcOff), std::move(size), _loc);
	// writeMemWordDyn is length-driven (replace3 writes len(slice) bytes), so it
	// handles the slot-0/slot-1+ conditional and bounds assert for the full slice.
	writeMemWordDyn(std::move(destOff), std::move(slice), _loc, _out);
}

void AssemblyBuilder::handleRevert(
	std::vector<std::shared_ptr<awst::Expression>> const& /* _args */,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	_out.push_back(awst::makeExpressionStatement(awst::makeAssert(awst::makeFalse(_loc), _loc, "revert"), _loc));
	// Mark halted: assert(false) is unconditional; trailing blob writeback
	// would be unreachable — puya's IR validator rejects it.
	m_haltEmitted = true;
}



// ─── Precompile helper methods ──────────────────────────────────────────────


} // namespace puyasol::builder
