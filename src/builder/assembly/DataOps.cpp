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
			auto extractCall = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
			extractCall->stackArgs.push_back(std::move(blob));
			extractCall->stackArgs.push_back(std::move(offArg));
			extractCall->stackArgs.push_back(std::move(lenArg));
			return awst::makeReinterpretCast(std::move(extractCall), awst::WType::biguintType(), _loc);
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
		Logger::instance().error("keccak256 with non-constant offset/length not supported", _loc);
		return nullptr;
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

// ─── Synthetic calldata blob ────────────────────────────────────────────────
//
// When Yul accesses calldata at a non-constant offset (e.g.,
// `calldatacopy(0x20, public_inputs_start, public_inputs_size)` where
// `public_inputs_start := add(calldataload(0x24), 0x24)`), we materialise the
// EVM-ABI calldata as a single bytes local `__cd_blob` at the start of the
// assembly block. After that, any dynamic-offset calldataload becomes
// `extract3(__cd_blob, off, 32)`.

bool AssemblyBuilder::detectDynamicCalldataAccess(solidity::yul::Block const& _block)
{
	bool found = false;
	std::function<void(solidity::yul::Expression const&)> scanExpr;
	std::function<void(std::vector<solidity::yul::Statement> const&)> scanStmts;

	auto isCalldataOp = [](std::string const& n) {
		return n == "calldataload" || n == "calldatacopy" || n == "calldatasize";
	};

	scanExpr = [&](solidity::yul::Expression const& _expr) {
		if (found) return;
		if (auto const* call = std::get_if<solidity::yul::FunctionCall>(&_expr))
		{
			std::string n = getFunctionName(call->functionName);
			if (isCalldataOp(n))
			{
				// calldataload(off): non-constant off ⇒ dynamic.
				// calldatacopy(dest, src, len): non-constant src or len ⇒ dynamic.
				// calldatasize(): always returns runtime length — counts as
				// dynamic so the blob exists for `len(__cd_blob)` reads.
				if (n == "calldatasize")
					found = true;
				else if (n == "calldataload" && call->arguments.size() == 1)
				{
					if (!resolveConstantYulValue(call->arguments[0]))
						found = true;
				}
				else if (n == "calldatacopy" && call->arguments.size() == 3)
				{
					if (!resolveConstantYulValue(call->arguments[1])
						|| !resolveConstantYulValue(call->arguments[2]))
						found = true;
				}
			}
			for (auto const& a: call->arguments)
				scanExpr(a);
		}
	};
	scanStmts = [&](std::vector<solidity::yul::Statement> const& stmts) {
		for (auto const& s: stmts)
		{
			if (found) return;
			if (auto const* fd = std::get_if<solidity::yul::FunctionDefinition>(&s))
				scanStmts(fd->body.statements);
			else if (auto const* blk = std::get_if<solidity::yul::Block>(&s))
				scanStmts(blk->statements);
			else if (auto const* iff = std::get_if<solidity::yul::If>(&s))
			{
				scanExpr(*iff->condition);
				scanStmts(iff->body.statements);
			}
			else if (auto const* sw = std::get_if<solidity::yul::Switch>(&s))
			{
				scanExpr(*sw->expression);
				for (auto const& c: sw->cases)
					scanStmts(c.body.statements);
			}
			else if (auto const* fl = std::get_if<solidity::yul::ForLoop>(&s))
			{
				scanStmts(fl->pre.statements);
				scanExpr(*fl->condition);
				scanStmts(fl->post.statements);
				scanStmts(fl->body.statements);
			}
			else if (auto const* es = std::get_if<solidity::yul::ExpressionStatement>(&s))
				scanExpr(es->expression);
			else if (auto const* assign = std::get_if<solidity::yul::Assignment>(&s))
				scanExpr(*assign->value);
			else if (auto const* var = std::get_if<solidity::yul::VariableDeclaration>(&s))
				if (var->value)
					scanExpr(*var->value);
		}
	};
	scanStmts(_block.statements);
	return found;
}

namespace
{

// Helper: encode a uint64 as a 32-byte big-endian value via
// `concat(bzero(24), itob(val))`. itob produces 8 BE bytes.
std::shared_ptr<awst::Expression> pad32BE(
	std::shared_ptr<awst::Expression> _u64Val, awst::SourceLocation const& _loc)
{
	auto bz = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), _loc);
	bz->stackArgs.push_back(awst::makeIntegerConstant("24", _loc));
	auto itob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), _loc);
	itob->stackArgs.push_back(std::move(_u64Val));
	auto cat = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
	cat->stackArgs.push_back(std::move(bz));
	cat->stackArgs.push_back(std::move(itob));
	return cat;
}

