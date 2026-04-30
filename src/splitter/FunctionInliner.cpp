#include "splitter/FunctionInliner.h"
#include "Logger.h"

#include <algorithm>
#include <sstream>

namespace puyasol::splitter
{

// ─── Public API ─────────────────────────────────────────────────────────────

FunctionInliner::InlineResult FunctionInliner::inlineAll(
	std::set<std::string> const& _targetNames,
	std::shared_ptr<awst::Contract> _contract,
	std::vector<std::shared_ptr<awst::RootNode>>& _roots
)
{
	auto& logger = Logger::instance();
	InlineResult result;

	buildCallableMaps(_contract, _roots);

	for (auto& method: _contract->methods)
	{
		if (!_targetNames.count(method.memberName))
			continue;
		if (!method.body)
			continue;

		logger.info("FunctionInliner: inlining all calls in '" + method.memberName +
			"' (" + std::to_string(method.body->body.size()) + " statements)");

		std::set<std::string> inlineStack;
		inlineStack.insert(method.memberName);

		// Repeat until no more inlining occurs (handles nested calls)
		for (int pass = 0; pass < 100; ++pass)
		{
			bool didInline = inlineBlock(*method.body, inlineStack, 0);
			if (!didInline)
				break;
			result.didInline = true;
			if (pass % 10 == 0 || pass < 5)
				logger.info("  Pass " + std::to_string(pass) + ": " +
					std::to_string(method.body->body.size()) + " statements");
		}

		result.totalStatements = method.body->body.size();
		logger.info("  Final: " + std::to_string(result.totalStatements) +
			" statements, " + std::to_string(m_inlinedFunctions.size()) +
			" unique functions inlined");

		// Create a Subroutine in roots so FunctionSplitter can split it.
		// The contract method will be rewritten by ContractSplitter later.
		auto sub = std::make_shared<awst::Subroutine>();
		sub->sourceLocation = method.sourceLocation;
		sub->id = _contract->id + "." + method.memberName;
		sub->name = method.memberName;
		sub->args = method.args;
		sub->returnType = method.returnType;
		sub->body = method.body;
		sub->pure = method.pure;
		_roots.push_back(sub);

		logger.info("  Created Subroutine '" + sub->name + "' with " +
			std::to_string(sub->body->body.size()) + " statements for splitting");
	}

	result.inlinedFunctions = m_inlinedFunctions;
	return result;
}

// ─── Callable map construction ──────────────────────────────────────────────

void FunctionInliner::buildCallableMaps(
	std::shared_ptr<awst::Contract> _contract,
	std::vector<std::shared_ptr<awst::RootNode>> const& _roots
)
{
	m_callableById.clear();
	m_callableByMember.clear();

	// Register all Subroutines (library/free functions) by their ID
	for (auto const& root: _roots)
	{
		if (auto const* sub = dynamic_cast<awst::Subroutine const*>(root.get()))
		{
			if (sub->body)
			{
				Callable c;
				c.name = sub->name;
				c.args = sub->args;
				c.returnType = sub->returnType;
				c.body = sub->body.get();
				m_callableById[sub->id] = c;
			}
		}
	}

	// Register all ContractMethods by their memberName (for InstanceMethodTarget)
	for (auto const& method: _contract->methods)
	{
		if (method.body)
		{
			Callable c;
			c.name = method.memberName;
			c.args = method.args;
			c.returnType = method.returnType;
			c.body = method.body.get();
			m_callableByMember[method.memberName] = c;
		}
	}
}

FunctionInliner::Callable const* FunctionInliner::resolveCall(
	awst::SubroutineCallExpression const& _call
)
{
	if (auto const* sid = std::get_if<awst::SubroutineID>(&_call.target))
	{
		auto it = m_callableById.find(sid->target);
		if (it != m_callableById.end())
			return &it->second;
	}
	else if (auto const* imt = std::get_if<awst::InstanceMethodTarget>(&_call.target))
	{
		auto it = m_callableByMember.find(imt->memberName);
		if (it != m_callableByMember.end())
			return &it->second;
	}
	return nullptr;
}

// ─── Block-level inlining ───────────────────────────────────────────────────

bool FunctionInliner::inlineBlock(
	awst::Block& _block,
	std::set<std::string>& _inlineStack,
	int _depth
)
{
	bool anyInlined = false;
	std::vector<std::shared_ptr<awst::Statement>> newBody;

	for (auto& stmt: _block.body)
	{
		if (!stmt)
		{
			newBody.push_back(stmt);
			continue;
		}

		// Try to extract a SubroutineCallExpression from the statement.
		awst::SubroutineCallExpression const* callExpr = nullptr;
		std::shared_ptr<awst::Expression> assignTarget;
		bool isReturn = false;
		Callable const* callee = nullptr;

		auto tryExtract = [&](awst::Expression const* _expr) -> bool
		{
			if (!_expr || _expr->nodeType() != "SubroutineCallExpression")
				return false;
			auto const& call = static_cast<awst::SubroutineCallExpression const&>(*_expr);
			auto const* c = resolveCall(call);
			if (!c || !c->body)
				return false;
			if (_inlineStack.count(c->name))
				return false; // recursion guard
			callExpr = &call;
			callee = c;
			return true;
		};

		std::string stmtType = stmt->nodeType();

		if (stmtType == "ExpressionStatement")
		{
			auto& es = static_cast<awst::ExpressionStatement&>(*stmt);
			if (!tryExtract(es.expr.get()))
			{
				if (es.expr && es.expr->nodeType() == "AssignmentExpression")
				{
					auto& ae = static_cast<awst::AssignmentExpression&>(*es.expr);
					if (tryExtract(ae.value.get()))
						assignTarget = ae.target;
				}
			}
		}
		else if (stmtType == "AssignmentStatement")
		{
			auto& as = static_cast<awst::AssignmentStatement&>(*stmt);
			if (tryExtract(as.value.get()))
				assignTarget = as.target;
		}
		else if (stmtType == "ReturnStatement")
		{
			auto& rs = static_cast<awst::ReturnStatement&>(*stmt);
			if (tryExtract(rs.value.get()))
				isReturn = true;
		}

		if (!callExpr || !callee)
		{
			newBody.push_back(stmt);
			continue;
		}

		// Inline the call
		auto inlinedStmts = inlineCall(*callExpr, *callee, assignTarget, _inlineStack, _depth);

		if (isReturn && !inlinedStmts.empty())
		{
			// For return context with no assignTarget, the callee's ReturnStatement
			// is preserved as-is. Just verify it ends with a return.
			auto& lastStmt = inlinedStmts.back();
			if (lastStmt && lastStmt->nodeType() == "ExpressionStatement")
			{
				auto& es = static_cast<awst::ExpressionStatement&>(*lastStmt);
				auto ret = awst::makeReturnStatement(es.expr, es.sourceLocation);
				inlinedStmts.back() = ret;
			}
		}

		for (auto& s: inlinedStmts)
			newBody.push_back(std::move(s));

		anyInlined = true;
	}

	if (anyInlined)
		_block.body = std::move(newBody);

	// Recurse into nested control flow structures
	for (auto& stmt: _block.body)
	{
		if (!stmt)
			continue;
		std::string stmtT = stmt->nodeType();
		if (stmtT == "IfElse")
		{
			auto& ie = static_cast<awst::IfElse&>(*stmt);
			if (ie.ifBranch && inlineBlock(*ie.ifBranch, _inlineStack, _depth + 1))
				anyInlined = true;
			if (ie.elseBranch && inlineBlock(*ie.elseBranch, _inlineStack, _depth + 1))
				anyInlined = true;
		}
		else if (stmtT == "WhileLoop")
		{
			auto& wl = static_cast<awst::WhileLoop&>(*stmt);
			if (wl.loopBody && inlineBlock(*wl.loopBody, _inlineStack, _depth + 1))
				anyInlined = true;
		}
		else if (stmtT == "Block")
		{
			auto& block = static_cast<awst::Block&>(*stmt);
			if (inlineBlock(block, _inlineStack, _depth + 1))
				anyInlined = true;
		}
		else if (stmtT == "Switch")
		{
			auto& sw = static_cast<awst::Switch&>(*stmt);
			for (auto& [_, caseBlock]: sw.cases)
				if (caseBlock && inlineBlock(*caseBlock, _inlineStack, _depth + 1))
					anyInlined = true;
			if (sw.defaultCase && inlineBlock(*sw.defaultCase, _inlineStack, _depth + 1))
				anyInlined = true;
		}
		else if (stmtT == "ForInLoop")
		{
			auto& fil = static_cast<awst::ForInLoop&>(*stmt);
			if (fil.loopBody && inlineBlock(*fil.loopBody, _inlineStack, _depth + 1))
				anyInlined = true;
		}
	}

	return anyInlined;
}

// ─── Single call inlining ───────────────────────────────────────────────────

std::vector<std::shared_ptr<awst::Statement>> FunctionInliner::inlineCall(
	awst::SubroutineCallExpression const& _call,
	Callable const& _callee,
	std::shared_ptr<awst::Expression> _assignTarget,
	std::set<std::string>& _inlineStack,
	int _depth
)
{
	m_inlinedFunctions.insert(_callee.name);

	std::string prefix = "__il" + std::to_string(m_inlineCounter++) + "_";
	auto loc = _call.sourceLocation;

	std::vector<std::shared_ptr<awst::Statement>> result;

	// 1. Assign arguments to renamed parameters
	for (size_t i = 0; i < _callee.args.size() && i < _call.args.size(); ++i)
	{
		std::string renamedParam = prefix + _callee.args[i].name;

		auto target = awst::makeVarExpression(renamedParam, _callee.args[i].wtype, loc);

		auto assign = awst::makeAssignmentStatement(target, _call.args[i].value, loc);
		result.push_back(assign);
	}

	// 2. Collect local variable names (for renaming)
	std::set<std::string> localNames;
	for (auto const& arg: _callee.args)
		localNames.insert(arg.name);
	collectLocalNames(*_callee.body, localNames);

	// 3. Deep-copy body statements and rename variables (avoid corrupting callee body)
	std::vector<std::shared_ptr<awst::Statement>> bodyStmts;
	for (auto const& stmt: _callee.body->body)
		bodyStmts.push_back(deepCopyStmt(stmt));

	renameVarsInStmts(bodyStmts, prefix, localNames);

	// 4. Process the body: convert ReturnStatements
	for (size_t i = 0; i < bodyStmts.size(); ++i)
	{
		auto& s = bodyStmts[i];
		if (!s)
			continue;

		bool isLast = (i == bodyStmts.size() - 1);

		if (s->nodeType() == "ReturnStatement")
		{
			auto& rs = static_cast<awst::ReturnStatement&>(*s);

			if (isLast)
			{
				if (rs.value)
				{
					if (_assignTarget)
					{
						auto assign = awst::makeAssignmentStatement(_assignTarget, rs.value, rs.sourceLocation);
						result.push_back(assign);
					}
					else
					{
						// Keep ReturnStatement — caller handles context
						result.push_back(s);
					}
				}
			}
			else
			{
				// Inner return: assign + skip remaining (unreachable)
				if (rs.value && _assignTarget)
				{
					auto assign = awst::makeAssignmentStatement(_assignTarget, rs.value, rs.sourceLocation);
					result.push_back(assign);
				}
				break;
			}
		}
		else
		{
			result.push_back(s);
		}
	}

	return result;
}

// ─── Variable renaming ──────────────────────────────────────────────────────


// ─── Local name collection ──────────────────────────────────────────────────

void FunctionInliner::collectLocalNames(
	awst::Block const& _block,
	std::set<std::string>& _names
)
{
	for (auto const& stmt: _block.body)
		if (stmt)
			collectStmtDefs(*stmt, _names);
}

void FunctionInliner::collectStmtDefs(
	awst::Statement const& _stmt,
	std::set<std::string>& _names
)
{
	std::string type = _stmt.nodeType();

	if (type == "AssignmentStatement")
	{
		auto const& as = static_cast<awst::AssignmentStatement const&>(_stmt);
		if (as.target && as.target->nodeType() == "VarExpression")
			_names.insert(static_cast<awst::VarExpression const&>(*as.target).name);
	}
	else if (type == "ExpressionStatement")
	{
		auto const& es = static_cast<awst::ExpressionStatement const&>(_stmt);
		if (es.expr && es.expr->nodeType() == "AssignmentExpression")
		{
			auto const& ae = static_cast<awst::AssignmentExpression const&>(*es.expr);
			if (ae.target && ae.target->nodeType() == "VarExpression")
				_names.insert(static_cast<awst::VarExpression const&>(*ae.target).name);
		}
	}
	else if (type == "IfElse")
	{
		auto const& ie = static_cast<awst::IfElse const&>(_stmt);
		if (ie.ifBranch) collectLocalNames(*ie.ifBranch, _names);
		if (ie.elseBranch) collectLocalNames(*ie.elseBranch, _names);
	}
	else if (type == "Block")
	{
		collectLocalNames(static_cast<awst::Block const&>(_stmt), _names);
	}
	else if (type == "WhileLoop")
	{
		auto const& wl = static_cast<awst::WhileLoop const&>(_stmt);
		if (wl.loopBody) collectLocalNames(*wl.loopBody, _names);
	}
}

} // namespace puyasol::splitter
