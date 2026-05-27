/// @file UserFunctionOps.cpp
/// Inlining + recursive-subroutine dispatch for user-defined Yul
/// functions (`function f(a, b) -> c { ... }`). Extracted from
/// StatementOps.cpp; the dispatcher there delegates each
/// `FunctionDefinition` AST node and each `FunctionCall` whose
/// callee is a user function to one of these.

#include "builder/assembly/AssemblyBuilder.h"
#include "Logger.h"

#include <functional>
#include <string>

namespace puyasol::builder
{

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

		auto call = awst::makeSubroutineCall(awst::SubroutineID{subIt->second}, funcDef.returnVariables.size() == 1
			? awst::WType::biguintType()
			: awst::WType::voidType(), _loc);
		for (auto const& a: _args)
			awst::pushCallArg(call->args, ensureBiguint(a, _loc));

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
		return awst::makeVoidConstant(_loc);
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

		auto zero = awst::makeBiguintConstant("0", _loc);

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
		auto block = awst::makeBlock(_loc);
		block->body = std::move(bodyStmts);
		block->body.push_back(awst::makeLoopExit(_loc));

		_out.push_back(awst::makeWhileLoop(
			awst::makeTrue(_loc), std::move(block), _loc));
	}
	else
	{
		for (auto& s: bodyStmts)
			_out.push_back(std::move(s));
	}

	return nullptr;
}

} // namespace puyasol::builder
