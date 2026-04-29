/// @file MemoryOps.cpp
/// Memory operations: mload, mstore, handleReturn, tryHandleBytesMemoryRead.
/// Uses scratch-slot-backed bytes blob for EVM memory simulation.

#include "builder/assembly/AssemblyBuilder.h"
#include "Logger.h"

#include <sstream>

namespace puyasol::builder
{

std::shared_ptr<awst::Expression> AssemblyBuilder::handleMload(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	if (_args.size() != 1)
	{
		Logger::instance().error("mload requires 1 argument", _loc);
		return nullptr;
	}

	// First check calldata map for constant offsets (function parameters)
	auto constOffset = resolveConstantOffset(_args[0]);
	if (constOffset)
	{
		auto cdIt = m_calldataMap.find(*constOffset);
		if (cdIt != m_calldataMap.end())
		{
			auto const& elem = cdIt->second;
			auto base = awst::makeVarExpression(elem.paramName, m_locals.count(elem.paramName)
				? m_locals[elem.paramName]
				: awst::WType::biguintType(), _loc);

			return accessFlatElement(std::move(base), elem.paramType, elem.flatIndex, _loc);
		}
	}

	// Read 32 bytes from the memory blob at the given offset.
	// extract3(__evm_memory, offset, 32) → cast to biguint
	auto offsetU64 = offsetToUint64(_args[0], _loc);

	auto len32 = awst::makeIntegerConstant("32", _loc);

	auto extract = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
	extract->stackArgs.push_back(memoryVar(_loc));
	extract->stackArgs.push_back(std::move(offsetU64));
	extract->stackArgs.push_back(std::move(len32));

	// Cast bytes → biguint (mload returns uint256)
	auto result = awst::makeReinterpretCast(std::move(extract), awst::WType::biguintType(), _loc);
	return result;
}

std::shared_ptr<awst::Expression> AssemblyBuilder::tryHandleBytesMemoryLength(
	solidity::yul::Expression const& _addrExpr,
	awst::SourceLocation const& _loc
)
{
	// Match: mload(bytes_var) — the EVM idiom for reading the 32-byte
	// length header that precedes the bytes payload in memory layout.
	// AVM has no length header (bytes are length-prefixed only when
	// stored as ARC4); the runtime length is len(bytes_var).
	auto* paramId = std::get_if<solidity::yul::Identifier>(&_addrExpr);
	if (!paramId)
		return nullptr;

	std::string paramName = paramId->name.str();
	auto paramIt = m_locals.find(paramName);
	if (paramIt == m_locals.end())
		return nullptr;

	auto const* paramType = paramIt->second;
	bool isBytesLike = paramType == awst::WType::bytesType()
		|| paramType == awst::WType::stringType();
	// Memory aggregates (arrays, structs) live as ARC4-encoded bytes at
	// runtime; their `mload(var)` would also be a length read, but their
	// length encoding is different (header at offset 0). For now only
	// handle plain bytes/string.
	if (!isBytesLike)
		return nullptr;

	Logger::instance().debug(
		"mload bytes-length: len(" + paramName + ")", _loc
	);

	auto paramVar = awst::makeVarExpression(paramName, paramType, _loc);
	auto len = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), _loc);
	len->stackArgs.push_back(std::move(paramVar));
	// mload returns uint256 in EVM. Convert uint64 → biguint via itob.
	auto itob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), _loc);
	itob->stackArgs.push_back(std::move(len));
	return awst::makeReinterpretCast(std::move(itob), awst::WType::biguintType(), _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::tryHandleBytesMemoryRead(
	solidity::yul::Expression const& _addrExpr,
	awst::SourceLocation const& _loc
)
{
	// Match: mload(add(add(bytes_param, 32), offset))
	// or:    mload(add(offset, add(bytes_param, 32)))
	//
	// This is the standard Solidity pattern for reading 32 bytes from a
	// bytes memory parameter at a variable byte offset.
	// In EVM: data_ptr + 32 (skip length header) + offset → mload → 32 bytes
	// In AVM: extract3(data, offset, 32) — bytes have no length header
	//
	// Also: simpler `mload(add(bytes_param, K))` for K compile-time constant
	// >= 32. Solady's manual r/s/v parsing in `_verifyECDSASignature` (the
	// AVM-PORT-ADAPTATION) does exactly this with K = 0x20, 0x40, 0x60. Treat
	// as `extract3(param, K-32, min(32, len(param)-(K-32)))` — read up to
	// 32 bytes from the data offset, clipped at the bytes_var's actual
	// length. Out-of-bounds reads (e.g. K=0x60 on a 65-byte signature, where
	// only 1 byte is in bounds) return a shorter slice; downstream `byte()`
	// extraction reads only what it needs anyway.

	auto* outerAdd = std::get_if<solidity::yul::FunctionCall>(&_addrExpr);
	if (!outerAdd || getFunctionName(outerAdd->functionName) != "add"
		|| outerAdd->arguments.size() != 2)
		return nullptr;

	// Single-add fast path: mload(add(bytes_var, K)) where K is a constant
	// >= 32. Try this first — if it matches, the double-add path is moot.
	//
	// Match by *type identity*, not by which arg "looks constant": parameter
	// names are entered into m_localConstants (their EVM calldata offsets),
	// so `resolveConstantYulValue(signature)` returns 68 even though signature
	// is a runtime bytes variable. We have to look at m_locals first.
	{
		auto matchBytesVar = [&](solidity::yul::Expression const& _e)
			-> std::pair<solidity::yul::Identifier const*, awst::WType const*> {
			auto const* id = std::get_if<solidity::yul::Identifier>(&_e);
			if (!id) return {nullptr, nullptr};
			auto it = m_locals.find(id->name.str());
			if (it == m_locals.end()) return {nullptr, nullptr};
			if (it->second == awst::WType::bytesType()
				|| it->second == awst::WType::stringType())
				return {id, it->second};
			return {nullptr, nullptr};
		};

		solidity::yul::Identifier const* singleVarId = nullptr;
		awst::WType const* paramType = nullptr;
		uint64_t singleK = 0;

		auto [id0, t0] = matchBytesVar(outerAdd->arguments[0]);
		auto [id1, t1] = matchBytesVar(outerAdd->arguments[1]);
		if (id0)
		{
			auto k = resolveConstantYulValue(outerAdd->arguments[1]);
			if (k && *k >= 32)
			{
				singleVarId = id0;
				paramType = t0;
				singleK = *k;
			}
		}
		else if (id1)
		{
			auto k = resolveConstantYulValue(outerAdd->arguments[0]);
			if (k && *k >= 32)
			{
				singleVarId = id1;
				paramType = t1;
				singleK = *k;
			}
		}

		if (singleVarId)
		{
			std::string name = singleVarId->name.str();
			uint64_t off = singleK - 32;

			Logger::instance().debug(
				"mload(add(" + name + ", " + std::to_string(singleK)
					+ ")) → extract3(" + name + ", " + std::to_string(off)
					+ ", min(32, len-" + std::to_string(off) + "))",
				_loc
			);

			// Compute clip = max(0, min(32, len(name) - off))
			// via runtime arithmetic, so over-extracting on a short
			// bytes_var is safe.
			auto buildLenMinusOff = [&]() {
				auto v = awst::makeVarExpression(name, paramType, _loc);
				auto l = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), _loc);
				l->stackArgs.push_back(std::move(v));
				return awst::makeUInt64BinOp(
					std::move(l),
					awst::UInt64BinaryOperator::Sub,
					awst::makeIntegerConstant(std::to_string(off), _loc),
					_loc
				);
			};
			auto cmp = awst::makeNumericCompare(
				buildLenMinusOff(),
				awst::NumericComparison::Gt,
				awst::makeIntegerConstant("32", _loc),
				_loc
			);
			auto clip = std::make_shared<awst::ConditionalExpression>();
			clip->sourceLocation = _loc;
			clip->wtype = awst::WType::uint64Type();
			clip->condition = std::move(cmp);
			clip->trueExpr = awst::makeIntegerConstant("32", _loc);
			clip->falseExpr = buildLenMinusOff();

			auto base = awst::makeVarExpression(name, paramType, _loc);
			auto offConst = awst::makeIntegerConstant(std::to_string(off), _loc);

			auto extract = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
			extract->stackArgs.push_back(std::move(base));
			extract->stackArgs.push_back(std::move(offConst));
			extract->stackArgs.push_back(std::move(clip));

			// Right-pad to 32 bytes by `concat(extract, bzero(32-extract.len))`
			// so the result has the EVM-mload shape (32-byte-wide). Solady
			// uses `byte(0, mload(...))` to take the first byte; padding
			// preserves correctness whether the read was full or partial.
			auto bzero32 = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), _loc);
			bzero32->stackArgs.push_back(awst::makeIntegerConstant("32", _loc));
			auto concat = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
			concat->stackArgs.push_back(std::move(extract));
			concat->stackArgs.push_back(std::move(bzero32));
			// Take the first 32 bytes of (extract ++ bzero(32))
			auto first32 = awst::makeIntrinsicCall("extract", awst::WType::bytesType(), _loc);
			first32->immediates.push_back(0);
			first32->immediates.push_back(32);
			first32->stackArgs.push_back(std::move(concat));

			return awst::makeReinterpretCast(
				std::move(first32), awst::WType::biguintType(), _loc);
		}
	}

	// One arg of outer add should be add(bytes_param, 32), the other is the offset
	solidity::yul::FunctionCall const* innerAdd = nullptr;
	solidity::yul::Expression const* offsetExprYul = nullptr;

	auto* call0 = std::get_if<solidity::yul::FunctionCall>(&outerAdd->arguments[0]);
	auto* call1 = std::get_if<solidity::yul::FunctionCall>(&outerAdd->arguments[1]);

	if (call0 && getFunctionName(call0->functionName) == "add" && call0->arguments.size() == 2)
	{
		innerAdd = call0;
		offsetExprYul = &outerAdd->arguments[1];
	}
	else if (call1 && getFunctionName(call1->functionName) == "add" && call1->arguments.size() == 2)
	{
		innerAdd = call1;
		offsetExprYul = &outerAdd->arguments[0];
	}

	if (!innerAdd)
		return nullptr;

	// Inner add should have: (bytes_param, 32) or (32, bytes_param)
	solidity::yul::Expression const* paramExpr = nullptr;

	auto val1 = resolveConstantYulValue(innerAdd->arguments[1]);
	if (val1 && *val1 == 32)
	{
		paramExpr = &innerAdd->arguments[0];
	}
	else
	{
		auto val0 = resolveConstantYulValue(innerAdd->arguments[0]);
		if (val0 && *val0 == 32)
			paramExpr = &innerAdd->arguments[1];
	}

	if (!paramExpr)
		return nullptr;

	// param must be an Identifier referencing a bytes/string parameter
	auto* paramId = std::get_if<solidity::yul::Identifier>(paramExpr);
	if (!paramId)
		return nullptr;

	std::string paramName = paramId->name.str();
	auto paramIt = m_locals.find(paramName);
	if (paramIt == m_locals.end())
		return nullptr;

	auto* paramType = paramIt->second;
	if (paramType != awst::WType::bytesType() && paramType != awst::WType::stringType())
		return nullptr;

	// Pattern matched! Generate: extract3(param, btoi(offset), 32) → cast to biguint

	Logger::instance().debug(
		"mload bytes memory read: extract3(" + paramName + ", offset, 32)", _loc
	);

	// Build param reference
	auto paramVar = awst::makeVarExpression(paramName, paramType, _loc);

	// Translate the dynamic offset and convert biguint → uint64
	auto offsetExpr = buildExpression(*offsetExprYul);

	auto offsetBytes = awst::makeReinterpretCast(offsetExpr, awst::WType::bytesType(), _loc);

	auto offsetU64 = awst::makeIntrinsicCall("btoi", awst::WType::uint64Type(), _loc);
	offsetU64->stackArgs.push_back(std::move(offsetBytes));

	// Length: 32 bytes
	auto lenArg = awst::makeIntegerConstant("32", _loc);

	// extract3(param, offset, 32)
	auto extract = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
	extract->stackArgs.push_back(std::move(paramVar));
	extract->stackArgs.push_back(std::move(offsetU64));
	extract->stackArgs.push_back(std::move(lenArg));

	// Cast bytes → biguint (mload returns uint256)
	auto result = awst::makeReinterpretCast(std::move(extract), awst::WType::biguintType(), _loc);

	return result;
}

