/// @file UserFunctionOps.cpp
/// User-defined Yul subroutine calls and Solidity-frame inline fallback.

#include "builder/assembly/AssemblyBuilder.h"
#include "awst/NameGen.h"
#include "Logger.h"

#include <functional>
#include <string>
#include <libyul/AST.h>
#include <libyul/Dialect.h>
#include <libyul/optimiser/ASTWalker.h>

namespace puyasol::builder
{

void AssemblyBuilder::buildFunctionDefinition(
	solidity::yul::FunctionDefinition const& _def,
	std::vector<std::shared_ptr<awst::Statement>>& /*_out*/
)
{
	// Collected in the first pass; definitions have no runtime effect here.
	auto loc = makeLoc(_def.debugData);
	Logger::instance().debug(
		"assembly function '" + _def.name.str() + "' collected", loc
	);
}

// ─── Assembly function inlining ─────────────────────────────────────────────

std::shared_ptr<awst::Expression> AssemblyBuilder::handleUserFunctionCall(
	solidity::yul::FunctionCall const& _call,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	auto const _name = getFunctionName(_call.functionName);
	auto _args = buildCallOperands(_call, _out);
	for (auto const& arg: _args)
		if (!arg) return nullptr;
	m_frame.yulSubReturnTemps.clear();

	// Preserve the function boundary through initial SSA construction. Puya can
	// still selectively inline the resulting IR after each function is built.
	auto subIt = m_context->yulFuncSubroutineIds.find(_name);
	if (subIt != m_context->yulFuncSubroutineIds.end())
	{
		auto defIt = m_context->asmFunctions.find(_name);
		if (defIt == m_context->asmFunctions.end())
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
			: m_typeMapper.createType<awst::WTuple>(
				std::vector<awst::WType const*>(nRet, awst::WType::biguintType()));
		auto call = awst::makeSubroutineCall(awst::SubroutineID{subIt->second}, callRetType, _loc);
		for (auto const& a: _args)
			awst::pushCallArg(call->args, ensureBiguint(a, _loc));
		if (m_context->yulCalldataFunctions.count(_name))
			awst::pushCallArg(call->args,
				awst::makeVarExpression(CD_BLOB_VAR, awst::WType::bytesType(), _loc));
		if (m_context->yulMemoryWritingFunctions.count(_name))
			invalidateMemConstants();

		int callId = (awst::NameGen::next("UserFunctionOps.s_yulCallId") + 1);

		if (nRet == 0)
		{
			_out.push_back(awst::makeExpressionStatement(call, _loc));
			return awst::makeVoidConstant(_loc);
		}

		// Fresh temps __yulret_<id>_<i>: decoupled from the function's return-var names
		// so a recursive call can't clobber the caller's live values. Multi-return wraps
		// call in SingleEvaluation. Callers map temps via m_frame.yulSubReturnTemps.
		std::shared_ptr<awst::Expression> resultSrc;
		if (nRet == 1)
			resultSrc = call;
		else
			resultSrc = awst::makeSingleEvaluation(call, callRetType, callId, _loc);

		for (size_t i = 0; i < nRet; ++i)
		{
			std::string t = "__yulret_" + std::to_string(callId) + "_" + std::to_string(i);
			m_frame.locals[t] = awst::WType::biguintType();
			std::shared_ptr<awst::Expression> value = (nRet == 1)
				? resultSrc
				: awst::makeTupleItem(resultSrc, static_cast<int>(i), awst::WType::biguintType(), _loc);
			auto target = awst::makeVarExpression(t, awst::WType::biguintType(), _loc);
			_out.push_back(awst::makeAssignmentStatement(std::move(target), std::move(value), _loc));
			m_frame.yulSubReturnTemps.push_back(t);
		}

		// Single-return may be used in expression context — return the temp.
		if (nRet == 1)
			return awst::makeVarExpression(m_frame.yulSubReturnTemps[0], awst::WType::biguintType(), _loc);
		return nullptr;
	}

	// Depth backstop: genuine recursion is detected above; reaching >64 means a
	// very deep non-recursive chain or a detection gap — NOT unsupported recursion.
	if (m_frame.inlineDepth > 64)
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

	auto it = m_context->asmFunctions.find(_name);
	if (it == m_context->asmFunctions.end())
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
	// clobber the same runtime vars. resolveVarRef applies m_frame.yulInlineRenames to the body;
	// saved/restored per frame so an outer/sibling frame's renames are unaffected.
	int uid = (awst::NameGen::next("UserFunctionOps.s_yulInlineUid") + 1);
	auto uniqueName = [&](std::string const& n) { return "__yul_" + std::to_string(uid) + "_" + n; };
	std::vector<std::tuple<std::string, bool, std::string>> savedRenames;
	auto pushRename = [&](std::string const& bare, std::string const& unique) {
		auto it = m_frame.yulInlineRenames.find(bare);
		savedRenames.emplace_back(bare, it != m_frame.yulInlineRenames.end(),
			it != m_frame.yulInlineRenames.end() ? it->second : std::string());
		m_frame.yulInlineRenames[bare] = unique;
	};

	// Bind parameters; use arg's actual type (handles arrays passed to assembly fns).
	for (size_t i = 0; i < funcDef.parameters.size(); ++i)
	{
		std::string paramName = funcDef.parameters[i].name.str();
		std::string uName = uniqueName(paramName);
		pushRename(paramName, uName);
		awst::WType const* paramType = _args[i]->wtype;
		m_frame.locals[uName] = paramType;
		// Only single-assignment params: the fn body's `paramName := …` sites were
		// collected under the ORIGINAL name; a reassigned param's bound constant
		// would go stale mid-body (same rule as `let` locals).
		auto constVal = resolveConstantOffset(_args[i]);
		if (constVal && !m_context->reassignedLocals.count(paramName))
			m_frame.localConstants[uName] = *constVal;
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
		m_frame.locals[uName] = awst::WType::biguintType();
		_out.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(uName, awst::WType::biguintType(), _loc),
			awst::makeBiguintConstant("0", _loc), _loc));
	}

	// One solc walk collects inline locals and leaves, without entering
	// nested functions (their frame belongs to their own invocation).
	struct InlineFacts: solidity::yul::ASTWalker
	{
		using ASTWalker::operator();
		std::function<void(std::string const&)> rename;
		bool hasLeave = false;
		void operator()(solidity::yul::FunctionDefinition const&) override {}
		void operator()(solidity::yul::Leave const&) override { hasLeave = true; }
		void operator()(solidity::yul::VariableDeclaration const& declaration) override
		{
			for (auto const& variable: declaration.variables) rename(variable.name.str());
		}
	} facts;
	facts.rename = [&](std::string const& name) { pushRename(name, uniqueName(name)); };
	facts(funcDef.body);
	bool const hasLeave = facts.hasLeave;
	std::vector<std::shared_ptr<awst::Statement>> bodyStmts;
	auto savedLeaveFlag = m_frame.yulLeaveFlag;
	if (hasLeave)
	{
		m_frame.yulLeaveFlag = "__yul_leave_" + std::to_string(uid);
		_out.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(
				m_frame.yulLeaveFlag, awst::WType::boolType(), _loc),
			awst::makeFalse(_loc), _loc));
	}
	++m_frame.inlineDepth;
	for (auto const& stmt: funcDef.body.statements)
	{
		buildStatement(stmt, bodyStmts);
		// Top-level leave makes remainder unreachable (puya rejects that); stop.
		// Nested leaves (inside if/switch) don't trigger this.
		if (std::holds_alternative<solidity::yul::Leave>(stmt))
			break;
	}
	--m_frame.inlineDepth;
	m_frame.yulLeaveFlag = savedLeaveFlag;

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
			m_frame.yulInlineRenames[bare] = old;
		else
			m_frame.yulInlineRenames.erase(bare);
	}

	// Publish this call's return temps (unique names) so the caller reads the right vars, not the
	// shared bare return-var name. Single-return also returns it as the expression value.
	m_frame.yulSubReturnTemps = uniqueRetNames;
	if (uniqueRetNames.size() == 1)
		return awst::makeVarExpression(uniqueRetNames[0], awst::WType::biguintType(), _loc);
	return nullptr;
}

} // namespace puyasol::builder
