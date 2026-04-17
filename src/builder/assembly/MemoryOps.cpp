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

	auto extract = std::make_shared<awst::IntrinsicCall>();
	extract->sourceLocation = _loc;
	extract->wtype = awst::WType::bytesType();
	extract->opCode = "extract3";
	extract->stackArgs.push_back(memoryVar(_loc));
	extract->stackArgs.push_back(std::move(offsetU64));
	extract->stackArgs.push_back(std::move(len32));

	// Cast bytes → biguint (mload returns uint256)
	auto result = awst::makeReinterpretCast(std::move(extract), awst::WType::biguintType(), _loc);
	return result;
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

	auto* outerAdd = std::get_if<solidity::yul::FunctionCall>(&_addrExpr);
	if (!outerAdd || getFunctionName(outerAdd->functionName) != "add"
		|| outerAdd->arguments.size() != 2)
		return nullptr;

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

	auto offsetU64 = std::make_shared<awst::IntrinsicCall>();
	offsetU64->sourceLocation = _loc;
	offsetU64->wtype = awst::WType::uint64Type();
	offsetU64->opCode = "btoi";
	offsetU64->stackArgs.push_back(std::move(offsetBytes));

	// Length: 32 bytes
	auto lenArg = awst::makeIntegerConstant("32", _loc);

	// extract3(param, offset, 32)
	auto extract = std::make_shared<awst::IntrinsicCall>();
	extract->sourceLocation = _loc;
	extract->wtype = awst::WType::bytesType();
	extract->opCode = "extract3";
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
	// Match: mstore(add(bytes_var, 32), value)
	// or:    mstore(add(32, bytes_var), value)
	//
	// In EVM: bytes memory x has layout [length(32)][data...] at address x.
	// add(x, 32) points to the data region. mstore writes 32 bytes there.
	// The variable's length is unchanged, so on return only len(x) bytes matter.
	//
	// In AVM: x is raw bytes (no length header). We overwrite x's content
	// with the first len(x) bytes of the 32-byte value.

	if (_call.arguments.size() != 2)
		return false;

	// First arg must be add(bytes_var, 32) or add(32, bytes_var)
	auto* addCall = std::get_if<solidity::yul::FunctionCall>(&_call.arguments[0]);
	if (!addCall || getFunctionName(addCall->functionName) != "add"
		|| addCall->arguments.size() != 2)
		return false;

	// Find which arg is 32 and which is the bytes variable
	solidity::yul::Expression const* varExpr = nullptr;

	auto val0 = resolveConstantYulValue(addCall->arguments[0]);
	auto val1 = resolveConstantYulValue(addCall->arguments[1]);

	if (val1 && *val1 == 32)
		varExpr = &addCall->arguments[0];
	else if (val0 && *val0 == 32)
		varExpr = &addCall->arguments[1];
	else
		return false;

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

	Logger::instance().debug(
		"mstore bytes memory write: replacing content of '" + varName + "'", _loc
	);

	// Build: x = extract3(pad32(value), 0, len(x))
	// This overwrites x with the first len(x) bytes of the 32-byte value.

	// Reference to the variable
	auto varRef = awst::makeVarExpression(varName, varType, _loc);

	// len(x)
	auto lenCall = std::make_shared<awst::IntrinsicCall>();
	lenCall->sourceLocation = _loc;
	lenCall->wtype = awst::WType::uint64Type();
	lenCall->opCode = "len";
	lenCall->stackArgs.push_back(varRef);

	// pad32(value) — get the 32 bytes representation
	auto padded = padTo32Bytes(ensureBiguint(valueExpr, _loc), _loc);

	// extract3(padded, 0, len(x))
	auto zero = awst::makeIntegerConstant("0", _loc);

	auto extract = std::make_shared<awst::IntrinsicCall>();
	extract->sourceLocation = _loc;
	extract->wtype = awst::WType::bytesType();
	extract->opCode = "extract3";
	extract->stackArgs.push_back(std::move(padded));
	extract->stackArgs.push_back(std::move(zero));
	extract->stackArgs.push_back(std::move(lenCall));

	// Cast if needed for string type
	std::shared_ptr<awst::Expression> newValue = std::move(extract);
	if (varType == awst::WType::stringType())
	{
		auto cast = awst::makeReinterpretCast(std::move(newValue), awst::WType::stringType(), _loc);
		newValue = std::move(cast);
	}

	// x = newValue
	auto target = awst::makeVarExpression(varName, varType, _loc);

	auto assign = std::make_shared<awst::AssignmentStatement>();
	assign->sourceLocation = _loc;
	assign->target = std::move(target);
	assign->value = std::move(newValue);
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

	auto replace = std::make_shared<awst::IntrinsicCall>();
	replace->sourceLocation = _loc;
	replace->wtype = awst::WType::bytesType();
	replace->opCode = "replace3";
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

	auto lowByte = std::make_shared<awst::IntrinsicCall>();
	lowByte->sourceLocation = _loc;
	lowByte->wtype = awst::WType::bytesType();
	lowByte->opCode = "extract3";
	lowByte->stackArgs.push_back(std::move(padded));
	lowByte->stackArgs.push_back(std::move(start));
	lowByte->stackArgs.push_back(std::move(len));

	auto replace = std::make_shared<awst::IntrinsicCall>();
	replace->sourceLocation = _loc;
	replace->wtype = awst::WType::bytesType();
	replace->opCode = "replace3";
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

			auto returnOp = std::make_shared<awst::IntrinsicCall>();
			returnOp->sourceLocation = _loc;
			returnOp->wtype = awst::WType::voidType();
			returnOp->opCode = "return";
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

		auto extract = std::make_shared<awst::IntrinsicCall>();
		extract->sourceLocation = _loc;
		extract->wtype = awst::WType::bytesType();
		extract->opCode = "extract3";
		extract->stackArgs.push_back(memoryVar(_loc));
		extract->stackArgs.push_back(std::move(offsetU64));
		extract->stackArgs.push_back(std::move(sizeU64));

		// log(data) — emit the raw bytes as a transaction log
		auto logCall = std::make_shared<awst::IntrinsicCall>();
		logCall->sourceLocation = _loc;
		logCall->wtype = awst::WType::voidType();
		logCall->opCode = "log";
		logCall->stackArgs.push_back(std::move(extract));

		auto logStmt = awst::makeExpressionStatement(std::move(logCall), _loc);
		_out.push_back(std::move(logStmt));

		// Flush and return void
		flushMemoryToScratch(_loc, _out);
		auto ret = std::make_shared<awst::ReturnStatement>();
		ret->sourceLocation = _loc;
		ret->value = nullptr;
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

		auto cmp = std::make_shared<awst::NumericComparisonExpression>();
		cmp->sourceLocation = _loc;
		cmp->wtype = awst::WType::boolType();
		cmp->lhs = std::move(returnValue);
		cmp->op = awst::NumericComparison::Ne;
		cmp->rhs = std::move(zero);
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

	auto ret = std::make_shared<awst::ReturnStatement>();
	ret->sourceLocation = _loc;
	ret->value = std::move(returnValue);
	_out.push_back(std::move(ret));
}

// ─── Statement translation ─────────────────────────────────────────────────


} // namespace puyasol::builder