bool AssemblyBuilder::tryHandleBytesMemoryWrite(
	solidity::yul::FunctionCall const& _call,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	// Match: mstore(add(bytes_var, K), value)
	// or:    mstore(add(K, bytes_var), value)
	// where K is a compile-time constant >= 32.
	//
	// EVM layout: bytes memory x has layout [length(32)][data...] at address x.
	// add(x, K) for K >= 32 points (K - 32) bytes into the data region.
	// mstore writes 32 bytes there, optionally overflowing into the next
	// allocation if K - 32 + 32 > len(x).
	//
	// In AVM: x is raw bytes (no length header). We splice up to 32 bytes of
	// `value` into x at offset (K - 32). The splice is clipped at len(x), so
	// trailing bytes that would write past the end are dropped (matching the
	// caller's intent — Solidity allocations sized exactly for the data).

	if (_call.arguments.size() != 2)
		return false;

	// First arg must be add(bytes_var, K) or add(K, bytes_var) with K >= 32
	auto* addCall = std::get_if<solidity::yul::FunctionCall>(&_call.arguments[0]);
	if (!addCall || getFunctionName(addCall->functionName) != "add"
		|| addCall->arguments.size() != 2)
		return false;

	auto val0 = resolveConstantYulValue(addCall->arguments[0]);
	auto val1 = resolveConstantYulValue(addCall->arguments[1]);

	solidity::yul::Expression const* varExpr = nullptr;
	uint64_t kConst = 0;

	if (val1 && *val1 >= 32)
	{
		varExpr = &addCall->arguments[0];
		kConst = *val1;
	}
	else if (val0 && *val0 >= 32)
	{
		varExpr = &addCall->arguments[1];
		kConst = *val0;
	}
	else
	{
		return false;
	}

	// The variable must be an Identifier referencing a bytes/string local
	auto* varId = std::get_if<solidity::yul::Identifier>(varExpr);
	if (!varId)
		return false;

	std::string varName = varId->name.str();
	auto localIt = m_locals.find(varName);
	if (localIt == m_locals.end())
		return false;

	auto* varType = localIt->second;
	if (varType != awst::WType::bytesType() && varType != awst::WType::stringType())
		return false;

	// Pattern matched! Translate the value expression.
	auto valueExpr = buildExpression(_call.arguments[1]);
	uint64_t off = kConst - 32;

	Logger::instance().debug(
		"mstore bytes memory write: splice into '" + varName + "' at offset "
			+ std::to_string(off), _loc
	);

	// Build: x = replace3(x, off, extract3(pad32(value), 0, write_len))
	// where write_len = min(32, len(x) - off).

	// pad32(value) — 32 bytes
	auto padded = padTo32Bytes(ensureBiguint(valueExpr, _loc), _loc);

	// avail = len(x) - off  (uint64 sub; underflows to a huge value if
	// off > len(x), which then caps at 32 below — replace3 will panic on the
	// out-of-bounds write, mirroring AVM bounds-check semantics).
	auto buildAvail = [&]() {
		auto v = awst::makeVarExpression(varName, varType, _loc);
		auto l = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), _loc);
		l->stackArgs.push_back(std::move(v));
		auto offConst = awst::makeIntegerConstant(std::to_string(off), _loc);
		return awst::makeUInt64BinOp(
			std::move(l),
			awst::UInt64BinaryOperator::Sub,
			std::move(offConst),
			_loc
		);
	};

	// write_len = (avail > 32) ? 32 : avail
	auto cmp = awst::makeNumericCompare(
		buildAvail(),
		awst::NumericComparison::Gt,
		awst::makeIntegerConstant("32", _loc),
		_loc
	);
	auto writeLen = std::make_shared<awst::ConditionalExpression>();
	writeLen->sourceLocation = _loc;
	writeLen->wtype = awst::WType::uint64Type();
	writeLen->condition = std::move(cmp);
	writeLen->trueExpr = awst::makeIntegerConstant("32", _loc);
	writeLen->falseExpr = buildAvail();

	// splice = extract3(pad32(value), 0, write_len)
	auto extract = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
	extract->stackArgs.push_back(std::move(padded));
	extract->stackArgs.push_back(awst::makeIntegerConstant("0", _loc));
	extract->stackArgs.push_back(std::move(writeLen));

	// x = replace3(x, off, splice)
	auto bufRef = awst::makeVarExpression(varName, varType, _loc);
	auto offForReplace = awst::makeIntegerConstant(std::to_string(off), _loc);
	auto replace = awst::makeIntrinsicCall("replace3", awst::WType::bytesType(), _loc);
	replace->stackArgs.push_back(std::move(bufRef));
	replace->stackArgs.push_back(std::move(offForReplace));
	replace->stackArgs.push_back(std::move(extract));

	// Cast if needed for string type
	std::shared_ptr<awst::Expression> newValue = std::move(replace);
	if (varType == awst::WType::stringType())
	{
		auto cast = awst::makeReinterpretCast(std::move(newValue), awst::WType::stringType(), _loc);
		newValue = std::move(cast);
	}

	auto target = awst::makeVarExpression(varName, varType, _loc);
	auto assign = awst::makeAssignmentStatement(std::move(target), std::move(newValue), _loc);
	_out.push_back(std::move(assign));

	return true;
}

