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

			// User-defined Yul function: single (let x := f()) or multi (let a,b := f()) return.
			if (m_asmFunctions.count(callName))
			{
				auto const& funcDef = *m_asmFunctions[callName];

				for (auto const& var: _decl.variables)
				{
					std::string n = var.name.str();
					if (auto rit = m_yulInlineRenames.find(n); rit != m_yulInlineRenames.end())
						n = rit->second;
					m_locals[n] = awst::WType::biguintType();
				}

				// Right-to-left: Yul argument evaluation order.
				std::vector<std::shared_ptr<awst::Expression>> args(call->arguments.size());
				for (size_t ai = call->arguments.size(); ai-- > 0; )
					args[ai] = buildExpression(call->arguments[ai]);

				handleUserFunctionCall(callName, args, loc, _out);

				// Subroutine call → return values in m_yulSubReturnTemps;
				// inlined call → return values in the function's own return-var names.
				bool fromSub = !m_yulSubReturnTemps.empty();
				size_t numReturns = std::min(
					_decl.variables.size(), funcDef.returnVariables.size()
				);
				for (size_t i = 0; i < numReturns; ++i)
				{
					std::string retName = fromSub
						? m_yulSubReturnTemps[i]
						: funcDef.returnVariables[i].name.str();
					std::string varName = _decl.variables[i].name.str();
					// Inline frames: declare under the frame's unique name.
					if (auto rit = m_yulInlineRenames.find(varName); rit != m_yulInlineRenames.end())
						varName = rit->second;

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
		std::string origName = var.name.str();
		// Inline-expanded bodies: declare under the frame's UNIQUE name so
		// sibling/nested calls reusing the same bare local don't share one
		// runtime var (reads already resolve through the same map).
		std::string name = origName;
		if (auto rit = m_yulInlineRenames.find(origName); rit != m_yulInlineRenames.end())
			name = rit->second;
		m_locals[name] = awst::WType::biguintType();

		// Record the initializer constant only for SINGLE-ASSIGNMENT locals —
		// the fold is flow-insensitive, so a later `name := …` (loop counter,
		// pointer bump) would leave this entry stale. Erase on the non-constant
		// path: a shadowing `let` in a sibling scope must not inherit a stale
		// entry from an earlier same-named declaration. (The reassignment scan
		// keys on ORIGINAL names — check origName, record under name.)
		if (m_reassignedLocals.count(origName))
			m_localConstants.erase(name);
		else if (_decl.value)
		{
			auto constVal = resolveConstantYulValue(*_decl.value);
			if (constVal)
				m_localConstants[name] = *constVal;
			else
				m_localConstants.erase(name);
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
			drainPendingStatements(_out);
			if (!value)
				value = awst::makeZero(loc, awst::WType::biguintType());
		}
		else
		{
			value = awst::makeZero(loc, awst::WType::biguintType());
		}

		// Yul values are always 256-bit.
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

	if (_assign.variableNames.size() > 1) // multi-var: a, b, c := f(...)
	{
		if (_assign.value)
		{
			if (auto const* call = std::get_if<solidity::yul::FunctionCall>(_assign.value.get()))
			{
				std::string callName = getFunctionName(call->functionName);
				if (m_asmFunctions.count(callName))
				{
					auto const& funcDef = *m_asmFunctions[callName];

					// Right-to-left: Yul argument evaluation order.
					std::vector<std::shared_ptr<awst::Expression>> args(call->arguments.size());
					for (size_t ai = call->arguments.size(); ai-- > 0; )
						args[ai] = buildExpression(call->arguments[ai]);

					handleUserFunctionCall(callName, args, loc, _out);

					bool fromSub = !m_yulSubReturnTemps.empty();
					size_t numReturns = std::min(
						_assign.variableNames.size(), funcDef.returnVariables.size()
					);
					for (size_t i = 0; i < numReturns; ++i)
					{
						std::string retName = fromSub
							? m_yulSubReturnTemps[i]
							: funcDef.returnVariables[i].name.str();
						std::string varName = resolveVarRef(_assign.variableNames[i]);
						m_localConstants.erase(varName); // reassigned → any recorded constant is stale

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

	std::string name = resolveVarRef(_assign.variableNames[0]);

	// Reassigned → any recorded constant is stale. Calldata param/pointer names
	// are exempt: their entries are HEAD OFFSETS owned by the calldata machinery
	// (repoints go through the mutable __cd_off_/__cd_len_ locals instead).
	if (!m_calldataParamNames.count(name) && !m_calldataStaticPtrNames.count(name))
		m_localConstants.erase(name);

	// Bare STATIC calldata pointer write (`s := s2`, `s2 := 4`): repoint —
	// assign the mutable __cd_off_<name> local; later reads (asm or Solidity
	// member access through the live pointer) follow the new offset.
	if (m_useSyntheticCalldata && m_calldataStaticPtrNames.count(name) && _assign.value)
	{
		auto rhs = buildExpression(*_assign.value);
		drainPendingStatements(_out);
		if (!rhs)
			return;
		if (m_seededCalldataPointers)
			m_seededCalldataPointers->insert(name);
		_out.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression("__cd_off_" + name, awst::WType::biguintType(), loc),
			std::move(rhs), loc));
		return;
	}

	// fn-ptr writes: fp.selector := expr / fp.address := expr
	// → replace3 the 4- or 8-byte slice of the 12-byte fn-ptr local.
	{
		auto dotIdx = name.rfind('.');
		if (dotIdx != std::string::npos && _assign.value)
		{
			std::string suffix = name.substr(dotIdx + 1);
			std::string baseName = name.substr(0, dotIdx);
			// Dynamic calldata param: `x.offset := V` / `x.length := L` repoints x within __cd_blob —
			// write the mutable pointer local so later reads / value-extracts see the new range.
			if ((suffix == "offset" || suffix == "length") && _assign.value)
			{
				auto typeIt = m_locals.find(baseName);
				bool isCdPtr = (typeIt != m_locals.end() && isDynamicCalldataType(typeIt->second))
					|| m_calldataPointerNames.count(baseName);
				if (m_useSyntheticCalldata && isCdPtr)
				{
					auto rhs = buildExpression(*_assign.value);
					drainPendingStatements(_out);
					if (!rhs)
						return;
					std::string local = (suffix == "offset" ? "__cd_off_" : "__cd_len_") + baseName;
					// Mark the pointer locals LIVE: later blocks must not re-seed over
					// this write, and value reads of the param (return x) now go
					// through extract3(__cd_blob, off, len).
					if (m_seededCalldataPointers)
						m_seededCalldataPointers->insert(baseName);
					_out.push_back(awst::makeAssignmentStatement(
						awst::makeVarExpression(local, awst::WType::biguintType(), loc),
						std::move(rhs), loc));
					return;
				}
			}
			if (suffix == "selector" || suffix == "address")
			{
				auto fullIt = m_locals.find(name);
				if (fullIt != m_locals.end())
				{
					auto const* bwt = dynamic_cast<awst::BytesWType const*>(fullIt->second);
					if (bwt && bwt->length().has_value() && *bwt->length() == 12)
					{
						auto rhs = buildExpression(*_assign.value);
						drainPendingStatements(_out);
						if (!rhs)
							return;

						int sliceWidth = (suffix == "selector") ? 4 : 8;
						int sliceOffset = (suffix == "selector") ? 8 : 0;

						// Slice to exactly sliceWidth bytes:
						// - bytes/account: take low sliceWidth bytes (EVM right-aligns addresses).
						// - numeric: itob(8) then extract low sliceWidth.
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

	if (name.find(".slot") != std::string::npos)
	{
		std::string baseName = name.substr(0, name.find(".slot"));
		if (!baseName.empty() && _assign.value)
		{
			auto slotExpr = buildExpression(*_assign.value);
			if (slotExpr)
			{
				drainPendingStatements(_out); // early-return bypasses normal drain
				if (slotExpr->wtype == awst::WType::uint64Type())
				{
					auto itob = awst::makeItob(std::move(slotExpr), loc);
					slotExpr = awst::makeAsBiguint(std::move(itob), loc);
				}
				else if (slotExpr->wtype != awst::WType::biguintType())
				{
					slotExpr = awst::makeAsBiguint(std::move(slotExpr), loc);
				}

				auto target = awst::makeVarExpression(baseName, awst::WType::biguintType(), loc);

				auto assign = awst::makeAssignmentStatement(std::move(target), std::move(slotExpr), loc);
				_out.push_back(std::move(assign));
			}
		}
		return;
	}

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

	// Signed intN (N<=64) local: writes land on its biguint shadow (the raw Yul
	// word — see the buildBlock prologue); the typed local refreshes at block exit.
	if (auto shIt = m_signedShadow.find(name); shIt != m_signedShadow.end())
		name = shIt->second;

	auto it = m_locals.find(name);
	auto const* wtype = (it != m_locals.end()) ? it->second : awst::WType::biguintType();
	auto target = awst::makeVarExpression(name, wtype, loc);

	auto value = buildExpression(*_assign.value);
	drainPendingStatements(_out);

	if (!value)
		value = awst::makeZero(loc, target->wtype);

	if (target->wtype != value->wtype)
	{
		if (target->wtype == awst::WType::biguintType())
		{
			value = ensureBiguint(std::move(value), loc);
		}
		else if (target->wtype == awst::WType::boolType())
		{
			value = ensureBool(std::move(value), loc);
		}
		else if (target->wtype->kind() == awst::WTypeKind::Bytes)
		{
			auto const* bytesType = dynamic_cast<awst::BytesWType const*>(target->wtype);
			// bytes[N]: pad biguint to 32 bytes then extract first N (EVM: left-aligned).
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
				auto biguintVal = ensureBiguint(std::move(value), loc);
				auto cast = awst::makeReinterpretCast(std::move(biguintVal), target->wtype, loc);
				value = std::move(cast);
			}
		}
		else if (target->wtype == awst::WType::accountType())
		{
			auto biguintVal = ensureBiguint(std::move(value), loc);
			auto padded = padTo32Bytes(std::move(biguintVal), loc);
			auto cast = awst::makeAsAccount(std::move(padded), loc);
			value = std::move(cast);
		}
		else if (target->wtype == awst::WType::uint64Type())
		{
			// Truncate biguint→uint64 via safeBtoi (keeps phi-node types consistent).
			if (value->wtype == awst::WType::biguintType())
			{
				// Sub-64-bit Solidity type (uint8/16/32): mask before btoi.
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
			if (value->wtype == awst::WType::biguintType())
			{
				value = awst::makeAsAccount(awst::makeAsBytes(std::move(value), loc), loc);
			}
			else if (value->wtype != awst::WType::accountType())
			{
				auto cast = awst::makeAsAccount(std::move(value), loc);
				value = std::move(cast);
			}
		}
		else
		{
			// Pointer reinterpretation (e.g. address[] ↔ bytes32[] — same EVM layout).
			auto const* targetArr = dynamic_cast<awst::ReferenceArray const*>(target->wtype);
			auto const* valueArr = dynamic_cast<awst::ReferenceArray const*>(value->wtype);
			if (targetArr && valueArr)
			{
				value->wtype = target->wtype;
				if (auto* srcVar = dynamic_cast<awst::VarExpression*>(value.get()))
					m_locals[srcVar->name] = target->wtype;
			}
			else
			{
				Logger::instance().debug(
					"assembly type coercion: " + value->wtype->name() + " → " + target->wtype->name()
				);
				// Don't mutate IntegerConstant wtype to struct/array — puya rejects it.
				// Wrap with ReinterpretCast instead.
				if (dynamic_cast<awst::IntegerConstant const*>(value.get()))
					value = awst::makeReinterpretCast(std::move(value), target->wtype, loc);
				else
					value->wtype = target->wtype;
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

	if (auto const* call = std::get_if<solidity::yul::FunctionCall>(&_stmt.expression))
	{
		std::string funcName = getFunctionName(call->functionName);

		// Pattern checks that need the raw Yul AST must run before arg translation.
		if (funcName == "mstore" && tryHandleBytesMemoryWrite(*call, loc, _out))
			return;
		if (funcName == "mstore8" && tryHandleBytesMemoryWrite8(*call, loc, _out))
			return;
		if (funcName == "mcopy" && tryHandleBytesMemoryMcopy(*call, loc, _out))
			return;

		// pop(call(...)) / pop(staticcall(...)): the discarded value is the call's
		// success flag, but the call's SIDE EFFECTS (inner txn + returndata
		// output-copy) must still happen. Route to the full app-call lowering with
		// NO assign target (its output-copy is not gated on the target). This is
		// exactly the shape UnusedPruner produces from `let unused := call(...)`,
		// and the common `pop(call(...))` idiom for calls whose success is ignored.
		if (funcName == "pop" && call->arguments.size() == 1)
			if (auto const* inner = std::get_if<solidity::yul::FunctionCall>(&call->arguments[0]))
			{
				std::string innerName = getFunctionName(inner->functionName);
				if (innerName == "call" || innerName == "staticcall")
				{
					handlePrecompileCall(*inner, "", loc, _out, /*_isCall=*/innerName == "call");
					return;
				}
			}

		// Right-to-left: Yul argument evaluation order (see CoreTranslation).
		std::vector<std::shared_ptr<awst::Expression>> args(call->arguments.size());
		for (size_t ai = call->arguments.size(); ai-- > 0; )
			args[ai] = buildExpression(call->arguments[ai]);
		drainPendingStatements(_out);

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
			if (!tryHandleStateVarSstore(*call, args, loc, _out))
				handleSstore(args, loc, _out);
			return;
		}
		if (funcName == "invalid")
		{
			auto stmt = awst::makeExpressionStatement(awst::makeAssert(awst::makeFalse(loc), loc, "invalid"), loc);
			_out.push_back(std::move(stmt));
			return;
		}
		if (funcName == "stop")
		{
			auto retStmt = awst::makeReturnStatement(nullptr, loc);
			_out.push_back(std::move(retStmt));
			return;
		}
		if (funcName == "returndatacopy")
		{
			emitReturndatacopy(args, loc, _out);
			return;
		}
		if (funcName == "pop")
			return; // discard value, no-op

		if (funcName == "delegatecall")
		{
			// No AVM equivalent; stubbing as no-op would silently drop the call.
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
			// Constant length: unroll full words to mstore(mload) copies (forward
			// order: correct for non-overlapping/copy-down), then stitch a sub-word
			// tail via word read-modify-write. Dynamic/huge lengths hard-error
			// (old code silently dropped bytes past the first word for len > 32).
			auto const* lenConst = (args.size() >= 3)
				? dynamic_cast<awst::IntegerConstant const*>(args[2].get())
				: nullptr;
			if (!lenConst)
			{
				Logger::instance().error(
					"mcopy with a dynamic length is not supported in the "
					"scratch-slot memory model (use a compile-time multiple "
					"of 32 bytes, or a `bytes memory` mcopy)", loc);
				return;
			}
			solidity::u256 lenVal(lenConst->value);
			if (lenVal == 0)
				return;  // no-op (mcopy_empty pattern)
			if (lenVal > 4096)
			{
				Logger::instance().error(
					"mcopy length too large to unroll in the scratch-slot "
					"memory model (max 4096 bytes)", loc);
				return;
			}
			if (args.size() >= 2)
			{
				auto atOff = [&](std::shared_ptr<awst::Expression> const& base,
					unsigned long long delta) -> std::shared_ptr<awst::Expression>
				{
					if (delta == 0)
						return base;
					return makeBigUIntBinOp(base,
						awst::BigUIntBinaryOperator::Add,
						awst::makeBiguintConstant(std::to_string(delta), loc), loc);
				};
				auto nwords = (lenVal / 32).convert_to<unsigned long long>();
				auto r = (lenVal % 32).convert_to<unsigned>();
				// Memmove semantics (M13): snapshot ALL source words into
				// temps BEFORE any write — the interleaved mload/mstore
				// forward loop corrupted overlapping ranges (dst inside src).
				static int s_mcopyCtr = 0;
				std::vector<std::pair<unsigned long long, std::string>> srcWords;
				for (unsigned long long w = 0; w < nwords; ++w)
				{
					auto mloadArgs = std::vector<std::shared_ptr<awst::Expression>>{atOff(args[1], 32 * w)};
					auto loadedVal = handleMload(mloadArgs, loc);
					if (!loadedVal)
						continue;
					std::string vn = "__mcopy_w_" + std::to_string(s_mcopyCtr++);
					auto const* wt = loadedVal->wtype;
					_out.push_back(awst::makeAssignmentStatement(
						awst::makeVarExpression(vn, wt, loc), std::move(loadedVal), loc));
					srcWords.emplace_back(w, vn);
					m_locals[vn] = wt;
				}
				std::string tailVn;
				if (r != 0)
				{
					tailVn = "__mcopy_w_" + std::to_string(s_mcopyCtr++);
					auto srcWord = readMemWordDyn(atOff(args[1], 32 * nwords), loc);
					_out.push_back(awst::makeAssignmentStatement(
						awst::makeVarExpression(tailVn, awst::WType::bytesType(), loc),
						std::move(srcWord), loc));
					m_locals[tailVn] = awst::WType::bytesType();
				}
				for (auto const& [w, vn]: srcWords)
				{
					auto storeArgs = std::vector<std::shared_ptr<awst::Expression>>{
						atOff(args[0], 32 * w),
						awst::makeVarExpression(vn, m_locals[vn], loc)};
					handleMstore(storeArgs, loc, _out);
				}
				// Sub-word tail: splice the first r bytes of the snapshotted
				// src word over the dst word (read after the full-word writes
				// — the dst tail word lies beyond them).
				if (r != 0)
				{
					auto dstWord = readMemWordDyn(atOff(args[0], 32 * nwords), loc);
					auto stitched = awst::makeConcat(
						awst::makeExtract(
							awst::makeVarExpression(tailVn, awst::WType::bytesType(), loc),
							0, static_cast<int>(r), loc),
						awst::makeExtract(std::move(dstWord), static_cast<int>(r),
							static_cast<int>(32 - r), loc),
						loc);
					writeMemWordDyn(atOff(args[0], 32 * nwords), std::move(stitched), loc, _out);
				}
			}
			return;
		}

		auto asmIt = m_asmFunctions.find(funcName);
		if (asmIt != m_asmFunctions.end())
		{
			handleUserFunctionCall(funcName, args, loc, _out);
			return;
		}

		auto expr = buildExpression(_stmt.expression);
		if (expr)
		{
			auto exprStmt = awst::makeExpressionStatement(std::move(expr), loc);
			_out.push_back(std::move(exprStmt));
		}
	}
	else
	{
		auto expr = buildExpression(_stmt.expression);
		if (expr)
		{
			auto exprStmt = awst::makeExpressionStatement(std::move(expr), loc);
			_out.push_back(std::move(exprStmt));
		}
	}
}

bool AssemblyBuilder::tryHandleStateVarSstore(
	solidity::yul::FunctionCall const& _call,
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out)
{
	if (_call.arguments.empty() || _args.size() < 2)
		return false;
	auto const* id = std::get_if<solidity::yul::Identifier>(&_call.arguments[0]);
	if (!id)
		return false;
	auto it = m_stateVarSlots.find(id->name.str());
	if (it == m_stateVarSlots.end())
		return false;
	auto const& sv = it->second;
	auto key = awst::makeUtf8BytesConstant(sv.varName, _loc, awst::WType::stateKeyType());
	auto target = awst::makeAppStateExpression(key, sv.wtype, _loc);
	auto value = ensureBiguint(_args[1], _loc);
	auto assign = awst::makeAssignmentExpression(std::move(target), std::move(value), _loc, sv.wtype);
	_out.push_back(awst::makeExpressionStatement(std::move(assign), _loc));
	return true;
}

std::shared_ptr<awst::Expression> AssemblyBuilder::tryHandleStateVarSload(
	solidity::yul::FunctionCall const& _call,
	awst::SourceLocation const& _loc)
{
	if (_call.arguments.empty())
		return nullptr;
	auto const* id = std::get_if<solidity::yul::Identifier>(&_call.arguments[0]);
	if (!id)
		return nullptr;
	auto it = m_stateVarSlots.find(id->name.str());
	if (it == m_stateVarSlots.end())
		return nullptr;
	auto const& sv = it->second;
	auto key = awst::makeUtf8BytesConstant(sv.varName, _loc, awst::WType::stateKeyType());
	return awst::makeAppStateExpression(std::move(key), sv.wtype, _loc);
}


} // namespace puyasol::builder
