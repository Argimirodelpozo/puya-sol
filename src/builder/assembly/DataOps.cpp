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
			uint64_t relativeOffset = *offset - m_localConstants[elem.paramName];

			auto offArg = awst::makeIntegerConstant(std::to_string(relativeOffset), _loc);

			auto lenArg = awst::makeIntegerConstant("32", _loc);

			auto extractCall = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
			extractCall->stackArgs.push_back(std::move(base));
			extractCall->stackArgs.push_back(std::move(offArg));
			extractCall->stackArgs.push_back(std::move(lenArg));

			auto cast = awst::makeReinterpretCast(std::move(extractCall), awst::WType::biguintType(), _loc);
			return cast;
		}

		return accessFlatElement(std::move(base), elem.paramType, elem.flatIndex, _loc);
	}

	Logger::instance().warning(
		"calldataload at unknown offset " + std::to_string(*offset) + ", returning 0", _loc
	);
	auto zero = awst::makeIntegerConstant("0", _loc, awst::WType::biguintType());
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
						auto field = std::make_shared<awst::TupleItemExpression>();
						field->sourceLocation = _loc;
						field->wtype = tupleType->types()[static_cast<size_t>(i)];
						field->base = _args[0];
						field->index = i;

						auto padded = padTo32Bytes(std::move(field), _loc);

						if (!data)
							data = std::move(padded);
						else
						{
							auto concat = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
							concat->stackArgs.push_back(std::move(data));
							concat->stackArgs.push_back(std::move(padded));
							data = std::move(concat);
						}
					}

					auto keccak = awst::makeIntrinsicCall("keccak256", awst::WType::bytesType(), _loc);
					keccak->stackArgs.push_back(std::move(data));

					auto castResult = awst::makeReinterpretCast(std::move(keccak), awst::WType::biguintType(), _loc);
					return castResult;
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
							auto field = std::make_shared<awst::TupleItemExpression>();
							field->sourceLocation = _loc;
							field->wtype = tupleType->types()[static_cast<size_t>(i)];
							field->base = _args[0];
							field->index = i;

							auto padded = padTo32Bytes(std::move(field), _loc);

							if (!data)
								data = std::move(padded);
							else
							{
								auto concat = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
								concat->stackArgs.push_back(std::move(data));
								concat->stackArgs.push_back(std::move(padded));
								data = std::move(concat);
							}
						}

						// keccak256 the concatenated bytes
						auto keccak = awst::makeIntrinsicCall("keccak256", awst::WType::bytesType(), _loc);
						keccak->stackArgs.push_back(std::move(data));

						auto castResult = awst::makeReinterpretCast(std::move(keccak), awst::WType::biguintType(), _loc);
						return castResult;
					}
				}
			}
		}

		// With blob model, read directly from the memory blob for dynamic offsets
		if (length)
		{
			// extract3(__evm_memory, offset, length) → keccak256
			auto offsetU64 = offsetToUint64(_args[0], _loc);

			auto lenConst = awst::makeIntegerConstant(std::to_string(*length), _loc);

			auto data = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
			data->stackArgs.push_back(memoryVar(_loc));
			data->stackArgs.push_back(std::move(offsetU64));
			data->stackArgs.push_back(std::move(lenConst));

			auto keccak = awst::makeIntrinsicCall("keccak256", awst::WType::bytesType(), _loc);
			keccak->stackArgs.push_back(std::move(data));

			auto castResult = awst::makeReinterpretCast(std::move(keccak), awst::WType::biguintType(), _loc);
			return castResult;
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
				auto paramVar = std::make_shared<awst::VarExpression>();
				paramVar->sourceLocation = _loc;
				paramVar->name = elem.paramName;
				auto paramType = m_locals.find(elem.paramName);
				if (paramType != m_locals.end() && paramType->second)
					paramVar->wtype = paramType->second;
				else
					paramVar->wtype = awst::WType::bytesType();

				std::shared_ptr<awst::Expression> paramBytes;
				if (paramVar->wtype != awst::WType::bytesType())
				{
					auto cast = awst::makeReinterpretCast(std::move(paramVar), awst::WType::bytesType(), _loc);
					paramBytes = std::move(cast);
				}
				else
					paramBytes = std::move(paramVar);

				auto slotPadded = padTo32Bytes(m_lastMstoreValue, _loc);

				auto concat = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
				concat->stackArgs.push_back(std::move(paramBytes));
				concat->stackArgs.push_back(std::move(slotPadded));

				auto keccak = awst::makeIntrinsicCall("keccak256", awst::WType::bytesType(), _loc);
				keccak->stackArgs.push_back(std::move(concat));

				auto castResult = awst::makeReinterpretCast(std::move(keccak), awst::WType::biguintType(), _loc);
				return castResult;
			}
		}
	}

	if (!offset || !length)
	{
		// Either offset or length (or both) are runtime values. AVM's keccak256
		// opcode accepts any-length bytes, so we just read the slice from the
		// EVM memory blob via runtime extract3 and hash it.
		auto offsetU64 = offset
			? std::static_pointer_cast<awst::Expression>(awst::makeIntegerConstant(std::to_string(*offset), _loc))
			: offsetToUint64(_args[0], _loc);
		auto lengthU64 = length
			? std::static_pointer_cast<awst::Expression>(awst::makeIntegerConstant(std::to_string(*length), _loc))
			: offsetToUint64(_args[1], _loc);

		auto data = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
		data->stackArgs.push_back(memoryVar(_loc));
		data->stackArgs.push_back(std::move(offsetU64));
		data->stackArgs.push_back(std::move(lengthU64));

		auto keccak = awst::makeIntrinsicCall("keccak256", awst::WType::bytesType(), _loc);
		keccak->stackArgs.push_back(std::move(data));

		auto castResult = awst::makeReinterpretCast(std::move(keccak), awst::WType::biguintType(), _loc);
		return castResult;
	}

	int numSlots = static_cast<int>(*length / 0x20);
	if (*length == 0)
	{
		// keccak256("") = 0xc5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470
		// Hash empty bytes
		auto emptyBytes = awst::makeBytesConstant({}, _loc, awst::BytesEncoding::Unknown);

		auto keccak = awst::makeIntrinsicCall("keccak256", awst::WType::bytesType(), _loc);
		keccak->stackArgs.push_back(std::move(emptyBytes));

		auto cast = awst::makeReinterpretCast(std::move(keccak), awst::WType::biguintType(), _loc);
		return cast;
	}
	if (numSlots <= 0)
	{
		// Non-zero length but less than 32 bytes — partial slot
		// Read from the memory blob and truncate to exact length
		Logger::instance().warning("keccak256 with sub-32-byte input, using partial slot", _loc);
		{
			auto offsetConst = awst::makeIntegerConstant(std::to_string(*offset), _loc);

			auto lenConst = awst::makeIntegerConstant(std::to_string(*length), _loc);

			auto data = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
			data->stackArgs.push_back(memoryVar(_loc));
			data->stackArgs.push_back(std::move(offsetConst));
			data->stackArgs.push_back(std::move(lenConst));

			auto keccak = awst::makeIntrinsicCall("keccak256", awst::WType::bytesType(), _loc);
			keccak->stackArgs.push_back(std::move(data));

			auto castResult = awst::makeReinterpretCast(std::move(keccak), awst::WType::biguintType(), _loc);
			return castResult;
		}
		// Check if offset = calldataParam + 0x20 (string/bytes data region)
		// Pattern: keccak256(add(param, 0x20), mload(param)) hashes string data
		for (auto const& [cdOffset, elem] : m_calldataMap)
		{
			if (*offset == cdOffset + 0x20)
			{
				// Found: offset points to the string data area of a calldata parameter.
				// On AVM, the parameter IS the string bytes. Hash them directly.
				auto paramVar = std::make_shared<awst::VarExpression>();
				paramVar->sourceLocation = _loc;
				paramVar->name = elem.paramName;
				// The parameter might be bytes or biguint — need bytes for keccak
				auto paramType = m_locals.find(elem.paramName);
				if (paramType != m_locals.end() && paramType->second)
					paramVar->wtype = paramType->second;
				else
					paramVar->wtype = awst::WType::bytesType();

				std::shared_ptr<awst::Expression> hashInput;
				if (paramVar->wtype != awst::WType::bytesType())
				{
					auto cast = awst::makeReinterpretCast(std::move(paramVar), awst::WType::bytesType(), _loc);
					hashInput = std::move(cast);
				}
				else
					hashInput = std::move(paramVar);

				auto keccak = awst::makeIntrinsicCall("keccak256", awst::WType::bytesType(), _loc);
				keccak->stackArgs.push_back(std::move(hashInput));

				auto castResult = awst::makeReinterpretCast(std::move(keccak), awst::WType::biguintType(), _loc);
				return castResult;
			}
		}
		Logger::instance().warning("keccak256 with sub-32-byte length and unknown memory slot, using keccak256(bzero(32))", _loc);
		// Fallback: hash 32 zero bytes (will produce a deterministic but incorrect hash)
		auto zeroBytes = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), _loc);
		auto thirtyTwo = awst::makeIntegerConstant("32", _loc);
		zeroBytes->stackArgs.push_back(std::move(thirtyTwo));

		auto keccak = awst::makeIntrinsicCall("keccak256", awst::WType::bytesType(), _loc);
		keccak->stackArgs.push_back(std::move(zeroBytes));

		auto castResult = awst::makeReinterpretCast(std::move(keccak), awst::WType::biguintType(), _loc);
		return castResult;
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
			auto structBytes = awst::makeReinterpretCast(base, awst::WType::bytesType(), _loc);

			std::shared_ptr<awst::Expression> data;
			int fieldByteOffset = 0;
			for (auto const& [fieldName, fieldType]: structType->fields())
			{
				int fieldSize = computeARC4ByteSize(fieldType);

				// extract3(structBytes, fieldByteOffset, fieldSize)
				auto offExpr = awst::makeIntegerConstant(std::to_string(fieldByteOffset), _loc);

				auto lenExpr = awst::makeIntegerConstant(std::to_string(fieldSize), _loc);

				auto extract = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
				extract->stackArgs.push_back(structBytes);
				extract->stackArgs.push_back(std::move(offExpr));
				extract->stackArgs.push_back(std::move(lenExpr));

				// Cast to biguint, then pad to 32 bytes
				auto asBiguint = awst::makeReinterpretCast(std::move(extract), awst::WType::biguintType(), _loc);

				auto padded = padTo32Bytes(std::move(asBiguint), _loc);

				if (!data)
					data = std::move(padded);
				else
				{
					auto concat = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
					concat->stackArgs.push_back(std::move(data));
					concat->stackArgs.push_back(std::move(padded));
					data = std::move(concat);
				}
				fieldByteOffset += fieldSize;
			}

			auto keccak = awst::makeIntrinsicCall("keccak256", awst::WType::bytesType(), _loc);
			keccak->stackArgs.push_back(std::move(data));

			auto castResult = awst::makeReinterpretCast(std::move(keccak), awst::WType::biguintType(), _loc);
			return castResult;
		}
	}

	// Concatenate all memory slots using extracted helper
	auto data = concatSlots(*offset, 0, numSlots, _loc);

	// Apply keccak256
	auto keccak = awst::makeIntrinsicCall("keccak256", awst::WType::bytesType(), _loc);
	keccak->stackArgs.push_back(std::move(data));

	// Convert bytes result to biguint (for Yul's uint256 type)
	auto castResult = awst::makeReinterpretCast(std::move(keccak), awst::WType::biguintType(), _loc);

	return castResult;
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleReturndatasize(
	awst::SourceLocation const& _loc
)
{
	// On AVM there is no return data concept — return 0
	auto zero = awst::makeIntegerConstant("0", _loc, awst::WType::biguintType());
	return zero;
}

void AssemblyBuilder::handleRevert(
	std::vector<std::shared_ptr<awst::Expression>> const& /* _args */,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	// revert(offset, length) — on AVM, assert(false, "revert")
	auto stmt = awst::makeExpressionStatement(awst::makeAssert(awst::makeBoolConstant(false, _loc), _loc, "revert"), _loc);
	_out.push_back(std::move(stmt));
}

// ─── Precompile helper methods ──────────────────────────────────────────────


} // namespace puyasol::builder
