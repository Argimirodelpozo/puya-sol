/// @file MemoryOps.cpp
/// Memory operations: mload, mstore, handleReturn, tryHandleBytesMemoryRead.

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

	auto offset = resolveConstantOffset(_args[0]);
	if (!offset)
	{
		Logger::instance().warning(
			"mload with non-constant offset not supported — returning 0 (EVM memory model)", _loc
		);
		auto zero = std::make_shared<awst::IntegerConstant>();
		zero->sourceLocation = _loc;
		zero->wtype = awst::WType::biguintType();
		zero->value = "0";
		return zero;
	}

	auto it = m_memoryMap.find(*offset);
	if (it != m_memoryMap.end())
	{
		auto const& slot = it->second;
		if (slot.isParam)
		{
			// Access array parameter element: param[index]
			auto base = std::make_shared<awst::VarExpression>();
			base->sourceLocation = _loc;
			base->name = slot.varName;
			base->wtype = m_arrayParamType;

			auto index = std::make_shared<awst::IntegerConstant>();
			index->sourceLocation = _loc;
			index->wtype = awst::WType::uint64Type();
			index->value = std::to_string(slot.paramIndex);

			auto indexExpr = std::make_shared<awst::IndexExpression>();
			indexExpr->sourceLocation = _loc;
			indexExpr->wtype = awst::WType::biguintType();
			indexExpr->base = std::move(base);
			indexExpr->index = std::move(index);
			return indexExpr;
		}
		else
		{
			// Read from scratch variable
			auto var = std::make_shared<awst::VarExpression>();
			var->sourceLocation = _loc;
			var->name = slot.varName;
			var->wtype = awst::WType::biguintType();
			return var;
		}
	}

	// Check if the offset falls within m_calldataMap (function parameters).
	// This handles mload from calldata struct field offsets, e.g., mload(key + 32)
	// where key is a struct parameter at calldata offset 4.
	auto cdIt = m_calldataMap.find(*offset);
	if (cdIt != m_calldataMap.end())
	{
		auto const& elem = cdIt->second;
		auto base = std::make_shared<awst::VarExpression>();
		base->sourceLocation = _loc;
		base->name = elem.paramName;
		base->wtype = m_locals.count(elem.paramName)
			? m_locals[elem.paramName]
			: awst::WType::biguintType();

		return accessFlatElement(std::move(base), elem.paramType, elem.flatIndex, _loc);
	}

	// Unknown memory offset — create a scratch variable on the fly
	std::string varName = "mem_0x" + ([&] {
		std::ostringstream oss;
		oss << std::hex << *offset;
		return oss.str();
	})();

	MemorySlot slot;
	slot.varName = varName;
	m_memoryMap[*offset] = slot;

	auto var = std::make_shared<awst::VarExpression>();
	var->sourceLocation = _loc;
	var->name = varName;
	var->wtype = awst::WType::biguintType();
	return var;
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
	if (!outerAdd || outerAdd->functionName.name.str() != "add"
		|| outerAdd->arguments.size() != 2)
		return nullptr;

	// One arg of outer add should be add(bytes_param, 32), the other is the offset
	solidity::yul::FunctionCall const* innerAdd = nullptr;
	solidity::yul::Expression const* offsetExprYul = nullptr;

	auto* call0 = std::get_if<solidity::yul::FunctionCall>(&outerAdd->arguments[0]);
	auto* call1 = std::get_if<solidity::yul::FunctionCall>(&outerAdd->arguments[1]);

	if (call0 && call0->functionName.name.str() == "add" && call0->arguments.size() == 2)
	{
		innerAdd = call0;
		offsetExprYul = &outerAdd->arguments[1];
	}
	else if (call1 && call1->functionName.name.str() == "add" && call1->arguments.size() == 2)
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
	auto paramVar = std::make_shared<awst::VarExpression>();
	paramVar->sourceLocation = _loc;
	paramVar->name = paramName;
	paramVar->wtype = paramType;

	// Translate the dynamic offset and convert biguint → uint64
	auto offsetExpr = buildExpression(*offsetExprYul);

	auto offsetBytes = std::make_shared<awst::ReinterpretCast>();
	offsetBytes->sourceLocation = _loc;
	offsetBytes->wtype = awst::WType::bytesType();
	offsetBytes->expr = offsetExpr;

	auto offsetU64 = std::make_shared<awst::IntrinsicCall>();
	offsetU64->sourceLocation = _loc;
	offsetU64->wtype = awst::WType::uint64Type();
	offsetU64->opCode = "btoi";
	offsetU64->stackArgs.push_back(std::move(offsetBytes));

	// Length: 32 bytes
	auto lenArg = std::make_shared<awst::IntegerConstant>();
	lenArg->sourceLocation = _loc;
	lenArg->wtype = awst::WType::uint64Type();
	lenArg->value = "32";

	// extract3(param, offset, 32)
	auto extract = std::make_shared<awst::IntrinsicCall>();
	extract->sourceLocation = _loc;
	extract->wtype = awst::WType::bytesType();
	extract->opCode = "extract3";
	extract->stackArgs.push_back(std::move(paramVar));
	extract->stackArgs.push_back(std::move(offsetU64));
	extract->stackArgs.push_back(std::move(lenArg));

	// Cast bytes → biguint (mload returns uint256)
	auto result = std::make_shared<awst::ReinterpretCast>();
	result->sourceLocation = _loc;
	result->wtype = awst::WType::biguintType();
	result->expr = std::move(extract);

	return result;
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

	auto offset = resolveConstantOffset(_args[0]);
	if (!offset)
	{
		// Try to decompose as VarExpression + constant offset
		auto decomposed = decomposeVarOffset(_args[0]);
		if (!decomposed)
		{
			Logger::instance().warning(
				"mstore with non-constant offset not supported in assembly translation (skipping)", _loc
			);
			return;
		}
		// Track in variable-offset memory map
		auto const& [baseName, relOffset] = *decomposed;
		std::string varName = "vmem_" + baseName + "_0x" + ([&] {
			std::ostringstream oss;
			oss << std::hex << relOffset;
			return oss.str();
		})();
		MemorySlot slot;
		slot.varName = varName;
		m_varMemoryMap[baseName][relOffset] = slot;

		if (m_locals.find(varName) == m_locals.end())
			m_locals[varName] = awst::WType::biguintType();

		auto target = std::make_shared<awst::VarExpression>();
		target->sourceLocation = _loc;
		target->name = varName;
		target->wtype = awst::WType::biguintType();

		auto storeValue = ensureBiguint(_args[1], _loc);

		auto assign = std::make_shared<awst::AssignmentStatement>();
		assign->sourceLocation = _loc;
		assign->target = std::move(target);
		assign->value = std::move(storeValue);
		_out.push_back(std::move(assign));
		return;
	}

	// Find or create the scratch variable for this offset
	std::string varName;
	auto it = m_memoryMap.find(*offset);
	if (it != m_memoryMap.end())
	{
		if (it->second.isParam)
		{
			// Param slot is being overwritten — shadow it with a scratch variable.
			// After this, mload at this offset will read the scratch var, not the param.
			varName = "mem_0x" + ([&] {
				std::ostringstream oss;
				oss << std::hex << *offset;
				return oss.str();
			})();
			MemorySlot slot;
			slot.varName = varName;
			slot.isParam = false;
			slot.paramIndex = -1;
			it->second = slot;
		}
		else
		{
			varName = it->second.varName;
		}
	}
	else
	{
		varName = "mem_0x" + ([&] {
			std::ostringstream oss;
			oss << std::hex << *offset;
			return oss.str();
		})();
		MemorySlot slot;
		slot.varName = varName;
		m_memoryMap[*offset] = slot;
	}

	// Register as local if not already
	if (m_locals.find(varName) == m_locals.end())
		m_locals[varName] = awst::WType::biguintType();

	// Track constant values stored to memory (especially free memory pointer)
	auto storedVal = resolveConstantOffset(_args[1]);
	if (storedVal)
		m_localConstants[varName] = *storedVal;

	auto target = std::make_shared<awst::VarExpression>();
	target->sourceLocation = _loc;
	target->name = varName;
	target->wtype = awst::WType::biguintType();

	// Coerce value to biguint to match memory slot type (EVM mstore = 256-bit write)
	auto storeValue = ensureBiguint(_args[1], _loc);

	auto assign = std::make_shared<awst::AssignmentStatement>();
	assign->sourceLocation = _loc;
	assign->target = std::move(target);
	assign->value = std::move(storeValue);
	_out.push_back(std::move(assign));
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
	auto offset = resolveConstantOffset(_args[0]);
	if (!offset)
	{
		Logger::instance().error(
			"return with non-constant offset not supported", _loc
		);
		return;
	}

	// Look up what was stored at this offset
	std::shared_ptr<awst::Expression> returnValue;
	auto it = m_memoryMap.find(*offset);
	if (it != m_memoryMap.end())
	{
		auto var = std::make_shared<awst::VarExpression>();
		var->sourceLocation = _loc;
		var->name = it->second.varName;
		var->wtype = awst::WType::biguintType();
		returnValue = std::move(var);
	}
	else
	{
		// Offset not in memory map — return zero
		Logger::instance().warning(
			"return from unknown memory offset; defaulting to zero", _loc
		);
		auto zero = std::make_shared<awst::IntegerConstant>();
		zero->sourceLocation = _loc;
		zero->wtype = m_returnType ? m_returnType : awst::WType::biguintType();
		zero->value = "0";
		returnValue = std::move(zero);
	}

	// Convert to bool if the function's return type is bool
	if (m_returnType == awst::WType::boolType()
		&& returnValue->wtype != awst::WType::boolType())
	{
		auto zero = std::make_shared<awst::IntegerConstant>();
		zero->sourceLocation = _loc;
		zero->wtype = awst::WType::biguintType();
		zero->value = "0";

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

	auto ret = std::make_shared<awst::ReturnStatement>();
	ret->sourceLocation = _loc;
	ret->value = std::move(returnValue);
	_out.push_back(std::move(ret));
}

// ─── Statement translation ─────────────────────────────────────────────────


} // namespace puyasol::builder