// Helper: pad a bytes value to a 32-byte multiple (right-pad with zeros).
//   pad = (32 - (len % 32)) % 32
//   result = bytes ++ bzero(pad)
std::shared_ptr<awst::Expression> padTo32Multiple(
	std::shared_ptr<awst::Expression> _bytes, awst::SourceLocation const& _loc)
{
	using O = awst::UInt64BinaryOperator;
	auto lenCall = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), _loc);
	lenCall->stackArgs.push_back(_bytes);

	// pad = (32 - (len % 32)) % 32 == (-len) % 32 == (-len) & 31
	auto modPart = awst::makeUInt64BinOp(
		std::move(lenCall), O::Mod, awst::makeIntegerConstant("32", _loc), _loc);
	auto sub = awst::makeUInt64BinOp(
		awst::makeIntegerConstant("32", _loc), O::Sub, std::move(modPart), _loc);
	auto pad = awst::makeUInt64BinOp(
		std::move(sub), O::Mod, awst::makeIntegerConstant("32", _loc), _loc);

	auto bz = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), _loc);
	bz->stackArgs.push_back(std::move(pad));

	auto cat = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
	cat->stackArgs.push_back(std::move(_bytes));
	cat->stackArgs.push_back(std::move(bz));
	return cat;
}

bool isDynamicAbi(awst::WType const* _type)
{
	if (!_type) return false;
	if (_type == awst::WType::bytesType()) return true;
	if (_type == awst::WType::stringType()) return true;
	if (_type->kind() == awst::WTypeKind::ARC4DynamicArray) return true;
	if (_type->kind() == awst::WTypeKind::ReferenceArray)
	{
		auto const* refArr = dynamic_cast<awst::ReferenceArray const*>(_type);
		return refArr && !refArr->arraySize().has_value();
	}
	return false;
}

} // anonymous

