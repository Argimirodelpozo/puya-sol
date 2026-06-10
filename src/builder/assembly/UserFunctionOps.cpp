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
	// Reset the marker so the inline path below leaves it empty (callers then
	// read the function's own return-var names, which the inlined body assigns).
	m_yulSubReturnTemps.clear();

	// Recursive Yul functions are lowered to AWST Subroutines (emitted in
	// AssemblyBuilder::buildBlock). Dispatch via a subroutine call instead of
	// inlining so calls don't recurse at compile time.
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

		size_t nRet = funcDef.returnVariables.size();
		awst::WType const* callRetType =
			nRet == 0 ? awst::WType::voidType()
			: nRet == 1 ? awst::WType::biguintType()
			: static_cast<awst::WType const*>(new awst::WTuple(
				std::vector<awst::WType const*>(nRet, awst::WType::biguintType())));
		auto call = awst::makeSubroutineCall(awst::SubroutineID{subIt->second}, callRetType, _loc);
		for (auto const& a: _args)
			awst::pushCallArg(call->args, ensureBiguint(a, _loc));

		static int s_yulCallId = 0;
		int callId = ++s_yulCallId;

		if (nRet == 0)
		{
			_out.push_back(awst::makeExpressionStatement(call, _loc));
			return awst::makeVoidConstant(_loc);
		}

		// Destructure the return value(s) into FRESH temps __yulret_<id>_<i> —
		// decoupled from the function's own return-var names so a recursive call
		// (whose return-var names equal the current frame's) doesn't clobber the
		// caller's live values before they're read. Callers map these temps to
		// their declared variables via m_yulSubReturnTemps. Multi-return wraps
		// the call in a SingleEvaluation so it runs once.
		std::shared_ptr<awst::Expression> resultSrc;
		if (nRet == 1)
			resultSrc = call;
		else
			resultSrc = awst::makeSingleEvaluation(call, callRetType, callId, _loc);

		for (size_t i = 0; i < nRet; ++i)
		{
			std::string t = "__yulret_" + std::to_string(callId) + "_" + std::to_string(i);
			m_locals[t] = awst::WType::biguintType();
			std::shared_ptr<awst::Expression> value = (nRet == 1)
				? resultSrc
				: awst::makeTupleItem(resultSrc, static_cast<int>(i), awst::WType::biguintType(), _loc);
			auto target = awst::makeVarExpression(t, awst::WType::biguintType(), _loc);
			_out.push_back(awst::makeAssignmentStatement(std::move(target), std::move(value), _loc));
			m_yulSubReturnTemps.push_back(t);
		}

		// Single-return may be used in expression context — return the temp.
		if (nRet == 1)
			return awst::makeVarExpression(m_yulSubReturnTemps[0], awst::WType::biguintType(), _loc);
		return nullptr;
	}

	// Recursion guard: Yul function inlining expands each call at the AST
	// level. Recursive Yul functions (e.g. `function fac(n) -> nf { ... fac(sub(n,1)) ... }`)
	// otherwise recurse forever here and blow the C++ stack. Emit an error
	// and bail out of the call path so the rest of the contract at least
	// compiles far enough to report a meaningful diagnostic.
	if (m_inlineDepth > 64)
	{
		// Backstop against the C++ translator stack overflowing on runaway inline
		// expansion. Genuine recursion (direct or mutual) is detected and lowered
		// to a subroutine above, so reaching this means either an unusually deep
		// non-recursive call chain or a gap in that detection — NOT that recursion
		// is unsupported.
		Logger::instance().error(
			"assembly function '" + _name + "' exceeded the inline-expansion depth "
			"limit (64 frames); recursion is normally lowered to a subroutine, so "
			"this is either a very deep non-recursive call chain or a recursion-"
			"detection gap",
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
