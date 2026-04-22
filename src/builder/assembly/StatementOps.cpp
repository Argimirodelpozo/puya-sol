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
			{
				// Yul if (no else)
				auto loc = makeLoc(_node.debugData);

				// Check if body is a revert-only block (common SafeCast/require pattern).
				// Emitting assert(NOT(cond)) directly avoids puya DCE of if(cond){assert(false)}.
				bool isRevertBody = false;
				for (auto const& stmt : _node.body.statements)
				{
					if (auto const* exprStmt = std::get_if<solidity::yul::ExpressionStatement>(&stmt))
					{
						if (auto const* funcCall = std::get_if<solidity::yul::FunctionCall>(&exprStmt->expression))
						{
							if (getFunctionName(funcCall->functionName) == "revert")
								isRevertBody = true;
						}
					}
				}

				if (isRevertBody)
				{
					// Emit assert(NOT(condition)) — avoids DCE of if(cond){assert(false)}
					auto cond = ensureBool(buildExpression(*_node.condition), loc);
					auto notCond = std::make_shared<awst::Not>();
					notCond->sourceLocation = loc;
					notCond->wtype = awst::WType::boolType();
					notCond->expr = std::move(cond);

					auto stmt = awst::makeExpressionStatement(awst::makeAssert(std::move(notCond), loc, "revert"), loc);
					_out.push_back(std::move(stmt));
				}
				else
				{
					// Original IfElse path for non-revert if-bodies
					auto ifElse = std::make_shared<awst::IfElse>();
					ifElse->sourceLocation = loc;
					ifElse->condition = ensureBool(buildExpression(*_node.condition), loc);

					auto ifBlock = std::make_shared<awst::Block>();
					ifBlock->sourceLocation = loc;
					for (auto const& innerStmt: _node.body.statements)
						buildStatement(innerStmt, ifBlock->body);
					ifElse->ifBranch = std::move(ifBlock);

					_out.push_back(std::move(ifElse));
				}
			}
			else if constexpr (std::is_same_v<T, solidity::yul::ForLoop>)
			{
				auto loc = makeLoc(_node.debugData);
				// Translate pre block
				for (auto const& preStmt: _node.pre.statements)
					buildStatement(preStmt, _out);

				auto loop = std::make_shared<awst::WhileLoop>();
				loop->sourceLocation = loc;
				loop->condition = ensureBool(buildExpression(*_node.condition), loc);

				// Set post statements so `continue` can emit them
				auto* savedPost = m_forLoopPost;
				m_forLoopPost = &_node.post.statements;

				auto body = std::make_shared<awst::Block>();
				body->sourceLocation = loc;
				for (auto const& bodyStmt: _node.body.statements)
					buildStatement(bodyStmt, body->body);
				// Post statements at end of body (normal iteration path)
				for (auto const& postStmt: _node.post.statements)
					buildStatement(postStmt, body->body);
				loop->loopBody = std::move(body);

				m_forLoopPost = savedPost;
				_out.push_back(std::move(loop));
			}
			else if constexpr (std::is_same_v<T, solidity::yul::Break>)
			{
				auto stmt = std::make_shared<awst::LoopExit>();
				stmt->sourceLocation = makeLoc(_node.debugData);
				_out.push_back(std::move(stmt));
			}
			else if constexpr (std::is_same_v<T, solidity::yul::Continue>)
			{
				// In Yul, `continue` jumps to the for-loop's post expression,
				// not the condition. Emit post statements before LoopContinue.
				if (m_forLoopPost)
				{
					for (auto const& postStmt: *m_forLoopPost)
						buildStatement(postStmt, _out);
				}
				auto stmt = std::make_shared<awst::LoopContinue>();
				stmt->sourceLocation = makeLoc(_node.debugData);
				_out.push_back(std::move(stmt));
			}
			else if constexpr (std::is_same_v<T, solidity::yul::Leave>)
			{
				// Leave = early exit from a Yul function. We inline Yul
				// functions wrapped in `while true { … break }`, so a
				// `leave` is just a break out of that wrapper loop.
				// Outside of an inlined function it has no meaningful
				// translation — emit a no-op (Solidity wouldn't even
				// parse this case).
				if (m_inlineDepth > 0)
				{
					auto stmt = std::make_shared<awst::LoopExit>();
					stmt->sourceLocation = makeLoc(_node.debugData);
					_out.push_back(std::move(stmt));
				}
			}
			else if constexpr (std::is_same_v<T, solidity::yul::Switch>)
			{
				auto loc = makeLoc(_node.debugData);
				auto switchExpr = buildExpression(*_node.expression);

				// Build AWST Switch node from Yul switch cases.
				// AVM `match` does exact byte comparison. Yul values are u256 (32 bytes),
				// so ARC4 uint256 parameters decode to 32-byte biguint. We must ensure
				// both the switch expression and case constants use the same 32-byte
				// encoding. Cast switch expr to bytes and use 32-byte BytesConstants.
				bool useBytesMatch = switchExpr->wtype
					&& switchExpr->wtype->name() == "biguint";
				bool useBoolMatch = switchExpr->wtype
					&& switchExpr->wtype->name() == "bool";

				auto switchNode = std::make_shared<awst::Switch>();
				switchNode->sourceLocation = loc;

				if (useBytesMatch)
				{
					auto cast = awst::makeReinterpretCast(switchExpr, awst::WType::bytesType(), loc);

					// Normalise to a fixed 32-byte big-endian encoding —
					// biguint ABI decoding may produce 64-byte values (our
					// uint512 mapping) but the case constants below are 32
					// bytes. Pattern: b| bzero(32) → ensures at least 32
					// bytes, then extract the last 32.
					auto bzero32 = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), loc);
					auto size32 = awst::makeIntegerConstant("32", loc);
					bzero32->stackArgs.push_back(size32);

					auto bor = awst::makeIntrinsicCall("b|", awst::WType::bytesType(), loc);
					bor->stackArgs.push_back(std::move(bzero32));
					bor->stackArgs.push_back(std::move(cast));

					auto lenCall = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), loc);
					lenCall->stackArgs.push_back(bor);

					auto minus = std::make_shared<awst::UInt64BinaryOperation>();
					minus->sourceLocation = loc;
					minus->wtype = awst::WType::uint64Type();
					minus->left = std::move(lenCall);
					minus->op = awst::UInt64BinaryOperator::Sub;
					auto thirtyTwo = awst::makeIntegerConstant("32", loc);
					minus->right = thirtyTwo;

					auto width = awst::makeIntegerConstant("32", loc);

					auto extract = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), loc);
					extract->stackArgs.push_back(std::move(bor));
					extract->stackArgs.push_back(std::move(minus));
					extract->stackArgs.push_back(std::move(width));

					switchNode->value = std::move(extract);
				}
				else
				{
					switchNode->value = switchExpr;
				}

				for (auto const& yulCase: _node.cases)
				{
					if (!yulCase.value)
					{
						auto caseBlock = std::make_shared<awst::Block>();
						caseBlock->sourceLocation = makeLoc(yulCase.debugData);
						for (auto const& stmt: yulCase.body.statements)
							buildStatement(stmt, caseBlock->body);
						switchNode->defaultCase = std::move(caseBlock);
					}
					else
					{
						auto caseBlock = std::make_shared<awst::Block>();
						caseBlock->sourceLocation = makeLoc(yulCase.debugData);
						for (auto const& stmt: yulCase.body.statements)
							buildStatement(stmt, caseBlock->body);

						if (useBytesMatch
							&& yulCase.value->kind == solidity::yul::LiteralKind::Number)
						{
							// 32-byte big-endian BytesConstant matching ARC4 uint256 encoding
							auto const& val = yulCase.value->value.value();
							auto be = solidity::toBigEndian(val);
							switchNode->cases.emplace_back(
								awst::makeBytesConstant(
									std::vector<uint8_t>(be.begin(), be.end()),
									makeLoc(yulCase.value->debugData)),
								std::move(caseBlock));
						}
						else if (useBoolMatch
							&& yulCase.value->kind == solidity::yul::LiteralKind::Number)
						{
							// Convert numeric case to BoolConstant for bool switch
							auto const& val = yulCase.value->value.value();
							switchNode->cases.emplace_back(
								awst::makeBoolConstant(val != 0, makeLoc(yulCase.value->debugData)),
								std::move(caseBlock));
						}
						else
						{
							auto caseVal = buildLiteral(*yulCase.value);
							switchNode->cases.emplace_back(
								std::move(caseVal), std::move(caseBlock));
						}
					}
				}

				_out.push_back(std::move(switchNode));
			}
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
				auto zero = awst::makeIntegerConstant("0", loc, awst::WType::biguintType());
				value = std::move(zero);
			}
		}
		else
		{
			// Default: zero
			auto zero = awst::makeIntegerConstant("0", loc, awst::WType::biguintType());
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
					auto itob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), loc);
					itob->stackArgs.push_back(std::move(slotExpr));
					auto cast = awst::makeReinterpretCast(std::move(itob), awst::WType::biguintType(), loc);
					slotExpr = std::move(cast);
				}
				else if (slotExpr->wtype != awst::WType::biguintType())
				{
					// bytes[N] or other non-biguint → reinterpret as biguint
					auto cast = awst::makeReinterpretCast(std::move(slotExpr), awst::WType::biguintType(), loc);
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

	auto target = std::make_shared<awst::VarExpression>();
	target->sourceLocation = loc;
	target->name = name;

	auto it = m_locals.find(name);
	target->wtype = (it != m_locals.end()) ? it->second : awst::WType::biguintType();

	auto value = buildExpression(*_assign.value);
	// Drain any pending statements from inlined assembly functions
	for (auto& ps: m_pendingStatements)
		_out.push_back(std::move(ps));
	m_pendingStatements.clear();

	if (!value)
	{
		// Expression failed to translate (error already logged), use zero fallback
		auto zero = awst::makeIntegerConstant("0", loc, target->wtype);
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
				auto zero = awst::makeIntegerConstant("0", loc);
				auto lenConst = awst::makeIntegerConstant(std::to_string(n), loc);
				auto extract = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), loc);
				extract->stackArgs.push_back(std::move(padded));
				extract->stackArgs.push_back(std::move(zero));
				extract->stackArgs.push_back(std::move(lenConst));
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
			auto cast = awst::makeReinterpretCast(std::move(padded), awst::WType::accountType(), loc);
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
				auto toBytes = awst::makeReinterpretCast(std::move(value), awst::WType::bytesType(), loc);

				auto toAccount = awst::makeReinterpretCast(std::move(toBytes), awst::WType::accountType(), loc);
				value = std::move(toAccount);
			}
			else if (value->wtype != awst::WType::accountType())
			{
				auto cast = awst::makeReinterpretCast(std::move(value), awst::WType::accountType(), loc);
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
			auto stmt = awst::makeExpressionStatement(awst::makeAssert(awst::makeBoolConstant(false, loc), loc, "invalid"), loc);
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
		if (funcName == "returndatacopy" || funcName == "pop")
		{
			// No-op on AVM (returndatacopy: no return data; pop: discard value)
			return;
		}
		if (funcName == "delegatecall")
		{
			// delegatecall has no AVM equivalent — stub as no-op
			Logger::instance().warning(
				"delegatecall() has no AVM equivalent — stubbed as no-op",
				loc
			);
			return;
		}
		if (funcName == "mcopy")
		{
			// mcopy(dst, src, length): copy memory slot src to dst
			// In our memory-slot model, this is equivalent to mstore(dst, mload(src))
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

void AssemblyBuilder::buildFunctionDefinition(
	solidity::yul::FunctionDefinition const& _def,
	std::vector<std::shared_ptr<awst::Statement>>& /*_out*/
)
{
	// Function definitions are collected in the first pass and inlined at call sites.
	// Nothing to emit here.
	auto loc = makeLoc(_def.debugData);
	Logger::instance().debug(
		"assembly function '" + _def.name.str() + "' collected for inlining", loc
	);
}

// ─── Assembly function inlining ─────────────────────────────────────────────

std::shared_ptr<awst::Expression> AssemblyBuilder::handleUserFunctionCall(
	std::string const& _name,
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	// Recursive Yul functions are lowered to AWST Subroutines (emitted in
	// AssemblyBuilder::buildBlock). Dispatch via SubroutineCallExpression
	// instead of inlining so calls don't recurse at compile time.
	auto subIt = m_yulFuncSubroutineIds.find(_name);
	if (subIt != m_yulFuncSubroutineIds.end())
	{
		auto defIt = m_asmFunctions.find(_name);
		if (defIt == m_asmFunctions.end())
		{
			Logger::instance().error("unknown assembly function: " + _name, _loc);
			return nullptr;
		}
		auto const& funcDef = *defIt->second;
		if (_args.size() != funcDef.parameters.size())
		{
			Logger::instance().error(
				"assembly function '" + _name + "' called with wrong number of arguments", _loc);
			return nullptr;
		}

		auto call = std::make_shared<awst::SubroutineCallExpression>();
		call->sourceLocation = _loc;
		call->wtype = funcDef.returnVariables.size() == 1
			? awst::WType::biguintType()
			: awst::WType::voidType();
		call->target = awst::SubroutineID{subIt->second};
		for (auto const& a: _args)
		{
			awst::CallArg ca;
			ca.value = ensureBiguint(a, _loc);
			call->args.push_back(std::move(ca));
		}

		if (funcDef.returnVariables.size() == 1)
		{
			std::string retName = funcDef.returnVariables[0].name.str();
			m_locals[retName] = awst::WType::biguintType();
			auto target = awst::makeVarExpression(retName, awst::WType::biguintType(), _loc);
			auto assign = awst::makeAssignmentStatement(std::move(target), call, _loc);
			_out.push_back(std::move(assign));
			return awst::makeVarExpression(retName, awst::WType::biguintType(), _loc);
		}

		auto exprStmt = awst::makeExpressionStatement(call, _loc);
		_out.push_back(std::move(exprStmt));
		return std::make_shared<awst::VoidConstant>();
	}

	// Recursion guard: Yul function inlining expands each call at the AST
	// level. Recursive Yul functions (e.g. `function fac(n) -> nf { ... fac(sub(n,1)) ... }`)
	// otherwise recurse forever here and blow the C++ stack. Emit an error
	// and bail out of the call path so the rest of the contract at least
	// compiles far enough to report a meaningful diagnostic.
	if (m_inlineDepth > 64)
	{
		Logger::instance().error(
			"assembly function '" + _name + "' recurses deeper than the inlining "
			"limit (64 frames); recursive Yul functions are not supported on AVM",
			_loc
		);
		return nullptr;
	}

	auto it = m_asmFunctions.find(_name);
	if (it == m_asmFunctions.end())
	{
		Logger::instance().error("unknown assembly function: " + _name, _loc);
		return nullptr;
	}

	auto const& funcDef = *it->second;

	// Inline: bind parameters to arguments via assignment statements
	if (_args.size() != funcDef.parameters.size())
	{
		Logger::instance().error(
			"assembly function '" + _name + "' called with wrong number of arguments", _loc
		);
		return nullptr;
	}

	// Assign parameters and propagate constant values
	for (size_t i = 0; i < funcDef.parameters.size(); ++i)
	{
		std::string paramName = funcDef.parameters[i].name.str();
		// Use the argument's actual type (handles arrays passed to assembly functions)
		awst::WType const* paramType = _args[i]->wtype;
		m_locals[paramName] = paramType;

		// Propagate constant values from arguments
		auto constVal = resolveConstantOffset(_args[i]);
		if (constVal)
			m_localConstants[paramName] = *constVal;

		auto target = awst::makeVarExpression(paramName, paramType, _loc);

		auto assign = awst::makeAssignmentStatement(std::move(target), _args[i], _loc);
		_out.push_back(std::move(assign));
	}

	// Initialize return variables to zero
	for (auto const& retVar: funcDef.returnVariables)
	{
		std::string retName = retVar.name.str();
		m_locals[retName] = awst::WType::biguintType();

		auto target = awst::makeVarExpression(retName, awst::WType::biguintType(), _loc);

		auto zero = awst::makeIntegerConstant("0", _loc, awst::WType::biguintType());

		auto assign = awst::makeAssignmentStatement(std::move(target), std::move(zero), _loc);
		_out.push_back(std::move(assign));
	}

	// Translate the function body inline. Yul allows `leave` to early-exit
	// the function; we wrap the body in a single-iteration `while true { … }`
	// loop so that `leave` (translated to LoopExit) breaks out of just the
	// inlined body, without affecting any enclosing Solidity-level loops.
	bool hasLeave = false;
	std::function<void(std::vector<solidity::yul::Statement> const&)> scanLeave =
		[&](std::vector<solidity::yul::Statement> const& stmts)
	{
		for (auto const& s: stmts)
		{
			if (hasLeave) return;
			if (std::holds_alternative<solidity::yul::Leave>(s))
			{
				hasLeave = true;
				return;
			}
			if (auto const* blk = std::get_if<solidity::yul::Block>(&s))
				scanLeave(blk->statements);
			else if (auto const* iff = std::get_if<solidity::yul::If>(&s))
				scanLeave(iff->body.statements);
			else if (auto const* sw = std::get_if<solidity::yul::Switch>(&s))
				for (auto const& c: sw->cases)
					scanLeave(c.body.statements);
		}
	};
	scanLeave(funcDef.body.statements);

	std::vector<std::shared_ptr<awst::Statement>> bodyStmts;
	++m_inlineDepth;
	for (auto const& stmt: funcDef.body.statements)
	{
		buildStatement(stmt, bodyStmts);
		// Top-level `leave` makes the rest of the function body
		// unreachable; puya rejects unreachable code outright. Stop
		// translating once we see the top-level leave (nested leaves
		// inside if/switch keep emitting subsequent statements).
		if (std::holds_alternative<solidity::yul::Leave>(stmt))
			break;
	}
	--m_inlineDepth;

	if (hasLeave)
	{
		// Wrap in `while true { body; break; }` so leave→LoopExit works.
		auto loop = std::make_shared<awst::WhileLoop>();
		loop->sourceLocation = _loc;

		loop->condition = awst::makeBoolConstant(true, _loc);

		auto block = std::make_shared<awst::Block>();
		block->sourceLocation = _loc;
		block->body = std::move(bodyStmts);

		auto exit = std::make_shared<awst::LoopExit>();
		exit->sourceLocation = _loc;
		block->body.push_back(std::move(exit));

		loop->loopBody = std::move(block);
		_out.push_back(std::move(loop));
	}
	else
	{
		for (auto& s: bodyStmts)
			_out.push_back(std::move(s));
	}

	return nullptr;
}

} // namespace puyasol::builder