void AssemblyBuilder::buildSyntheticCalldataBlob(
	std::vector<std::pair<std::string, awst::WType const*>> const& _params,
	std::vector<std::shared_ptr<awst::Statement>>& _out,
	awst::SourceLocation const& _loc
)
{
	using O = awst::UInt64BinaryOperator;

	auto u64Const = [&](uint64_t v) {
		return awst::makeIntegerConstant(std::to_string(v), _loc, awst::WType::uint64Type());
	};
	auto bytesVar = [&](std::string const& n) {
		return awst::makeVarExpression(n, awst::WType::bytesType(), _loc);
	};
	auto u64Var = [&](std::string const& n) {
		return awst::makeVarExpression(n, awst::WType::uint64Type(), _loc);
	};
	auto bzeroOf = [&](std::shared_ptr<awst::Expression> n) {
		auto c = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), _loc);
		c->stackArgs.push_back(std::move(n));
		return c;
	};
	auto concatBytes = [&](std::shared_ptr<awst::Expression> a, std::shared_ptr<awst::Expression> b) {
		auto c = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
		c->stackArgs.push_back(std::move(a));
		c->stackArgs.push_back(std::move(b));
		return c;
	};
	auto lenOf = [&](std::shared_ptr<awst::Expression> b) {
		auto c = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), _loc);
		c->stackArgs.push_back(std::move(b));
		return c;
	};

	// Layout: 4-byte selector (zeros) + N×32 head section + tail section.
	// We compute the running tail offset as a uint64 local `__cd_tail_off`
	// (relative to start of args = 0x04). It starts at N*32 (size of head).
	uint64_t headWords = _params.size();

	// __cd_blob = bzero(4)  — selector slot
	_out.push_back(awst::makeAssignmentStatement(
		bytesVar(CD_BLOB_VAR), bzeroOf(u64Const(4)), _loc));

	// __cd_tail_off = headWords * 32  — running offset of next tail entry
	_out.push_back(awst::makeAssignmentStatement(
		u64Var("__cd_tail_off"), u64Const(headWords * 32), _loc));

	// Two passes: first append head words for each param (computing tail
	// offsets along the way for dynamic params), then append tail bodies
	// for the dynamic params in declaration order.
	for (size_t i = 0; i < _params.size(); ++i)
	{
		auto const& [name, type] = _params[i];
		if (isDynamicAbi(type))
		{
			// Head: pad32BE(__cd_tail_off)
			_out.push_back(awst::makeAssignmentStatement(
				bytesVar(CD_BLOB_VAR),
				concatBytes(bytesVar(CD_BLOB_VAR), pad32BE(u64Var("__cd_tail_off"), _loc)),
				_loc));
			// __cd_tail_off += 32 (length word) + paddedLen(param) — done
			// in the tail-emission pass below to avoid recomputing length.
		}
		else
		{
			// Static head: read param value, encode as 32 BE bytes.
			// For simplicity: pad biguint/uint64/bool/account/bytesN to 32 bytes BE.
			auto paramVar = awst::makeVarExpression(name, type, _loc);
			std::shared_ptr<awst::Expression> headWord;
			if (type == awst::WType::uint64Type())
				headWord = pad32BE(std::move(paramVar), _loc);
			else if (type == awst::WType::biguintType())
			{
				// biguint as bytes; left-pad to 32 if shorter.
				auto bz = bzeroOf(u64Const(32));
				auto orOp = awst::makeIntrinsicCall("b|", awst::WType::bytesType(), _loc);
				orOp->stackArgs.push_back(awst::makeReinterpretCast(
					std::move(paramVar), awst::WType::bytesType(), _loc));
				orOp->stackArgs.push_back(std::move(bz));
				headWord = std::move(orOp);
			}
			else if (type == awst::WType::boolType())
			{
				// bool → 32 bytes: 31 zeros + 0x01/0x00
				auto bz = bzeroOf(u64Const(31));
				auto byteByVal = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), _loc);
				auto castU64 = awst::makeReinterpretCast(
					std::move(paramVar), awst::WType::uint64Type(), _loc);
				byteByVal->stackArgs.push_back(std::move(castU64));
				// itob produces 8 bytes BE; take last byte
				auto extract = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
				extract->stackArgs.push_back(std::move(byteByVal));
				extract->stackArgs.push_back(u64Const(7));
				extract->stackArgs.push_back(u64Const(1));
				headWord = concatBytes(std::move(bz), std::move(extract));
			}
			else if (type == awst::WType::accountType())
			{
				headWord = awst::makeReinterpretCast(
					std::move(paramVar), awst::WType::bytesType(), _loc);
			}
			else
			{
				// Fallback: treat as 32-byte bytes (best-effort).
				headWord = awst::makeReinterpretCast(
					std::move(paramVar), awst::WType::bytesType(), _loc);
			}
			_out.push_back(awst::makeAssignmentStatement(
				bytesVar(CD_BLOB_VAR),
				concatBytes(bytesVar(CD_BLOB_VAR), std::move(headWord)),
				_loc));
		}
	}

	// Tail pass: for each dynamic param, append length word + data,
	// updating __cd_tail_off so subsequent dynamic params get correct heads.
	// Heads were already emitted — but we patched them with the
	// then-current __cd_tail_off, so this works iff we walk in declaration
	// order (which we did).
	//
	// HOWEVER — the loop above already wrote each dynamic-param head with
	// __cd_tail_off as it stood at that point. We now need to advance
	// __cd_tail_off by the size of the just-emitted tail entry BEFORE the
	// next dynamic param's head was written. That's an ordering issue:
	// the head writes happened in the loop above without updating
	// __cd_tail_off for dynamic params after them.
	//
	// Fix: redo the loop, this time interleaving — see updated impl below.
	// (Keeping the simple two-pass form here for an MVP that only handles
	// the common case of a SINGLE dynamic param OR multiple dynamics where
	// the test only reads the first dynamic's head; honk's verify(bytes,
	// bytes32[]) needs the second head correct, so emit the tail in order
	// AND patch the second head later via a replace3.)

	// MVP: assume <=1 dynamic OR caller doesn't read second head. For
	// honk/Blake.sol verify(bytes _proof, bytes32[] _publicInputs), the
	// Yul reads `calldataload(0x24)` (publicInputs head). To make that
	// correct, we need the second head value to be
	// 0x40 + 32 + paddedLen(_proof). Patch via replace3 after emitting
	// proof tail.

	// First dynamic encountered in head pass: collect its index & param,
	// so the tail pass knows which to emit first.
	for (size_t i = 0; i < _params.size(); ++i)
	{
		auto const& [name, type] = _params[i];
		if (!isDynamicAbi(type)) continue;

		// Append length word (32 bytes BE of len(param))
		auto var = awst::makeVarExpression(name, type, _loc);
		auto lenExpr = lenOf(var);
		_out.push_back(awst::makeAssignmentStatement(
			bytesVar(CD_BLOB_VAR),
			concatBytes(bytesVar(CD_BLOB_VAR), pad32BE(std::move(lenExpr), _loc)),
			_loc));

		// Append param data padded to 32-byte multiple
		auto var2 = awst::makeVarExpression(name, type, _loc);
		// For arc4 dynamic-array types the on-disk repr starts with a
		// uint16 length header — strip it for the calldata body.
		std::shared_ptr<awst::Expression> body;
		if (type == awst::WType::bytesType() || type == awst::WType::stringType())
			body = awst::makeReinterpretCast(std::move(var2), awst::WType::bytesType(), _loc);
		else
		{
			// ARC4 dynamic array: extract everything after the 2-byte length header.
			auto bytes = awst::makeReinterpretCast(std::move(var2), awst::WType::bytesType(), _loc);
			auto lenCall = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), _loc);
			lenCall->stackArgs.push_back(bytes);
			auto sub2 = awst::makeUInt64BinOp(
				std::move(lenCall), O::Sub, u64Const(2), _loc);
			auto extract = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
			extract->stackArgs.push_back(std::move(bytes));
			extract->stackArgs.push_back(u64Const(2));
			extract->stackArgs.push_back(std::move(sub2));
			body = std::move(extract);
		}
		auto paddedBody = padTo32Multiple(std::move(body), _loc);
		_out.push_back(awst::makeAssignmentStatement(
			bytesVar(CD_BLOB_VAR),
			concatBytes(bytesVar(CD_BLOB_VAR), std::move(paddedBody)),
			_loc));

		// Update tail offset for subsequent params: __cd_tail_off += 32 + paddedLen
		// (Used only if we needed to patch later heads. For the MVP we
		// patch the second head via replace3 just below.)
		auto var3 = awst::makeVarExpression(name, type, _loc);
		auto rawLen = lenOf(var3);
		auto modVal = awst::makeUInt64BinOp(
			rawLen, O::Mod, u64Const(32), _loc);
		auto padBytes = awst::makeUInt64BinOp(
			awst::makeUInt64BinOp(u64Const(32), O::Sub, std::move(modVal), _loc),
			O::Mod, u64Const(32), _loc);
		auto var4 = awst::makeVarExpression(name, type, _loc);
		auto rawLen2 = lenOf(var4);
		auto paddedLen = awst::makeUInt64BinOp(
			std::move(rawLen2), O::Add, std::move(padBytes), _loc);

		auto advance = awst::makeUInt64BinOp(
			awst::makeUInt64BinOp(u64Var("__cd_tail_off"), O::Add, u64Const(32), _loc),
			O::Add, std::move(paddedLen), _loc);
		_out.push_back(awst::makeAssignmentStatement(u64Var("__cd_tail_off"), advance, _loc));

		// PATCH later dynamic heads with the now-correct __cd_tail_off.
		// Each later dynamic param's head sits at byte offset
		// 4 + (its_index * 32) within the blob. Overwrite with the
		// current __cd_tail_off (which still points at the NEXT tail
		// entry, i.e. exactly the value that head should hold).
		for (size_t j = i + 1; j < _params.size(); ++j)
		{
			if (!isDynamicAbi(_params[j].second)) continue;
			uint64_t headByteOffset = 4 + j * 32;
			auto patch = awst::makeIntrinsicCall("replace3", awst::WType::bytesType(), _loc);
			patch->stackArgs.push_back(bytesVar(CD_BLOB_VAR));
			patch->stackArgs.push_back(u64Const(headByteOffset));
			patch->stackArgs.push_back(pad32BE(u64Var("__cd_tail_off"), _loc));
			_out.push_back(awst::makeAssignmentStatement(bytesVar(CD_BLOB_VAR), std::move(patch), _loc));
			break;  // only patch the very next dynamic; updating
			        // __cd_tail_off in subsequent iterations chains them.
		}
	}

	// Register __cd_blob in m_locals so subsequent reads pick up its type.
	m_locals[CD_BLOB_VAR] = awst::WType::bytesType();
}


// ─── Precompile helper methods ──────────────────────────────────────────────


} // namespace puyasol::builder