void AssemblyBuilder::handleMstore(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	if (_args.size() != 2)
	{
		Logger::instance().error("mstore requires 2 arguments", _loc);
		return;
	}

	// Track last mstore value for dynamic-length keccak256 patterns
	m_lastMstoreValue = _args[1];

	// Track constant values stored to memory (especially free memory pointer)
	auto constOffset = resolveConstantOffset(_args[0]);
	if (constOffset)
	{
		auto storedVal = resolveConstantOffset(_args[1]);
		if (storedVal)
		{
			// Track by offset for resolveConstantOffset to find later
			std::string varName = "mem_0x" + ([&] {
				std::ostringstream oss;
				oss << std::hex << *constOffset;
				return oss.str();
			})();
			m_localConstants[varName] = *storedVal;
		}
	}

	// Write 32 bytes into the memory blob at the given offset.
	// __evm_memory = replace3(__evm_memory, offset, pad32(value))
	auto offsetU64 = offsetToUint64(_args[0], _loc);
	auto padded = padTo32Bytes(ensureBiguint(_args[1], _loc), _loc);

	auto replace = awst::makeIntrinsicCall("replace3", awst::WType::bytesType(), _loc);
	replace->stackArgs.push_back(memoryVar(_loc));
	replace->stackArgs.push_back(std::move(offsetU64));
	replace->stackArgs.push_back(std::move(padded));

	assignMemoryVar(std::move(replace), _loc, _out);
}

