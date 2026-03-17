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

		auto base = std::make_shared<awst::VarExpression>();
		base->sourceLocation = _loc;
		base->name = elem.paramName;
		base->wtype = m_locals.count(elem.paramName)
			? m_locals[elem.paramName]
			: awst::WType::biguintType();

		// For bytes/string parameters, calldataload reads 32 bytes at a relative offset.
		// Extract 32 bytes from the parameter and convert to biguint.
		if (elem.paramType
			&& (elem.paramType == awst::WType::bytesType()
				|| elem.paramType == awst::WType::stringType()))
		{
			uint64_t relativeOffset = *offset - m_localConstants[elem.paramName];

			auto offArg = std::make_shared<awst::IntegerConstant>();
			offArg->sourceLocation = _loc;
			offArg->wtype = awst::WType::uint64Type();
			offArg->value = std::to_string(relativeOffset);

			auto lenArg = std::make_shared<awst::IntegerConstant>();
			lenArg->sourceLocation = _loc;
			lenArg->wtype = awst::WType::uint64Type();
			lenArg->value = "32";

			auto extractCall = std::make_shared<awst::IntrinsicCall>();
			extractCall->sourceLocation = _loc;
			extractCall->wtype = awst::WType::bytesType();
			extractCall->opCode = "extract3";
			extractCall->stackArgs.push_back(std::move(base));
			extractCall->stackArgs.push_back(std::move(offArg));
			extractCall->stackArgs.push_back(std::move(lenArg));

			auto cast = std::make_shared<awst::ReinterpretCast>();
			cast->sourceLocation = _loc;
			cast->wtype = awst::WType::biguintType();
			cast->expr = std::move(extractCall);
			return cast;
		}

		return accessFlatElement(std::move(base), elem.paramType, elem.flatIndex, _loc);
	}

	Logger::instance().warning(
		"calldataload at unknown offset " + std::to_string(*offset) + ", returning 0", _loc
	);
	auto zero = std::make_shared<awst::IntegerConstant>();
	zero->sourceLocation = _loc;
	zero->wtype = awst::WType::biguintType();
	zero->value = "0";
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
		std::string name = call->functionName.name.str();
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
				auto it = m_memoryMap.find(*offset);
				if (it != m_memoryMap.end() && !it->second.isParam)
				{
					auto cit = m_localConstants.find(it->second.varName);
					if (cit != m_localConstants.end())
						return cit->second;
				}
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
							auto concat = std::make_shared<awst::IntrinsicCall>();
							concat->sourceLocation = _loc;
							concat->wtype = awst::WType::bytesType();
							concat->opCode = "concat";
							concat->stackArgs.push_back(std::move(data));
							concat->stackArgs.push_back(std::move(padded));
							data = std::move(concat);
						}
					}

					auto keccak = std::make_shared<awst::IntrinsicCall>();
					keccak->sourceLocation = _loc;
					keccak->wtype = awst::WType::bytesType();
					keccak->opCode = "keccak256";
					keccak->stackArgs.push_back(std::move(data));

					auto castResult = std::make_shared<awst::ReinterpretCast>();
					castResult->sourceLocation = _loc;
					castResult->wtype = awst::WType::biguintType();
					castResult->expr = std::move(keccak);
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
								auto concat = std::make_shared<awst::IntrinsicCall>();
								concat->sourceLocation = _loc;
								concat->wtype = awst::WType::bytesType();
								concat->opCode = "concat";
								concat->stackArgs.push_back(std::move(data));
								concat->stackArgs.push_back(std::move(padded));
								data = std::move(concat);
							}
						}

						// keccak256 the concatenated bytes
						auto keccak = std::make_shared<awst::IntrinsicCall>();
						keccak->sourceLocation = _loc;
						keccak->wtype = awst::WType::bytesType();
						keccak->opCode = "keccak256";
						keccak->stackArgs.push_back(std::move(data));

						auto castResult = std::make_shared<awst::ReinterpretCast>();
						castResult->sourceLocation = _loc;
						castResult->wtype = awst::WType::biguintType();
						castResult->expr = std::move(keccak);
						return castResult;
					}
				}
			}
		}

		// Try variable-offset memory map: offset decomposes as VarExpr + constant
		auto decomposed = decomposeVarOffset(_args[0]);
		if (decomposed && length)
		{
			auto const& [baseName, baseOffset] = *decomposed;
			auto vit = m_varMemoryMap.find(baseName);
			if (vit != m_varMemoryMap.end())
			{
				int numSlots = static_cast<int>(*length / 0x20);
				if (numSlots > 0)
				{
					// Concatenate stored runtime variables from variable-offset map
					std::shared_ptr<awst::Expression> data;
					for (int i = 0; i < numSlots; ++i)
					{
						uint64_t slotOff = baseOffset + static_cast<uint64_t>(i) * 0x20;
						auto sit = vit->second.find(slotOff);
						std::shared_ptr<awst::Expression> slotVal;
						if (sit != vit->second.end())
						{
							auto var = std::make_shared<awst::VarExpression>();
							var->sourceLocation = _loc;
							var->name = sit->second.varName;
							var->wtype = awst::WType::biguintType();
							slotVal = padTo32Bytes(std::move(var), _loc);
						}
						else
						{
							// Slot not found — use zero
							auto zero = std::make_shared<awst::IntegerConstant>();
							zero->sourceLocation = _loc;
							zero->wtype = awst::WType::biguintType();
							zero->value = "0";
							slotVal = padTo32Bytes(std::move(zero), _loc);
						}
						if (!data)
							data = std::move(slotVal);
						else
						{
							auto concat = std::make_shared<awst::IntrinsicCall>();
							concat->sourceLocation = _loc;
							concat->wtype = awst::WType::bytesType();
							concat->opCode = "concat";
							concat->stackArgs.push_back(std::move(data));
							concat->stackArgs.push_back(std::move(slotVal));
							data = std::move(concat);
						}
					}

					auto keccak = std::make_shared<awst::IntrinsicCall>();
					keccak->sourceLocation = _loc;
					keccak->wtype = awst::WType::bytesType();
					keccak->opCode = "keccak256";
					keccak->stackArgs.push_back(std::move(data));

					auto castResult = std::make_shared<awst::ReinterpretCast>();
					castResult->sourceLocation = _loc;
					castResult->wtype = awst::WType::biguintType();
					castResult->expr = std::move(keccak);
					return castResult;
				}
			}
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
					auto cast = std::make_shared<awst::ReinterpretCast>();
					cast->sourceLocation = _loc;
					cast->wtype = awst::WType::bytesType();
					cast->expr = std::move(paramVar);
					paramBytes = std::move(cast);
				}
				else
					paramBytes = std::move(paramVar);

				auto slotPadded = padTo32Bytes(m_lastMstoreValue, _loc);

				auto concat = std::make_shared<awst::IntrinsicCall>();
				concat->sourceLocation = _loc;
				concat->wtype = awst::WType::bytesType();
				concat->opCode = "concat";
				concat->stackArgs.push_back(std::move(paramBytes));
				concat->stackArgs.push_back(std::move(slotPadded));

				auto keccak = std::make_shared<awst::IntrinsicCall>();
				keccak->sourceLocation = _loc;
				keccak->wtype = awst::WType::bytesType();
				keccak->opCode = "keccak256";
				keccak->stackArgs.push_back(std::move(concat));

				auto castResult = std::make_shared<awst::ReinterpretCast>();
				castResult->sourceLocation = _loc;
				castResult->wtype = awst::WType::biguintType();
				castResult->expr = std::move(keccak);
				return castResult;
			}
		}
	}

	if (!offset || !length)
	{
		Logger::instance().error("keccak256 with non-constant offset/length not supported", _loc);
		return nullptr;
	}

	int numSlots = static_cast<int>(*length / 0x20);
	if (*length == 0)
	{
		// keccak256("") = 0xc5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470
		// Hash empty bytes
		auto emptyBytes = std::make_shared<awst::BytesConstant>();
		emptyBytes->sourceLocation = _loc;
		emptyBytes->wtype = awst::WType::bytesType();
		// empty value

		auto keccak = std::make_shared<awst::IntrinsicCall>();
		keccak->sourceLocation = _loc;
		keccak->wtype = awst::WType::bytesType();
		keccak->opCode = "keccak256";
		keccak->stackArgs.push_back(std::move(emptyBytes));

		auto cast = std::make_shared<awst::ReinterpretCast>();
		cast->sourceLocation = _loc;
		cast->wtype = awst::WType::biguintType();
		cast->expr = std::move(keccak);
		return cast;
	}
	if (numSlots <= 0)
	{
		// Non-zero length but less than 32 bytes — partial slot
		// Handle by reading from memory slot at offset
		Logger::instance().warning("keccak256 with sub-32-byte input, using partial slot", _loc);
		auto slotIt = m_memoryMap.find(*offset);
		if (slotIt != m_memoryMap.end())
		{
			auto slotVar = std::make_shared<awst::VarExpression>();
			slotVar->sourceLocation = _loc;
			slotVar->name = slotIt->second.varName;
			slotVar->wtype = awst::WType::biguintType();

			// Cast biguint to bytes for keccak256
			auto castToBytes = std::make_shared<awst::ReinterpretCast>();
			castToBytes->sourceLocation = _loc;
			castToBytes->wtype = awst::WType::bytesType();
			castToBytes->expr = std::move(slotVar);

			auto keccak = std::make_shared<awst::IntrinsicCall>();
			keccak->sourceLocation = _loc;
			keccak->wtype = awst::WType::bytesType();
			keccak->opCode = "keccak256";
			keccak->stackArgs.push_back(std::move(castToBytes));

			auto castResult = std::make_shared<awst::ReinterpretCast>();
			castResult->sourceLocation = _loc;
			castResult->wtype = awst::WType::biguintType();
			castResult->expr = std::move(keccak);
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
					auto cast = std::make_shared<awst::ReinterpretCast>();
					cast->sourceLocation = _loc;
					cast->wtype = awst::WType::bytesType();
					cast->expr = std::move(paramVar);
					hashInput = std::move(cast);
				}
				else
					hashInput = std::move(paramVar);

				auto keccak = std::make_shared<awst::IntrinsicCall>();
				keccak->sourceLocation = _loc;
				keccak->wtype = awst::WType::bytesType();
				keccak->opCode = "keccak256";
				keccak->stackArgs.push_back(std::move(hashInput));

				auto castResult = std::make_shared<awst::ReinterpretCast>();
				castResult->sourceLocation = _loc;
				castResult->wtype = awst::WType::biguintType();
				castResult->expr = std::move(keccak);
				return castResult;
			}
		}
		Logger::instance().warning("keccak256 with sub-32-byte length and unknown memory slot, using keccak256(bzero(32))", _loc);
		// Fallback: hash 32 zero bytes (will produce a deterministic but incorrect hash)
		auto zeroBytes = std::make_shared<awst::IntrinsicCall>();
		zeroBytes->sourceLocation = _loc;
		zeroBytes->wtype = awst::WType::bytesType();
		zeroBytes->opCode = "bzero";
		auto thirtyTwo = std::make_shared<awst::IntegerConstant>();
		thirtyTwo->sourceLocation = _loc;
		thirtyTwo->wtype = awst::WType::uint64Type();
		thirtyTwo->value = "32";
		zeroBytes->stackArgs.push_back(std::move(thirtyTwo));

		auto keccak = std::make_shared<awst::IntrinsicCall>();
		keccak->sourceLocation = _loc;
		keccak->wtype = awst::WType::bytesType();
		keccak->opCode = "keccak256";
		keccak->stackArgs.push_back(std::move(zeroBytes));

		auto castResult = std::make_shared<awst::ReinterpretCast>();
		castResult->sourceLocation = _loc;
		castResult->wtype = awst::WType::biguintType();
		castResult->expr = std::move(keccak);
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
			auto base = std::make_shared<awst::VarExpression>();
			base->sourceLocation = _loc;
			base->name = elem.paramName;
			base->wtype = m_locals.count(elem.paramName)
				? m_locals[elem.paramName]
				: elem.paramType;

			// Cast struct to raw bytes for field extraction
			auto structBytes = std::make_shared<awst::ReinterpretCast>();
			structBytes->sourceLocation = _loc;
			structBytes->wtype = awst::WType::bytesType();
			structBytes->expr = base;

			std::shared_ptr<awst::Expression> data;
			int fieldByteOffset = 0;
			for (auto const& [fieldName, fieldType]: structType->fields())
			{
				int fieldSize = computeARC4ByteSize(fieldType);

				// extract3(structBytes, fieldByteOffset, fieldSize)
				auto offExpr = std::make_shared<awst::IntegerConstant>();
				offExpr->sourceLocation = _loc;
				offExpr->wtype = awst::WType::uint64Type();
				offExpr->value = std::to_string(fieldByteOffset);

				auto lenExpr = std::make_shared<awst::IntegerConstant>();
				lenExpr->sourceLocation = _loc;
				lenExpr->wtype = awst::WType::uint64Type();
				lenExpr->value = std::to_string(fieldSize);

				auto extract = std::make_shared<awst::IntrinsicCall>();
				extract->sourceLocation = _loc;
				extract->wtype = awst::WType::bytesType();
				extract->opCode = "extract3";
				extract->stackArgs.push_back(structBytes);
				extract->stackArgs.push_back(std::move(offExpr));
				extract->stackArgs.push_back(std::move(lenExpr));

				// Cast to biguint, then pad to 32 bytes
				auto asBiguint = std::make_shared<awst::ReinterpretCast>();
				asBiguint->sourceLocation = _loc;
				asBiguint->wtype = awst::WType::biguintType();
				asBiguint->expr = std::move(extract);

				auto padded = padTo32Bytes(std::move(asBiguint), _loc);

				if (!data)
					data = std::move(padded);
				else
				{
					auto concat = std::make_shared<awst::IntrinsicCall>();
					concat->sourceLocation = _loc;
					concat->wtype = awst::WType::bytesType();
					concat->opCode = "concat";
					concat->stackArgs.push_back(std::move(data));
					concat->stackArgs.push_back(std::move(padded));
					data = std::move(concat);
				}
				fieldByteOffset += fieldSize;
			}

			auto keccak = std::make_shared<awst::IntrinsicCall>();
			keccak->sourceLocation = _loc;
			keccak->wtype = awst::WType::bytesType();
			keccak->opCode = "keccak256";
			keccak->stackArgs.push_back(std::move(data));

			auto castResult = std::make_shared<awst::ReinterpretCast>();
			castResult->sourceLocation = _loc;
			castResult->wtype = awst::WType::biguintType();
			castResult->expr = std::move(keccak);
			return castResult;
		}
	}

	// Concatenate all memory slots using extracted helper
	auto data = concatSlots(*offset, 0, numSlots, _loc);

	// Apply keccak256
	auto keccak = std::make_shared<awst::IntrinsicCall>();
	keccak->sourceLocation = _loc;
	keccak->wtype = awst::WType::bytesType();
	keccak->opCode = "keccak256";
	keccak->stackArgs.push_back(std::move(data));

	// Convert bytes result to biguint (for Yul's uint256 type)
	auto castResult = std::make_shared<awst::ReinterpretCast>();
	castResult->sourceLocation = _loc;
	castResult->wtype = awst::WType::biguintType();
	castResult->expr = std::move(keccak);

	return castResult;
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleReturndatasize(
	awst::SourceLocation const& _loc
)
{
	// On AVM there is no return data concept — return 0
	auto zero = std::make_shared<awst::IntegerConstant>();
	zero->sourceLocation = _loc;
	zero->wtype = awst::WType::biguintType();
	zero->value = "0";
	return zero;
}

void AssemblyBuilder::handleRevert(
	std::vector<std::shared_ptr<awst::Expression>> const& /* _args */,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	// revert(offset, length) — on AVM, assert(false, "revert")
	auto assertExpr = std::make_shared<awst::AssertExpression>();
	assertExpr->sourceLocation = _loc;
	assertExpr->wtype = awst::WType::voidType();

	auto falseLit = std::make_shared<awst::BoolConstant>();
	falseLit->sourceLocation = _loc;
	falseLit->wtype = awst::WType::boolType();
	falseLit->value = false;

	assertExpr->condition = std::move(falseLit);
	assertExpr->errorMessage = "revert";

	auto stmt = std::make_shared<awst::ExpressionStatement>();
	stmt->sourceLocation = _loc;
	stmt->expr = std::move(assertExpr);
	_out.push_back(std::move(stmt));
}

// ─── Precompile helper methods ──────────────────────────────────────────────


} // namespace puyasol::builder
