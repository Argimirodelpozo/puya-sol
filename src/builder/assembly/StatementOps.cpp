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
							if (funcCall->functionName.name.str() == "revert")
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

					auto assertExpr = std::make_shared<awst::AssertExpression>();
					assertExpr->sourceLocation = loc;
					assertExpr->wtype = awst::WType::voidType();
					assertExpr->condition = std::move(notCond);
					assertExpr->errorMessage = "revert";

					auto stmt = std::make_shared<awst::ExpressionStatement>();
					stmt->sourceLocation = loc;
					stmt->expr = std::move(assertExpr);
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

				auto body = std::make_shared<awst::Block>();
				body->sourceLocation = loc;
				for (auto const& bodyStmt: _node.body.statements)
					buildStatement(bodyStmt, body->body);
				for (auto const& postStmt: _node.post.statements)
					buildStatement(postStmt, body->body);
				loop->loopBody = std::move(body);

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
				auto stmt = std::make_shared<awst::LoopContinue>();
				stmt->sourceLocation = makeLoc(_node.debugData);
				_out.push_back(std::move(stmt));
			}
			else if constexpr (std::is_same_v<T, solidity::yul::Leave>)
			{
				// Leave = return from assembly function; handled as a return
				auto ret = std::make_shared<awst::ReturnStatement>();
				ret->sourceLocation = makeLoc(_node.debugData);
				_out.push_back(std::move(ret));
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
					auto cast = std::make_shared<awst::ReinterpretCast>();
					cast->sourceLocation = loc;
					cast->wtype = awst::WType::bytesType();
					cast->expr = switchExpr;
					switchNode->value = cast;
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
							auto caseVal = std::make_shared<awst::BytesConstant>();
							caseVal->sourceLocation = makeLoc(yulCase.value->debugData);
							caseVal->wtype = awst::WType::bytesType();
							caseVal->encoding = awst::BytesEncoding::Base16;
							caseVal->value.assign(be.begin(), be.end());
							switchNode->cases.emplace_back(
								std::move(caseVal), std::move(caseBlock));
						}
						else if (useBoolMatch
							&& yulCase.value->kind == solidity::yul::LiteralKind::Number)
						{
							// Convert numeric case to BoolConstant for bool switch
							auto const& val = yulCase.value->value.value();
							auto caseVal = std::make_shared<awst::BoolConstant>();
							caseVal->sourceLocation = makeLoc(yulCase.value->debugData);
							caseVal->wtype = awst::WType::boolType();
							caseVal->value = (val != 0);
							switchNode->cases.emplace_back(
								std::move(caseVal), std::move(caseBlock));
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
	if (_decl.value && _decl.variables.size() == 1)
	{
		if (auto const* call = std::get_if<solidity::yul::FunctionCall>(_decl.value.get()))
		{
			std::string callName = call->functionName.name.str();
			if (callName == "staticcall" || callName == "call")
			{
				std::string varName = _decl.variables[0].name.str();
				handlePrecompileCall(*call, varName, loc, _out, /*_isCall=*/callName == "call");
				return;
			}

			// User-defined assembly function called in variable declaration context
			// (e.g., let isValid := checkPairing(...))
			if (m_asmFunctions.count(callName))
			{
				std::string varName = _decl.variables[0].name.str();
				m_locals[varName] = awst::WType::biguintType();

				// Translate arguments
				std::vector<std::shared_ptr<awst::Expression>> args;
				for (auto const& arg: call->arguments)
					args.push_back(buildExpression(arg));

				// Inline the function (writes to return variable)
				handleUserFunctionCall(callName, args, loc, _out);

				// The return variable of the inlined function is the result.
				// Find its name from the function definition's return variables.
				auto const& funcDef = *m_asmFunctions[callName];
				if (!funcDef.returnVariables.empty())
				{
					std::string retName = funcDef.returnVariables[0].name.str();
					auto retVar = std::make_shared<awst::VarExpression>();
					retVar->sourceLocation = loc;
					retVar->name = retName;
					retVar->wtype = awst::WType::biguintType();

					auto target = std::make_shared<awst::VarExpression>();
					target->sourceLocation = loc;
					target->name = varName;
					target->wtype = awst::WType::biguintType();

					auto assign = std::make_shared<awst::AssignmentStatement>();
					assign->sourceLocation = loc;
					assign->target = std::move(target);
					assign->value = std::move(retVar);
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

		auto target = std::make_shared<awst::VarExpression>();
		target->sourceLocation = makeLoc(var.debugData);
		target->wtype = awst::WType::biguintType();
		target->name = name;

		std::shared_ptr<awst::Expression> value;
		if (_decl.value)
		{
			value = buildExpression(*_decl.value);
			if (!value)
			{
				// Expression failed to translate (error already logged), use zero fallback
				auto zero = std::make_shared<awst::IntegerConstant>();
				zero->sourceLocation = loc;
				zero->wtype = awst::WType::biguintType();
				zero->value = "0";
				value = std::move(zero);
			}
		}
		else
		{
			// Default: zero
			auto zero = std::make_shared<awst::IntegerConstant>();
			zero->sourceLocation = loc;
			zero->wtype = awst::WType::biguintType();
			zero->value = "0";
			value = std::move(zero);
		}

		// Coerce value to match target (biguint) — Yul values are always 256-bit
		value = ensureBiguint(std::move(value), loc);

		auto assign = std::make_shared<awst::AssignmentStatement>();
		assign->sourceLocation = loc;
		assign->target = std::move(target);
		assign->value = std::move(value);
		_out.push_back(std::move(assign));
	}
}

void AssemblyBuilder::buildAssignment(
	solidity::yul::Assignment const& _assign,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	auto loc = makeLoc(_assign.debugData);

	if (_assign.variableNames.size() != 1)
	{
		Logger::instance().error(
			"multi-variable assignment not yet supported in assembly translation", loc
		);
		return;
	}

	std::string name = _assign.variableNames[0].name.str();

	// Skip ERC-7201 storage slot assignments: $.slot := CONSTANT
	// These set EVM storage base positions, which have no AVM equivalent
	if (name.find(".slot") != std::string::npos)
	{
		return;
	}

	// Check for staticcall pattern: success := staticcall(...)
	if (_assign.value)
	{
		if (auto const* call = std::get_if<solidity::yul::FunctionCall>(_assign.value.get()))
		{
			std::string callName = call->functionName.name.str();
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
	if (!value)
	{
		// Expression failed to translate (error already logged), use zero fallback
		auto zero = std::make_shared<awst::IntegerConstant>();
		zero->sourceLocation = loc;
		zero->wtype = target->wtype;
		zero->value = "0";
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
			// Target is bytes — coerce biguint to bytes via ReinterpretCast
			if (value->wtype == awst::WType::biguintType())
			{
				auto cast = std::make_shared<awst::ReinterpretCast>();
				cast->sourceLocation = loc;
				cast->wtype = target->wtype;
				cast->expr = std::move(value);
				value = std::move(cast);
			}
			else
			{
				// uint64/bool → biguint → bytes
				auto biguintVal = ensureBiguint(std::move(value), loc);
				auto cast = std::make_shared<awst::ReinterpretCast>();
				cast->sourceLocation = loc;
				cast->wtype = target->wtype;
				cast->expr = std::move(biguintVal);
				value = std::move(cast);
			}
		}
		else if (target->wtype == awst::WType::uint64Type())
		{
			// Target is uint64 but value is biguint (e.g. from mload).
			// Truncate to uint64 using safeBtoi to keep the variable type consistent
			// across all control flow paths (avoids phi node type mismatches).
			if (value->wtype == awst::WType::biguintType())
			{
				value = safeBtoi(std::move(value), loc);
			}
		}
		else if (target->wtype == awst::WType::accountType())
		{
			// Target is account — coerce biguint/bytes to account
			if (value->wtype == awst::WType::biguintType())
			{
				// biguint → bytes → account
				auto toBytes = std::make_shared<awst::ReinterpretCast>();
				toBytes->sourceLocation = loc;
				toBytes->wtype = awst::WType::bytesType();
				toBytes->expr = std::move(value);

				auto toAccount = std::make_shared<awst::ReinterpretCast>();
				toAccount->sourceLocation = loc;
				toAccount->wtype = awst::WType::accountType();
				toAccount->expr = std::move(toBytes);
				value = std::move(toAccount);
			}
			else if (value->wtype != awst::WType::accountType())
			{
				auto cast = std::make_shared<awst::ReinterpretCast>();
				cast->sourceLocation = loc;
				cast->wtype = awst::WType::accountType();
				cast->expr = std::move(value);
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
				value->wtype = target->wtype;
			}
		}
	}

	auto assign = std::make_shared<awst::AssignmentStatement>();
	assign->sourceLocation = loc;
	assign->target = std::move(target);
	assign->value = std::move(value);
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
		std::string funcName = call->functionName.name.str();

		// Translate arguments (stored in source order)
		std::vector<std::shared_ptr<awst::Expression>> args;
		for (auto const& arg: call->arguments)
			args.push_back(buildExpression(arg));

		if (funcName == "mstore")
		{
			handleMstore(args, loc, _out);
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
			auto exprStmt = std::make_shared<awst::ExpressionStatement>();
			exprStmt->sourceLocation = loc;
			exprStmt->expr = std::move(expr);
			_out.push_back(std::move(exprStmt));
		}
	}
	else
	{
		// Non-call expression statement
		auto expr = buildExpression(_stmt.expression);
		if (expr)
		{
			auto exprStmt = std::make_shared<awst::ExpressionStatement>();
			exprStmt->sourceLocation = loc;
			exprStmt->expr = std::move(expr);
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

		auto target = std::make_shared<awst::VarExpression>();
		target->sourceLocation = _loc;
		target->name = paramName;
		target->wtype = paramType;

		auto assign = std::make_shared<awst::AssignmentStatement>();
		assign->sourceLocation = _loc;
		assign->target = std::move(target);
		assign->value = _args[i];
		_out.push_back(std::move(assign));
	}

	// Initialize return variables to zero
	for (auto const& retVar: funcDef.returnVariables)
	{
		std::string retName = retVar.name.str();
		m_locals[retName] = awst::WType::biguintType();

		auto target = std::make_shared<awst::VarExpression>();
		target->sourceLocation = _loc;
		target->name = retName;
		target->wtype = awst::WType::biguintType();

		auto zero = std::make_shared<awst::IntegerConstant>();
		zero->sourceLocation = _loc;
		zero->wtype = awst::WType::biguintType();
		zero->value = "0";

		auto assign = std::make_shared<awst::AssignmentStatement>();
		assign->sourceLocation = _loc;
		assign->target = std::move(target);
		assign->value = std::move(zero);
		_out.push_back(std::move(assign));
	}

	// Translate the function body inline
	for (auto const& stmt: funcDef.body.statements)
		buildStatement(stmt, _out);

	return nullptr;
}

} // namespace puyasol::builder