void AssemblyBuilder::handleMstore8(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	if (_args.size() != 2)
	{
		Logger::instance().error("mstore8 requires 2 arguments", _loc);
		return;
	}

	// mstore8(ptr, value): write the low 8 bits of value as a single byte
	// at memory[ptr]. Pad the value to 32 bytes and extract byte[31] (the
	// low byte), then replace3 one byte at the target offset in the blob.
	auto offsetU64 = offsetToUint64(_args[0], _loc);
	auto padded = padTo32Bytes(ensureBiguint(_args[1], _loc), _loc);

	auto start = awst::makeIntegerConstant("31", _loc);
	auto len = awst::makeIntegerConstant("1", _loc);

	auto lowByte = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
	lowByte->stackArgs.push_back(std::move(padded));
	lowByte->stackArgs.push_back(std::move(start));
	lowByte->stackArgs.push_back(std::move(len));

	auto replace = awst::makeIntrinsicCall("replace3", awst::WType::bytesType(), _loc);
	replace->stackArgs.push_back(memoryVar(_loc));
	replace->stackArgs.push_back(std::move(offsetU64));
	replace->stackArgs.push_back(std::move(lowByte));

	assignMemoryVar(std::move(replace), _loc, _out);
}

void AssemblyBuilder::handleReturn(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	if (_args.size() != 2)
	{
		Logger::instance().error("return requires 2 arguments", _loc);
		return;
	}

	// return(offset, size): Return the value stored at memory[offset]

	// Void Solidity function with assembly return(offset, size) — EVM pattern
	// that bypasses ABI encoding. On AVM, emit the data as a structured log
	// so callers can read it from transaction logs.
	if (!m_returnType || m_returnType == awst::WType::voidType())
	{
		auto returnOffset = resolveConstantOffset(_args[0]);
		auto returnSize = resolveConstantOffset(_args[1]);

		if (!returnOffset || !returnSize || *returnSize == 0)
		{
			// `assembly { return(_, 0) }` halts the entire program with
			// success. Emit the raw AVM `return 1` intrinsic so puya lowers
			// it to an unconditional program-exit (not just a subroutine
			// return). Needed for Yul helpers that use the EVM `return`
			// opcode as a hard exit from inside a nested call.
			flushMemoryToScratch(_loc, _out);

			auto returnOp = awst::makeIntrinsicCall("return", awst::WType::voidType(), _loc);
			returnOp->stackArgs.push_back(awst::makeBoolConstant(true, _loc));

			auto exitStmt = awst::makeExpressionStatement(std::move(returnOp), _loc);
			_out.push_back(std::move(exitStmt));
			m_haltEmitted = true;
			return;
		}

		Logger::instance().warning(
			"assembly return() in void function — emitting " +
			std::to_string(*returnSize) + " bytes as structured log", _loc
		);

		// Read the return region from the memory blob: extract3(blob, offset, size)
		auto offsetU64 = awst::makeIntegerConstant(std::to_string(*returnOffset), _loc);

		auto sizeU64 = awst::makeIntegerConstant(std::to_string(*returnSize), _loc);

		auto extract = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
		extract->stackArgs.push_back(memoryVar(_loc));
		extract->stackArgs.push_back(std::move(offsetU64));
		extract->stackArgs.push_back(std::move(sizeU64));

		// log(data) — emit the raw bytes as a transaction log
		auto logCall = awst::makeIntrinsicCall("log", awst::WType::voidType(), _loc);
		logCall->stackArgs.push_back(std::move(extract));

		auto logStmt = awst::makeExpressionStatement(std::move(logCall), _loc);
		_out.push_back(std::move(logStmt));

		// Flush and return void
		flushMemoryToScratch(_loc, _out);
		auto ret = awst::makeReturnStatement(nullptr, _loc);
		_out.push_back(std::move(ret));
		return;
	}

	auto offset = resolveConstantOffset(_args[0]);
	if (!offset)
	{
		Logger::instance().error(
			"return with non-constant offset not supported", _loc
		);
		return;
	}

	// Read from the memory blob at this offset
	std::shared_ptr<awst::Expression> returnValue = readMemSlot(*offset, _loc);

	// Convert to bool if the function's return type is bool
	if (m_returnType == awst::WType::boolType()
		&& returnValue->wtype != awst::WType::boolType())
	{
		auto zero = awst::makeIntegerConstant("0", _loc, awst::WType::biguintType());

		auto cmp = awst::makeNumericCompare(std::move(returnValue), awst::NumericComparison::Ne, std::move(zero), _loc);
		returnValue = std::move(cmp);
	}

	// When the function returns an array type but assembly produces a scalar,
	// the assembly was manually building ABI-encoded memory (EVM-specific).
	// Return an empty array as fallback since the memory ops don't translate.
	if (m_returnType && dynamic_cast<awst::ReferenceArray const*>(m_returnType)
		&& !dynamic_cast<awst::ReferenceArray const*>(returnValue->wtype))
	{
		Logger::instance().warning(
			"assembly return produces scalar but function returns array; "
			"returning empty array (EVM memory layout not translatable)", _loc
		);
		auto emptyArr = std::make_shared<awst::NewArray>();
		emptyArr->sourceLocation = _loc;
		emptyArr->wtype = m_returnType;
		returnValue = std::move(emptyArr);
	}

	// Flush memory blob to scratch before returning
	flushMemoryToScratch(_loc, _out);

	auto ret = awst::makeReturnStatement(std::move(returnValue), _loc);
	_out.push_back(std::move(ret));
}

// ─── Statement translation ─────────────────────────────────────────────────


} // namespace puyasol::builder
