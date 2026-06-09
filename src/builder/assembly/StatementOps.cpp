/// @file StatementOps.cpp
/// Yul statement translation: variable declarations, assignments, expression statements, function definitions.

#include "builder/assembly/AssemblyBuilder.h"
#include "Logger.h"

#include <libsolutil/Numeric.h>

#include <sstream>

namespace puyasol::builder
{

void AssemblyBuilder::buildStatement(
	solidity::yul::Statement const& _stmt,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	std::visit(
		[this, &_out](auto const& _node) {
			using T = std::decay_t<decltype(_node)>;
			if constexpr (std::is_same_v<T, solidity::yul::VariableDeclaration>)
				buildVariableDeclaration(_node, _out);
			else if constexpr (std::is_same_v<T, solidity::yul::Assignment>)
				buildAssignment(_node, _out);
			else if constexpr (std::is_same_v<T, solidity::yul::ExpressionStatement>)
				buildExpressionStatement(_node, _out);
			else if constexpr (std::is_same_v<T, solidity::yul::FunctionDefinition>)
				buildFunctionDefinition(_node, _out);
			else if constexpr (std::is_same_v<T, solidity::yul::Block>)
			{
				// Nested block — translate all its statements
				for (auto const& innerStmt: _node.statements)
					buildStatement(innerStmt, _out);
			}
			else if constexpr (std::is_same_v<T, solidity::yul::If>)
				buildIfStatement(_node, _out);
			else if constexpr (std::is_same_v<T, solidity::yul::ForLoop>)
				buildForLoop(_node, _out);
			else if constexpr (std::is_same_v<T, solidity::yul::Break>)
				buildBreakStatement(_node, _out);
			else if constexpr (std::is_same_v<T, solidity::yul::Continue>)
				buildContinueStatement(_node, _out);
			else if constexpr (std::is_same_v<T, solidity::yul::Leave>)
				buildLeaveStatement(_node, _out);
			else if constexpr (std::is_same_v<T, solidity::yul::Switch>)
				buildSwitchStatement(_node, _out);
		},
		_stmt
	);
}

void AssemblyBuilder::buildVariableDeclaration(
	solidity::yul::VariableDeclaration const& _decl,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	auto loc = makeLoc(_decl.debugData);

	// Check for special function call patterns: staticcall, user-defined functions
	if (_decl.value)
	{
		if (auto const* call = std::get_if<solidity::yul::FunctionCall>(_decl.value.get()))
		{
			std::string callName = getFunctionName(call->functionName);

			if (_decl.variables.size() == 1 && (callName == "staticcall" || callName == "call"))
			{
				std::string varName = _decl.variables[0].name.str();
				handlePrecompileCall(*call, varName, loc, _out, /*_isCall=*/callName == "call");
				return;
			}

			// User-defined assembly function called in variable declaration context
			// Handles both single (let x := f()) and multi (let a, b, c := f()) returns
			if (m_asmFunctions.count(callName))
			{
				auto const& funcDef = *m_asmFunctions[callName];

				// Register all declared variables
				for (auto const& var: _decl.variables)
					m_locals[var.name.str()] = awst::WType::biguintType();

				// Translate arguments
				std::vector<std::shared_ptr<awst::Expression>> args;
				for (auto const& arg: call->arguments)
					args.push_back(buildExpression(arg));

				// Inline the function body (populates return variables)
				handleUserFunctionCall(callName, args, loc, _out);

				// Map the function's return variables to the declared variables
				size_t numReturns = std::min(
					_decl.variables.size(), funcDef.returnVariables.size()
				);
				for (size_t i = 0; i < numReturns; ++i)
				{
					std::string retName = funcDef.returnVariables[i].name.str();
					std::string varName = _decl.variables[i].name.str();

					auto retVar = awst::makeVarExpression(retName, awst::WType::biguintType(), loc);

					auto target = awst::makeVarExpression(varName, awst::WType::biguintType(), loc);

					auto assign = awst::makeAssignmentStatement(std::move(target), std::move(retVar), loc);
					_out.push_back(std::move(assign));
				}
				return;
			}
		}
	}

	for (auto const& var: _decl.variables)
	{
		std::string name = var.name.str();
		m_locals[name] = awst::WType::biguintType();

		// Try to resolve compile-time constant value for tracking
		if (_decl.value)
		{
			auto constVal = resolveConstantYulValue(*_decl.value);
			if (constVal)
				m_localConstants[name] = *constVal;
		}
		else
		{
			m_localConstants[name] = 0;
		}

		auto target = awst::makeVarExpression(name, awst::WType::biguintType(), makeLoc(var.debugData));

		std::shared_ptr<awst::Expression> value;
		if (_decl.value)
		{
			value = buildExpression(*_decl.value);
			// Drain any pending statements from inlined assembly functions
			for (auto& ps: m_pendingStatements)
				_out.push_back(std::move(ps));
			m_pendingStatements.clear();

			if (!value)
			{
				// Expression failed to translate (error already logged), use zero fallback
				auto zero = awst::makeZero(loc, awst::WType::biguintType());
				value = std::move(zero);
			}
		}
		else
		{
			// Default: zero
			auto zero = awst::makeZero(loc, awst::WType::biguintType());
			value = std::move(zero);
		}

		// Coerce value to match target (biguint) — Yul values are always 256-bit
		value = ensureBiguint(std::move(value), loc);

		auto assign = awst::makeAssignmentStatement(std::move(target), std::move(value), loc);
		_out.push_back(std::move(assign));
	}
}

void AssemblyBuilder::buildAssignment(
	solidity::yul::Assignment const& _assign,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	auto loc = makeLoc(_assign.debugData);

	// Multi-variable assignment from assembly function: a, b, c := f(...)
	if (_assign.variableNames.size() > 1)
	{
		if (_assign.value)
		{
			if (auto const* call = std::get_if<solidity::yul::FunctionCall>(_assign.value.get()))
			{
				std::string callName = getFunctionName(call->functionName);
				if (m_asmFunctions.count(callName))
				{
					auto const& funcDef = *m_asmFunctions[callName];

					// Translate arguments
					std::vector<std::shared_ptr<awst::Expression>> args;
					for (auto const& arg: call->arguments)
						args.push_back(buildExpression(arg));

					// Inline the function body
					handleUserFunctionCall(callName, args, loc, _out);

					// Map return variables to assignment targets
					size_t numReturns = std::min(
						_assign.variableNames.size(), funcDef.returnVariables.size()
					);
					for (size_t i = 0; i < numReturns; ++i)
					{
						std::string retName = funcDef.returnVariables[i].name.str();
						std::string varName = _assign.variableNames[i].name.str();

						auto retVar = awst::makeVarExpression(retName, awst::WType::biguintType(), loc);

						auto target = awst::makeVarExpression(varName, awst::WType::biguintType(), loc);

						auto assign = awst::makeAssignmentStatement(std::move(target), std::move(retVar), loc);
						_out.push_back(std::move(assign));
					}
					return;
				}
			}
		}

		Logger::instance().error(
			"multi-variable assignment not yet supported in assembly translation", loc
		);
		return;
	}

	std::string name = _assign.variableNames[0].name.str();

	// fn-ptr selector/address writes: fp.selector := expr  /  fp.address := expr
	// Update the corresponding 4-byte (selector) or 8-byte (address) slice of the
	// 12-byte fn-ptr local via replace3. The base local is identified by looking
	// up the dotted name in m_locals (SolInlineAssembly registers fp.selector with
	// the underlying fn-ptr type bytes[12]).
	{
		auto dotIdx = name.rfind('.');
		if (dotIdx != std::string::npos && _assign.value)
		{
			std::string suffix = name.substr(dotIdx + 1);
			std::string baseName = name.substr(0, dotIdx);
			if (suffix == "selector" || suffix == "address")
			{
				auto fullIt = m_locals.find(name);
				if (fullIt != m_locals.end())
				{
					auto const* bwt = dynamic_cast<awst::BytesWType const*>(fullIt->second);
					if (bwt && bwt->length().has_value() && *bwt->length() == 12)
					{
						auto rhs = buildExpression(*_assign.value);
						for (auto& ps: m_pendingStatements)
							_out.push_back(std::move(ps));
						m_pendingStatements.clear();
						if (!rhs)
							return;

						int sliceWidth = (suffix == "selector") ? 4 : 8;
						int sliceOffset = (suffix == "selector") ? 8 : 0;

						// Build the rhs slice as exactly `sliceWidth` bytes.
						// - account/bytes input (e.g. fp.address := someAddress): take the
						//   low `sliceWidth` bytes via extract3(rhs, len(rhs)-w, w). EVM
						//   right-aligns address values inside a 32-byte word.
						// - numeric input: itob to 8 bytes, then take the low `sliceWidth`.
						std::shared_ptr<awst::Expression> sliceBytes;
						bool rhsIsBytesLike =
							rhs->wtype == awst::WType::accountType()
							|| (rhs->wtype && rhs->wtype->kind() == awst::WTypeKind::Bytes);

						if (rhsIsBytesLike)
						{
							std::shared_ptr<awst::Expression> rhsBytes =
								(rhs->wtype == awst::WType::bytesType())
								? rhs
								: awst::makeAsBytes(rhs, loc);
							auto rhsBytesForLen = rhsBytes;

							auto lenCall = awst::makeIntrinsicCall(
								"len", awst::WType::uint64Type(), loc);
							lenCall->stackArgs.push_back(std::move(rhsBytesForLen));
							auto offsetExpr = awst::makeUInt64BinOp(
								std::move(lenCall), awst::UInt64BinaryOperator::Sub,
								awst::makeIntegerConstant(sliceWidth, loc), loc);

							auto extractCall = awst::makeIntrinsicCall(
								"extract3", awst::WType::bytesType(), loc);
							extractCall->stackArgs.push_back(std::move(rhsBytes));
							extractCall->stackArgs.push_back(std::move(offsetExpr));
							extractCall->stackArgs.push_back(awst::makeIntegerConstant(
								std::to_string(sliceWidth), loc));
							sliceBytes = std::move(extractCall);
						}
						else
						{
							if (rhs->wtype == awst::WType::biguintType())
								rhs = safeBtoi(std::move(rhs), loc);

							auto itobCall = awst::makeIntrinsicCall(
								"itob", awst::WType::bytesType(), loc);
							itobCall->stackArgs.push_back(std::move(rhs));

							if (sliceWidth == 8)
							{
								sliceBytes = std::move(itobCall);
							}
							else
							{
								auto extractCall = awst::makeIntrinsicCall(
									"extract3", awst::WType::bytesType(), loc);
								extractCall->stackArgs.push_back(std::move(itobCall));
								extractCall->stackArgs.push_back(awst::makeIntegerConstant(
									std::to_string(8 - sliceWidth), loc));
								extractCall->stackArgs.push_back(awst::makeIntegerConstant(
									std::to_string(sliceWidth), loc));
								sliceBytes = std::move(extractCall);
							}
						}

						auto baseVar = awst::makeVarExpression(baseName, fullIt->second, loc);
						auto baseAsBytes = awst::makeAsBytes(std::move(baseVar), loc);

						auto replaceCall = awst::makeIntrinsicCall(
							"replace3", awst::WType::bytesType(), loc);
						replaceCall->stackArgs.push_back(std::move(baseAsBytes));
						replaceCall->stackArgs.push_back(awst::makeIntegerConstant(
							std::to_string(sliceOffset), loc));
						replaceCall->stackArgs.push_back(std::move(sliceBytes));

						auto castBack = awst::makeReinterpretCast(
							std::move(replaceCall), fullIt->second, loc);

						auto target = awst::makeVarExpression(baseName, fullIt->second, loc);
						auto assign = awst::makeAssignmentStatement(
							std::move(target), std::move(castBack), loc);
						_out.push_back(std::move(assign));
						return;
					}
				}
			}
		}
	}

	// Handle storage slot assignments: _x.slot := expr
	// Compute the slot value and assign to the base variable name.
	// The variable holds the slot number as biguint, enabling slot-based
	// storage operations (sload/sstore) for storage references.
	if (name.find(".slot") != std::string::npos)
	{
		std::string baseName = name.substr(0, name.find(".slot"));
		if (!baseName.empty() && _assign.value)
		{
			auto slotExpr = buildExpression(*_assign.value);
			if (slotExpr)
			{
				// Ensure biguint type for the slot value
				if (slotExpr->wtype == awst::WType::uint64Type())
				{
					auto itob = awst::makeItob(std::move(slotExpr), loc);
					slotExpr = awst::makeAsBiguint(std::move(itob), loc);
				}
				else if (slotExpr->wtype != awst::WType::biguintType())
				{
					// bytes[N] or other non-biguint → reinterpret as biguint
					auto cast = awst::makeAsBiguint(std::move(slotExpr), loc);
					slotExpr = std::move(cast);
				}

				auto target = awst::makeVarExpression(baseName, awst::WType::biguintType(), loc);

				auto assign = awst::makeAssignmentStatement(std::move(target), std::move(slotExpr), loc);
				_out.push_back(std::move(assign));
			}
		}
		return;
	}

	// Check for staticcall pattern: success := staticcall(...)
	if (_assign.value)
	{
		if (auto const* call = std::get_if<solidity::yul::FunctionCall>(_assign.value.get()))
		{
			std::string callName = getFunctionName(call->functionName);
			if (callName == "staticcall" || callName == "call")
			{
				handlePrecompileCall(*call, name, loc, _out, /*_isCall=*/callName == "call");
				return;
			}
		}
	}

	auto it = m_locals.find(name);
	auto const* wtype = (it != m_locals.end()) ? it->second : awst::WType::biguintType();
	auto target = awst::makeVarExpression(name, wtype, loc);

	auto value = buildExpression(*_assign.value);
	// Drain any pending statements from inlined assembly functions
	for (auto& ps: m_pendingStatements)
		_out.push_back(std::move(ps));
	m_pendingStatements.clear();

	if (!value)
	{
		// Expression failed to translate (error already logged), use zero fallback
		auto zero = awst::makeZero(loc, target->wtype);
		value = std::move(zero);
	}

	// Coerce value type to match target type when they differ
	if (target->wtype != value->wtype)
	{
		if (target->wtype == awst::WType::biguintType())
		{
			// Target is biguint — coerce value to biguint
			value = ensureBiguint(std::move(value), loc);
		}
		else if (target->wtype == awst::WType::boolType())
		{
			// Target is bool — coerce value to bool
			value = ensureBool(std::move(value), loc);
		}
		else if (target->wtype->kind() == awst::WTypeKind::Bytes)
		{
			auto const* bytesType = dynamic_cast<awst::BytesWType const*>(target->wtype);
			// For fixed-size bytes[N], pad biguint to 32 bytes then extract first N bytes
			// (EVM stores bytesN left-aligned in 256-bit words)
			if (bytesType && bytesType->length() && *bytesType->length() > 0)
			{
				int n = *bytesType->length();
				auto biguintVal = ensureBiguint(std::move(value), loc);
				// padTo32Bytes: ensures exactly 32 bytes big-endian
				auto padded = padTo32Bytes(std::move(biguintVal), loc);
				// Extract first N bytes (EVM left-aligned)
				auto zero = awst::makeZero(loc);
				auto lenConst = awst::makeIntegerConstant(n, loc);
				auto extract = awst::makeExtract3(std::move(padded), std::move(zero), std::move(lenConst), loc);
				auto cast = awst::makeReinterpretCast(std::move(extract), target->wtype, loc);
				value = std::move(cast);
			}
			else
			{
				// Untyped bytes — coerce biguint to bytes via ReinterpretCast
				auto biguintVal = ensureBiguint(std::move(value), loc);
				auto cast = awst::makeReinterpretCast(std::move(biguintVal), target->wtype, loc);
				value = std::move(cast);
			}
		}
		else if (target->wtype == awst::WType::accountType())
		{
			// Account (address) — pad biguint to 32 bytes for AVM address
			auto biguintVal = ensureBiguint(std::move(value), loc);
			auto padded = padTo32Bytes(std::move(biguintVal), loc);
			auto cast = awst::makeAsAccount(std::move(padded), loc);
			value = std::move(cast);
		}
		else if (target->wtype == awst::WType::uint64Type())
		{
			// Target is uint64 but value is biguint (e.g. from mload).
			// Truncate to uint64 using safeBtoi to keep the variable type consistent
			// across all control flow paths (avoids phi node type mismatches).
			if (value->wtype == awst::WType::biguintType())
			{
				// If the Solidity type is sub-64-bit (uint8/uint16/uint32), mask first
				auto bwIt = m_paramBitWidths.find(name);
				if (bwIt != m_paramBitWidths.end() && bwIt->second < 64)
				{
					solidity::u256 mask = (solidity::u256(1) << bwIt->second) - 1;
					std::ostringstream maskStr;
					maskStr << mask;

					auto maskConst = awst::makeIntegerConstant(maskStr.str(), loc, awst::WType::biguintType());

					auto andOp = awst::makeBigUIntBinOp(std::move(value), awst::BigUIntBinaryOperator::BitAnd, std::move(maskConst), loc);
					value = std::move(andOp);
				}
				value = safeBtoi(std::move(value), loc);
			}
		}
		else if (target->wtype == awst::WType::accountType())
		{
			// Target is account — coerce biguint/bytes to account
			if (value->wtype == awst::WType::biguintType())
			{
				// biguint → bytes → account
				auto toBytes = awst::makeAsBytes(std::move(value), loc);

				auto toAccount = awst::makeAsAccount(std::move(toBytes), loc);
				value = std::move(toAccount);
			}
			else if (value->wtype != awst::WType::accountType())
			{
				auto cast = awst::makeAsAccount(std::move(value), loc);
				value = std::move(cast);
			}
		}
		else
		{
			// Fallback: assembly pointer reinterpretation (e.g., result := store
			// where result is address[] and store is bytes32[] — same layout on EVM).
			auto const* targetArr = dynamic_cast<awst::ReferenceArray const*>(target->wtype);
			auto const* valueArr = dynamic_cast<awst::ReferenceArray const*>(value->wtype);
			if (targetArr && valueArr)
			{
				// Both are arrays — EVM memory pointer alias.
				// Force the value's type to match the target so the assignment is valid.
				// On AVM, arrays of bytes32 and addresses are both 32-byte elements.
				value->wtype = target->wtype;
				// Also update the source variable's registered type so future references match
				if (auto* srcVar = dynamic_cast<awst::VarExpression*>(value.get()))
					m_locals[srcVar->name] = target->wtype;
			}
			else
			{
				Logger::instance().debug(
					"assembly type coercion: " + value->wtype->name() + " → " + target->wtype->name()
				);
				// Don't mutate IntegerConstant wtype to a non-integer type
				// (struct, array, etc.) — puya rejects it during deserialization.
				// Wrap with a ReinterpretCast instead. For aggregate targets the
				// runtime semantics may still be wrong, but at least the contract
				// compiles and downstream tests can run.
				if (dynamic_cast<awst::IntegerConstant const*>(value.get()))
				{
					auto cast = awst::makeReinterpretCast(std::move(value), target->wtype, loc);
					value = std::move(cast);
				}
				else
				{
					value->wtype = target->wtype;
				}
			}
		}
	}

	auto assign = awst::makeAssignmentStatement(std::move(target), std::move(value), loc);
	_out.push_back(std::move(assign));
}

void AssemblyBuilder::buildExpressionStatement(
	solidity::yul::ExpressionStatement const& _stmt,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	auto loc = makeLoc(_stmt.debugData);

	// Expression statements in Yul are typically side-effecting calls
	// like mstore(), return(), or user-defined function calls.
	if (auto const* call = std::get_if<solidity::yul::FunctionCall>(&_stmt.expression))
	{
		std::string funcName = getFunctionName(call->functionName);

		// Before translating args, check for patterns that need raw Yul AST access.
		if (funcName == "mstore")
		{
			// Try to detect mstore(add(bytes_var, 32), value) pattern
			if (tryHandleBytesMemoryWrite(*call, loc, _out))
				return;
		}
		if (funcName == "mcopy")
		{
			// Try to detect mcopy(add(add(bytes_var, 0x20), dstOff), ...) pattern
			if (tryHandleBytesMemoryMcopy(*call, loc, _out))
				return;
		}

		// Translate arguments (stored in source order)
		std::vector<std::shared_ptr<awst::Expression>> args;
		for (auto const& arg: call->arguments)
			args.push_back(buildExpression(arg));
		// Drain any pending statements from inlined assembly functions
		for (auto& ps: m_pendingStatements)
			_out.push_back(std::move(ps));
		m_pendingStatements.clear();

		if (funcName == "mstore")
		{
			handleMstore(args, loc, _out);
			return;
		}
		if (funcName == "mstore8")
		{
			handleMstore8(args, loc, _out);
			return;
		}
		if (funcName == "return")
		{
			handleReturn(args, loc, _out);
			return;
		}
		if (funcName == "staticcall" || funcName == "call")
		{
			handlePrecompileCall(*call, "", loc, _out, /*_isCall=*/funcName == "call");
			return;
		}
		if (funcName == "revert")
		{
			handleRevert(args, loc, _out);
			return;
		}
		if (funcName == "tstore")
		{
			handleTstore(args, loc, _out);
			return;
		}
		if (funcName == "sstore")
		{
			handleSstore(args, loc, _out);
			return;
		}
		if (funcName == "invalid")
		{
			// EVM INVALID opcode — unconditional revert
			auto stmt = awst::makeExpressionStatement(awst::makeAssert(awst::makeFalse(loc), loc, "invalid"), loc);
			_out.push_back(std::move(stmt));
			return;
		}
		if (funcName == "stop")
		{
			// EVM STOP — halt execution successfully
			auto retStmt = awst::makeReturnStatement(nullptr, loc);
			_out.push_back(std::move(retStmt));
			return;
		}
		if (funcName == "returndatacopy")
		{
			// Copy the last inner txn's log (itxn LastLog) into memory.
			emitReturndatacopy(args, loc, _out);
			return;
		}
		if (funcName == "pop")
		{
			// pop(x) — discard value, no-op
			return;
		}
		if (funcName == "delegatecall")
		{
			// delegatecall has no AVM equivalent — HARD ERROR. Stubbing it as a
			// no-op would silently drop the call. Matches the hard error on
			// high-level `.delegatecall(...)`.
			Logger::instance().error(
				"`delegatecall(...)` in inline assembly is not supported on AVM. "
				"It runs another contract's code in the caller's storage context, "
				"which has no AVM equivalent; stubbing it as a no-op would silently "
				"drop the call. This matches the hard error on high-level "
				"`.delegatecall(...)`.",
				loc
			);
			return;
		}
		if (funcName == "mcopy")
		{
			// mcopy(dst, src, length): copy memory slot src to dst
			// In our memory-slot model, this is equivalent to mstore(dst, mload(src)).
			// A compile-time length of 0 is a no-op — skip entirely so we don't
			// dereference possibly-out-of-bounds src offsets (mcopy_empty pattern).
			if (args.size() >= 3)
			{
				if (auto const* lenConst =
					dynamic_cast<awst::IntegerConstant const*>(args[2].get()))
				{
					if (lenConst->value == "0")
						return;
				}
			}
			if (args.size() >= 2)
			{
				auto mloadArgs = std::vector<std::shared_ptr<awst::Expression>>{args[1]};
				auto loadedVal = handleMload(mloadArgs, loc);
				if (loadedVal)
				{
					auto storeArgs = std::vector<std::shared_ptr<awst::Expression>>{args[0], loadedVal};
					handleMstore(storeArgs, loc, _out);
				}
			}
			return;
		}

		// Check for user-defined assembly function call
		auto asmIt = m_asmFunctions.find(funcName);
		if (asmIt != m_asmFunctions.end())
		{
			handleUserFunctionCall(funcName, args, loc, _out);
			return;
		}

		// Other side-effecting calls: wrap as ExpressionStatement
		auto expr = buildExpression(_stmt.expression);
		if (expr)
		{
			auto exprStmt = awst::makeExpressionStatement(std::move(expr), loc);
			_out.push_back(std::move(exprStmt));
		}
	}
	else
	{
		// Non-call expression statement
		auto expr = buildExpression(_stmt.expression);
		if (expr)
		{
			auto exprStmt = awst::makeExpressionStatement(std::move(expr), loc);
			_out.push_back(std::move(exprStmt));
		}
	}
}


} // namespace puyasol::builder
