/// @file UserFunctionOps.cpp
/// User-defined Yul function inlining + recursive-subroutine dispatch.

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
	// Collected in first pass and inlined at call sites; nothing to emit here.
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
	// Clear so callers read the function's own return-var names after inlining.
	m_yulSubReturnTemps.clear();

	// Recursive Yul functions → AWST Subroutines (emitted in buildBlock); dispatch
	// via subroutine call to avoid C++ compile-time recursion.
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

		// Fresh temps __yulret_<id>_<i>: decoupled from the function's return-var names
		// so a recursive call can't clobber the caller's live values. Multi-return wraps
		// call in SingleEvaluation. Callers map temps via m_yulSubReturnTemps.
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

	// Depth backstop: genuine recursion is detected above; reaching >64 means a
	// very deep non-recursive chain or a detection gap — NOT unsupported recursion.
	if (m_inlineDepth > 64)
	{
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

	if (_args.size() != funcDef.parameters.size())
	{
		Logger::instance().error(
			"assembly function '" + _name + "' called with wrong number of arguments", _loc
		);
		return nullptr;
	}

	// Per-inline-call unique names: a fn's bare params/returns (x, y) are renamed to
	// __yul_<uid>_<name> so sibling (sq(a)+cube(b)) and nested (cube calls sq) calls don't
	// clobber the same runtime vars. resolveVarRef applies m_yulInlineRenames to the body;
	// saved/restored per frame so an outer/sibling frame's renames are unaffected.
	static int s_yulInlineUid = 0;
	int uid = ++s_yulInlineUid;
	auto uniqueName = [&](std::string const& n) { return "__yul_" + std::to_string(uid) + "_" + n; };
	std::vector<std::tuple<std::string, bool, std::string>> savedRenames;
	auto pushRename = [&](std::string const& bare, std::string const& unique) {
		auto it = m_yulInlineRenames.find(bare);
		savedRenames.emplace_back(bare, it != m_yulInlineRenames.end(),
			it != m_yulInlineRenames.end() ? it->second : std::string());
		m_yulInlineRenames[bare] = unique;
	};

	// Bind parameters; use arg's actual type (handles arrays passed to assembly fns).
	for (size_t i = 0; i < funcDef.parameters.size(); ++i)
	{
		std::string paramName = funcDef.parameters[i].name.str();
		std::string uName = uniqueName(paramName);
		pushRename(paramName, uName);
		awst::WType const* paramType = _args[i]->wtype;
		m_locals[uName] = paramType;
		auto constVal = resolveConstantOffset(_args[i]);
		if (constVal)
			m_localConstants[uName] = *constVal;
		_out.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(uName, paramType, _loc), _args[i], _loc));
	}

	// Initialize return variables to zero (under unique names).
	std::vector<std::string> uniqueRetNames;
	for (auto const& retVar: funcDef.returnVariables)
	{
		std::string retName = retVar.name.str();
		std::string uName = uniqueName(retName);
		pushRename(retName, uName);
		uniqueRetNames.push_back(uName);
		m_locals[uName] = awst::WType::biguintType();
		_out.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(uName, awst::WType::biguintType(), _loc),
			awst::makeBiguintConstant("0", _loc), _loc));
	}

	// Wrap body in `while true { … break; }` when it contains `leave`, so that
	// leave→LoopExit breaks only the inlined body, not any enclosing loop.
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
		// Top-level leave makes remainder unreachable (puya rejects that); stop.
		// Nested leaves (inside if/switch) don't trigger this.
		if (std::holds_alternative<solidity::yul::Leave>(stmt))
			break;
	}
	--m_inlineDepth;

	if (hasLeave)
	{
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

	// Restore the renames this frame installed (siblings/outer must not see them).
	for (auto it = savedRenames.rbegin(); it != savedRenames.rend(); ++it)
	{
		auto const& [bare, had, old] = *it;
		if (had)
			m_yulInlineRenames[bare] = old;
		else
			m_yulInlineRenames.erase(bare);
	}

	// Publish this call's return temps (unique names) so the caller reads the right vars, not the
	// shared bare return-var name. Single-return also returns it as the expression value.
	m_yulSubReturnTemps = uniqueRetNames;
	if (uniqueRetNames.size() == 1)
		return awst::makeVarExpression(uniqueRetNames[0], awst::WType::biguintType(), _loc);
	return nullptr;
}

} // namespace puyasol::builder
