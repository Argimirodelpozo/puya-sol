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
				auto ret = std::make_shared<awst::ReturnStatement>();
				ret->sourceLocation = es.sourceLocation;
				ret->value = es.expr;
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

		auto assign = std::make_shared<awst::AssignmentStatement>();
		assign->sourceLocation = loc;
		assign->target = target;
		assign->value = _call.args[i].value;
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
						auto assign = std::make_shared<awst::AssignmentStatement>();
						assign->sourceLocation = rs.sourceLocation;
						assign->target = _assignTarget;
						assign->value = rs.value;
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
					auto assign = std::make_shared<awst::AssignmentStatement>();
					assign->sourceLocation = rs.sourceLocation;
					assign->target = _assignTarget;
					assign->value = rs.value;
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

void FunctionInliner::renameVarsInStmts(
	std::vector<std::shared_ptr<awst::Statement>>& _stmts,
	std::string const& _prefix,
	std::set<std::string> const& _localNames
)
{
	for (auto& stmt: _stmts)
		if (stmt)
			renameVarsInStmt(stmt, _prefix, _localNames);
}

void FunctionInliner::renameVarsInExpr(
	std::shared_ptr<awst::Expression>& _expr,
	std::string const& _prefix,
	std::set<std::string> const& _localNames
)
{
	if (!_expr)
		return;

	std::string type = _expr->nodeType();

	if (type == "VarExpression")
	{
		auto& var = static_cast<awst::VarExpression&>(*_expr);
		if (_localNames.count(var.name))
			var.name = _prefix + var.name;
	}
	else if (type == "SubroutineCallExpression")
	{
		auto& call = static_cast<awst::SubroutineCallExpression&>(*_expr);
		for (auto& arg: call.args)
			renameVarsInExpr(arg.value, _prefix, _localNames);
	}
	else if (type == "AssignmentExpression")
	{
		auto& a = static_cast<awst::AssignmentExpression&>(*_expr);
		renameVarsInExpr(a.target, _prefix, _localNames);
		renameVarsInExpr(a.value, _prefix, _localNames);
	}
	else if (type == "BigUIntBinaryOperation")
	{
		auto& op = static_cast<awst::BigUIntBinaryOperation&>(*_expr);
		renameVarsInExpr(op.left, _prefix, _localNames);
		renameVarsInExpr(op.right, _prefix, _localNames);
	}
	else if (type == "UInt64BinaryOperation")
	{
		auto& op = static_cast<awst::UInt64BinaryOperation&>(*_expr);
		renameVarsInExpr(op.left, _prefix, _localNames);
		renameVarsInExpr(op.right, _prefix, _localNames);
	}
	else if (type == "BytesBinaryOperation")
	{
		auto& op = static_cast<awst::BytesBinaryOperation&>(*_expr);
		renameVarsInExpr(op.left, _prefix, _localNames);
		renameVarsInExpr(op.right, _prefix, _localNames);
	}
	else if (type == "BytesUnaryOperation")
	{
		auto& op = static_cast<awst::BytesUnaryOperation&>(*_expr);
		renameVarsInExpr(op.expr, _prefix, _localNames);
	}
	else if (type == "NumericComparisonExpression")
	{
		auto& cmp = static_cast<awst::NumericComparisonExpression&>(*_expr);
		renameVarsInExpr(cmp.lhs, _prefix, _localNames);
		renameVarsInExpr(cmp.rhs, _prefix, _localNames);
	}
	else if (type == "BytesComparisonExpression")
	{
		auto& cmp = static_cast<awst::BytesComparisonExpression&>(*_expr);
		renameVarsInExpr(cmp.lhs, _prefix, _localNames);
		renameVarsInExpr(cmp.rhs, _prefix, _localNames);
	}
	else if (type == "BooleanBinaryOperation")
	{
		auto& op = static_cast<awst::BooleanBinaryOperation&>(*_expr);
		renameVarsInExpr(op.left, _prefix, _localNames);
		renameVarsInExpr(op.right, _prefix, _localNames);
	}
	else if (type == "Not")
	{
		auto& n = static_cast<awst::Not&>(*_expr);
		renameVarsInExpr(n.expr, _prefix, _localNames);
	}
	else if (type == "AssertExpression")
	{
		auto& a = static_cast<awst::AssertExpression&>(*_expr);
		renameVarsInExpr(a.condition, _prefix, _localNames);
	}
	else if (type == "ConditionalExpression")
	{
		auto& c = static_cast<awst::ConditionalExpression&>(*_expr);
		renameVarsInExpr(c.condition, _prefix, _localNames);
		renameVarsInExpr(c.trueExpr, _prefix, _localNames);
		renameVarsInExpr(c.falseExpr, _prefix, _localNames);
	}
	else if (type == "IntrinsicCall")
	{
		auto& ic = static_cast<awst::IntrinsicCall&>(*_expr);
		for (auto& arg: ic.stackArgs)
			renameVarsInExpr(arg, _prefix, _localNames);
	}
	else if (type == "PuyaLibCall")
	{
		auto& plc = static_cast<awst::PuyaLibCall&>(*_expr);
		for (auto& arg: plc.args)
			renameVarsInExpr(arg.value, _prefix, _localNames);
	}
	else if (type == "FieldExpression")
	{
		auto& f = static_cast<awst::FieldExpression&>(*_expr);
		renameVarsInExpr(f.base, _prefix, _localNames);
	}
	else if (type == "IndexExpression")
	{
		auto& idx = static_cast<awst::IndexExpression&>(*_expr);
		renameVarsInExpr(idx.base, _prefix, _localNames);
		renameVarsInExpr(idx.index, _prefix, _localNames);
	}
	else if (type == "TupleExpression")
	{
		auto& t = static_cast<awst::TupleExpression&>(*_expr);
		for (auto& item: t.items)
			renameVarsInExpr(item, _prefix, _localNames);
	}
	else if (type == "TupleItemExpression")
	{
		auto& ti = static_cast<awst::TupleItemExpression&>(*_expr);
		renameVarsInExpr(ti.base, _prefix, _localNames);
	}
	else if (type == "ARC4Encode")
	{
		auto& e = static_cast<awst::ARC4Encode&>(*_expr);
		renameVarsInExpr(e.value, _prefix, _localNames);
	}
	else if (type == "ARC4Decode")
	{
		auto& d = static_cast<awst::ARC4Decode&>(*_expr);
		renameVarsInExpr(d.value, _prefix, _localNames);
	}
	else if (type == "ReinterpretCast")
	{
		auto& rc = static_cast<awst::ReinterpretCast&>(*_expr);
		renameVarsInExpr(rc.expr, _prefix, _localNames);
	}
	else if (type == "Copy")
	{
		auto& c = static_cast<awst::Copy&>(*_expr);
		renameVarsInExpr(c.value, _prefix, _localNames);
	}
	else if (type == "SingleEvaluation")
	{
		auto& se = static_cast<awst::SingleEvaluation&>(*_expr);
		renameVarsInExpr(se.source, _prefix, _localNames);
	}
	else if (type == "CheckedMaybe")
	{
		auto& cm = static_cast<awst::CheckedMaybe&>(*_expr);
		renameVarsInExpr(cm.expr, _prefix, _localNames);
	}
	else if (type == "NewArray")
	{
		auto& na = static_cast<awst::NewArray&>(*_expr);
		for (auto& v: na.values)
			renameVarsInExpr(v, _prefix, _localNames);
	}
	else if (type == "ArrayLength")
	{
		auto& al = static_cast<awst::ArrayLength&>(*_expr);
		renameVarsInExpr(al.array, _prefix, _localNames);
	}
	else if (type == "ArrayPop")
	{
		auto& ap = static_cast<awst::ArrayPop&>(*_expr);
		renameVarsInExpr(ap.base, _prefix, _localNames);
	}
	else if (type == "ArrayConcat")
	{
		auto& ac = static_cast<awst::ArrayConcat&>(*_expr);
		renameVarsInExpr(ac.left, _prefix, _localNames);
		renameVarsInExpr(ac.right, _prefix, _localNames);
	}
	else if (type == "ArrayExtend")
	{
		auto& ae = static_cast<awst::ArrayExtend&>(*_expr);
		renameVarsInExpr(ae.base, _prefix, _localNames);
		renameVarsInExpr(ae.other, _prefix, _localNames);
	}
	else if (type == "StateGet")
	{
		auto& sg = static_cast<awst::StateGet&>(*_expr);
		renameVarsInExpr(sg.field, _prefix, _localNames);
		renameVarsInExpr(sg.defaultValue, _prefix, _localNames);
	}
	else if (type == "StateExists")
	{
		auto& se = static_cast<awst::StateExists&>(*_expr);
		renameVarsInExpr(se.field, _prefix, _localNames);
	}
	else if (type == "StateDelete")
	{
		auto& sd = static_cast<awst::StateDelete&>(*_expr);
		renameVarsInExpr(sd.field, _prefix, _localNames);
	}
	else if (type == "StateGetEx")
	{
		auto& sge = static_cast<awst::StateGetEx&>(*_expr);
		renameVarsInExpr(sge.field, _prefix, _localNames);
	}
	else if (type == "BoxValueExpression")
	{
		auto& bve = static_cast<awst::BoxValueExpression&>(*_expr);
		renameVarsInExpr(bve.key, _prefix, _localNames);
	}
	else if (type == "BoxPrefixedKeyExpression")
	{
		auto& bpk = static_cast<awst::BoxPrefixedKeyExpression&>(*_expr);
		renameVarsInExpr(bpk.prefix, _prefix, _localNames);
		renameVarsInExpr(bpk.key, _prefix, _localNames);
	}
	else if (type == "NewStruct")
	{
		auto& ns = static_cast<awst::NewStruct&>(*_expr);
		for (auto& [_, val]: ns.values)
			renameVarsInExpr(val, _prefix, _localNames);
	}
	else if (type == "NamedTupleExpression")
	{
		auto& nt = static_cast<awst::NamedTupleExpression&>(*_expr);
		for (auto& [_, val]: nt.values)
			renameVarsInExpr(val, _prefix, _localNames);
	}
	else if (type == "Emit")
	{
		auto& e = static_cast<awst::Emit&>(*_expr);
		renameVarsInExpr(e.value, _prefix, _localNames);
	}
	else if (type == "CreateInnerTransaction")
	{
		auto& cit = static_cast<awst::CreateInnerTransaction&>(*_expr);
		for (auto& [_, val]: cit.fields)
			renameVarsInExpr(val, _prefix, _localNames);
	}
	else if (type == "SubmitInnerTransaction")
	{
		auto& sit = static_cast<awst::SubmitInnerTransaction&>(*_expr);
		for (auto& itxn: sit.itxns)
			renameVarsInExpr(itxn, _prefix, _localNames);
	}
	else if (type == "InnerTransactionField")
	{
		auto& itf = static_cast<awst::InnerTransactionField&>(*_expr);
		renameVarsInExpr(itf.itxn, _prefix, _localNames);
	}
	else if (type == "CommaExpression")
	{
		auto& ce = static_cast<awst::CommaExpression&>(*_expr);
		for (auto& e: ce.expressions)
			renameVarsInExpr(e, _prefix, _localNames);
	}
}

void FunctionInliner::renameVarsInStmt(
	std::shared_ptr<awst::Statement>& _stmt,
	std::string const& _prefix,
	std::set<std::string> const& _localNames
)
{
	if (!_stmt)
		return;

	std::string type = _stmt->nodeType();

	if (type == "ExpressionStatement")
	{
		auto& es = static_cast<awst::ExpressionStatement&>(*_stmt);
		renameVarsInExpr(es.expr, _prefix, _localNames);
	}
	else if (type == "AssignmentStatement")
	{
		auto& as = static_cast<awst::AssignmentStatement&>(*_stmt);
		renameVarsInExpr(as.target, _prefix, _localNames);
		renameVarsInExpr(as.value, _prefix, _localNames);
	}
	else if (type == "ReturnStatement")
	{
		auto& rs = static_cast<awst::ReturnStatement&>(*_stmt);
		renameVarsInExpr(rs.value, _prefix, _localNames);
	}
	else if (type == "IfElse")
	{
		auto& ie = static_cast<awst::IfElse&>(*_stmt);
		renameVarsInExpr(ie.condition, _prefix, _localNames);
		if (ie.ifBranch)
			for (auto& s: ie.ifBranch->body)
				renameVarsInStmt(s, _prefix, _localNames);
		if (ie.elseBranch)
			for (auto& s: ie.elseBranch->body)
				renameVarsInStmt(s, _prefix, _localNames);
	}
	else if (type == "Block")
	{
		auto& block = static_cast<awst::Block&>(*_stmt);
		for (auto& s: block.body)
			renameVarsInStmt(s, _prefix, _localNames);
	}
	else if (type == "WhileLoop")
	{
		auto& wl = static_cast<awst::WhileLoop&>(*_stmt);
		renameVarsInExpr(wl.condition, _prefix, _localNames);
		if (wl.loopBody)
			for (auto& s: wl.loopBody->body)
				renameVarsInStmt(s, _prefix, _localNames);
	}
	else if (type == "Switch")
	{
		auto& sw = static_cast<awst::Switch&>(*_stmt);
		renameVarsInExpr(sw.value, _prefix, _localNames);
		for (auto& [caseExpr, caseBlock]: sw.cases)
		{
			auto ce = caseExpr;
			renameVarsInExpr(ce, _prefix, _localNames);
			if (caseBlock)
				for (auto& s: caseBlock->body)
					renameVarsInStmt(s, _prefix, _localNames);
		}
	}
	else if (type == "ForInLoop")
	{
		auto& fil = static_cast<awst::ForInLoop&>(*_stmt);
		renameVarsInExpr(fil.sequence, _prefix, _localNames);
		renameVarsInExpr(fil.items, _prefix, _localNames);
		if (fil.loopBody)
			for (auto& s: fil.loopBody->body)
				renameVarsInStmt(s, _prefix, _localNames);
	}
	else if (type == "UInt64AugmentedAssignment")
	{
		auto& ua = static_cast<awst::UInt64AugmentedAssignment&>(*_stmt);
		renameVarsInExpr(ua.target, _prefix, _localNames);
		renameVarsInExpr(ua.value, _prefix, _localNames);
	}
	else if (type == "BigUIntAugmentedAssignment")
	{
		auto& ba = static_cast<awst::BigUIntAugmentedAssignment&>(*_stmt);
		renameVarsInExpr(ba.target, _prefix, _localNames);
		renameVarsInExpr(ba.value, _prefix, _localNames);
	}
}

// ─── Expression flattening ──────────────────────────────────────────────────

bool FunctionInliner::flattenBlock(awst::Block& _block)
{
	bool anyFlattened = false;
	std::vector<std::shared_ptr<awst::Statement>> newBody;

	for (auto& stmt: _block.body)
	{
		if (!stmt)
		{
			newBody.push_back(stmt);
			continue;
		}

		std::vector<std::shared_ptr<awst::Statement>> hoisted;
		std::string type = stmt->nodeType();

		if (type == "ExpressionStatement")
		{
			auto& es = static_cast<awst::ExpressionStatement&>(*stmt);
			if (es.expr)
			{
				// For a bare call expression, don't hoist the call itself
				if (es.expr->nodeType() == "SubroutineCallExpression")
				{
					auto& call = static_cast<awst::SubroutineCallExpression&>(*es.expr);
					for (auto& arg: call.args)
						flattenExpr(arg.value, hoisted, stmt->sourceLocation);
				}
				else if (es.expr->nodeType() == "AssignmentExpression")
				{
					auto& ae = static_cast<awst::AssignmentExpression&>(*es.expr);
					flattenExpr(ae.value, hoisted, stmt->sourceLocation);
				}
				else
				{
					flattenExpr(es.expr, hoisted, stmt->sourceLocation);
				}
			}
		}
		else if (type == "AssignmentStatement")
		{
			auto& as = static_cast<awst::AssignmentStatement&>(*stmt);
			// For assignment with a call as value, only flatten the call's arguments
			if (as.value && as.value->nodeType() == "SubroutineCallExpression")
			{
				auto& call = static_cast<awst::SubroutineCallExpression&>(*as.value);
				for (auto& arg: call.args)
					flattenExpr(arg.value, hoisted, stmt->sourceLocation);
			}
			else
			{
				flattenExpr(as.value, hoisted, stmt->sourceLocation);
			}
		}
		else if (type == "ReturnStatement")
		{
			auto& rs = static_cast<awst::ReturnStatement&>(*stmt);
			if (rs.value && rs.value->nodeType() == "SubroutineCallExpression")
			{
				auto& call = static_cast<awst::SubroutineCallExpression&>(*rs.value);
				for (auto& arg: call.args)
					flattenExpr(arg.value, hoisted, stmt->sourceLocation);
			}
			else
			{
				flattenExpr(rs.value, hoisted, stmt->sourceLocation);
			}
		}
		else if (type == "IfElse")
		{
			auto& ie = static_cast<awst::IfElse&>(*stmt);
			// Flatten condition expression — hoist calls out of it
			flattenExpr(ie.condition, hoisted, stmt->sourceLocation);
			// Recurse into branches
			if (ie.ifBranch)
				if (flattenBlock(*ie.ifBranch))
					anyFlattened = true;
			if (ie.elseBranch)
				if (flattenBlock(*ie.elseBranch))
					anyFlattened = true;
		}
		else if (type == "WhileLoop")
		{
			auto& wl = static_cast<awst::WhileLoop&>(*stmt);
			flattenExpr(wl.condition, hoisted, stmt->sourceLocation);
			if (wl.loopBody)
				if (flattenBlock(*wl.loopBody))
					anyFlattened = true;
		}
		else if (type == "Block")
		{
			auto& block = static_cast<awst::Block&>(*stmt);
			if (flattenBlock(block))
				anyFlattened = true;
		}
		else if (type == "Switch")
		{
			auto& sw = static_cast<awst::Switch&>(*stmt);
			flattenExpr(sw.value, hoisted, stmt->sourceLocation);
			for (auto& [_, caseBlock]: sw.cases)
				if (caseBlock && flattenBlock(*caseBlock))
					anyFlattened = true;
			if (sw.defaultCase && flattenBlock(*sw.defaultCase))
				anyFlattened = true;
		}
		else if (type == "ForInLoop")
		{
			auto& fil = static_cast<awst::ForInLoop&>(*stmt);
			if (fil.loopBody && flattenBlock(*fil.loopBody))
				anyFlattened = true;
		}
		else if (type == "UInt64AugmentedAssignment")
		{
			auto& ua = static_cast<awst::UInt64AugmentedAssignment&>(*stmt);
			flattenExpr(ua.value, hoisted, stmt->sourceLocation);
		}
		else if (type == "BigUIntAugmentedAssignment")
		{
			auto& ba = static_cast<awst::BigUIntAugmentedAssignment&>(*stmt);
			flattenExpr(ba.value, hoisted, stmt->sourceLocation);
		}

		if (!hoisted.empty())
		{
			anyFlattened = true;
			for (auto& h: hoisted)
				newBody.push_back(std::move(h));
		}
		newBody.push_back(stmt);
	}

	if (anyFlattened)
		_block.body = std::move(newBody);

	return anyFlattened;
}

void FunctionInliner::flattenExpr(
	std::shared_ptr<awst::Expression>& _expr,
	std::vector<std::shared_ptr<awst::Statement>>& _hoisted,
	awst::SourceLocation const& _loc
)
{
	if (!_expr)
		return;

	std::string type = _expr->nodeType();

	if (type == "SubroutineCallExpression")
	{
		auto& call = static_cast<awst::SubroutineCallExpression&>(*_expr);
		// First, flatten the call's own arguments
		for (auto& arg: call.args)
			flattenExpr(arg.value, _hoisted, _loc);

		// If this call appears as a sub-expression, only hoist if it returns non-void
		if (call.wtype && call.wtype->name() != "void")
		{
			// Hoist the call to a temp variable
			std::string tmpName = "__flat" + std::to_string(m_flattenCounter++) + "_";
			auto tmpVar = awst::makeVarExpression(tmpName, call.wtype, _loc);

			auto assign = std::make_shared<awst::AssignmentStatement>();
			assign->sourceLocation = _loc;
			assign->target = tmpVar;
			assign->value = _expr;
			_hoisted.push_back(assign);

			// Replace the call expression with the temp variable reference
			auto ref = awst::makeVarExpression(tmpName, call.wtype, _loc);
			_expr = ref;
		}
		return;
	}

	// Recurse into child expressions
	if (type == "BigUIntBinaryOperation")
	{
		auto& op = static_cast<awst::BigUIntBinaryOperation&>(*_expr);
		flattenExpr(op.left, _hoisted, _loc);
		flattenExpr(op.right, _hoisted, _loc);
	}
	else if (type == "UInt64BinaryOperation")
	{
		auto& op = static_cast<awst::UInt64BinaryOperation&>(*_expr);
		flattenExpr(op.left, _hoisted, _loc);
		flattenExpr(op.right, _hoisted, _loc);
	}
	else if (type == "BytesBinaryOperation")
	{
		auto& op = static_cast<awst::BytesBinaryOperation&>(*_expr);
		flattenExpr(op.left, _hoisted, _loc);
		flattenExpr(op.right, _hoisted, _loc);
	}
	else if (type == "BytesUnaryOperation")
	{
		auto& op = static_cast<awst::BytesUnaryOperation&>(*_expr);
		flattenExpr(op.expr, _hoisted, _loc);
	}
	else if (type == "NumericComparisonExpression")
	{
		auto& cmp = static_cast<awst::NumericComparisonExpression&>(*_expr);
		flattenExpr(cmp.lhs, _hoisted, _loc);
		flattenExpr(cmp.rhs, _hoisted, _loc);
	}
	else if (type == "BytesComparisonExpression")
	{
		auto& cmp = static_cast<awst::BytesComparisonExpression&>(*_expr);
		flattenExpr(cmp.lhs, _hoisted, _loc);
		flattenExpr(cmp.rhs, _hoisted, _loc);
	}
	else if (type == "BooleanBinaryOperation")
	{
		auto& op = static_cast<awst::BooleanBinaryOperation&>(*_expr);
		flattenExpr(op.left, _hoisted, _loc);
		flattenExpr(op.right, _hoisted, _loc);
	}
	else if (type == "Not")
	{
		auto& n = static_cast<awst::Not&>(*_expr);
		flattenExpr(n.expr, _hoisted, _loc);
	}
	else if (type == "AssertExpression")
	{
		auto& a = static_cast<awst::AssertExpression&>(*_expr);
		flattenExpr(a.condition, _hoisted, _loc);
	}
	else if (type == "ConditionalExpression")
	{
		auto& c = static_cast<awst::ConditionalExpression&>(*_expr);
		flattenExpr(c.condition, _hoisted, _loc);
		flattenExpr(c.trueExpr, _hoisted, _loc);
		flattenExpr(c.falseExpr, _hoisted, _loc);
	}
	else if (type == "IntrinsicCall")
	{
		auto& ic = static_cast<awst::IntrinsicCall&>(*_expr);
		for (auto& arg: ic.stackArgs)
			flattenExpr(arg, _hoisted, _loc);
	}
	else if (type == "PuyaLibCall")
	{
		auto& plc = static_cast<awst::PuyaLibCall&>(*_expr);
		for (auto& arg: plc.args)
			flattenExpr(arg.value, _hoisted, _loc);
	}
	else if (type == "FieldExpression")
	{
		auto& f = static_cast<awst::FieldExpression&>(*_expr);
		flattenExpr(f.base, _hoisted, _loc);
	}
	else if (type == "IndexExpression")
	{
		auto& idx = static_cast<awst::IndexExpression&>(*_expr);
		flattenExpr(idx.base, _hoisted, _loc);
		flattenExpr(idx.index, _hoisted, _loc);
	}
	else if (type == "TupleExpression")
	{
		auto& t = static_cast<awst::TupleExpression&>(*_expr);
		for (auto& item: t.items)
			flattenExpr(item, _hoisted, _loc);
	}
	else if (type == "TupleItemExpression")
	{
		auto& ti = static_cast<awst::TupleItemExpression&>(*_expr);
		flattenExpr(ti.base, _hoisted, _loc);
	}
	else if (type == "ARC4Encode")
	{
		auto& e = static_cast<awst::ARC4Encode&>(*_expr);
		flattenExpr(e.value, _hoisted, _loc);
	}
	else if (type == "ARC4Decode")
	{
		auto& d = static_cast<awst::ARC4Decode&>(*_expr);
		flattenExpr(d.value, _hoisted, _loc);
	}
	else if (type == "ReinterpretCast")
	{
		auto& rc = static_cast<awst::ReinterpretCast&>(*_expr);
		flattenExpr(rc.expr, _hoisted, _loc);
	}
	else if (type == "Copy")
	{
		auto& c = static_cast<awst::Copy&>(*_expr);
		flattenExpr(c.value, _hoisted, _loc);
	}
	else if (type == "SingleEvaluation")
	{
		auto& se = static_cast<awst::SingleEvaluation&>(*_expr);
		flattenExpr(se.source, _hoisted, _loc);
	}
	else if (type == "CheckedMaybe")
	{
		auto& cm = static_cast<awst::CheckedMaybe&>(*_expr);
		flattenExpr(cm.expr, _hoisted, _loc);
	}
	else if (type == "NewArray")
	{
		auto& na = static_cast<awst::NewArray&>(*_expr);
		for (auto& v: na.values)
			flattenExpr(v, _hoisted, _loc);
	}
	else if (type == "ArrayConcat")
	{
		auto& ac = static_cast<awst::ArrayConcat&>(*_expr);
		flattenExpr(ac.left, _hoisted, _loc);
		flattenExpr(ac.right, _hoisted, _loc);
	}
	else if (type == "ArrayExtend")
	{
		auto& ae = static_cast<awst::ArrayExtend&>(*_expr);
		flattenExpr(ae.base, _hoisted, _loc);
		flattenExpr(ae.other, _hoisted, _loc);
	}
	else if (type == "AssignmentExpression")
	{
		auto& a = static_cast<awst::AssignmentExpression&>(*_expr);
		flattenExpr(a.value, _hoisted, _loc);
	}
	else if (type == "StateGet")
	{
		auto& sg = static_cast<awst::StateGet&>(*_expr);
		flattenExpr(sg.field, _hoisted, _loc);
		flattenExpr(sg.defaultValue, _hoisted, _loc);
	}
	else if (type == "BoxValueExpression")
	{
		auto& bve = static_cast<awst::BoxValueExpression&>(*_expr);
		flattenExpr(bve.key, _hoisted, _loc);
	}
	else if (type == "BoxPrefixedKeyExpression")
	{
		auto& bpk = static_cast<awst::BoxPrefixedKeyExpression&>(*_expr);
		flattenExpr(bpk.prefix, _hoisted, _loc);
		flattenExpr(bpk.key, _hoisted, _loc);
	}
	else if (type == "NewStruct")
	{
		auto& ns = static_cast<awst::NewStruct&>(*_expr);
		for (auto& [_, val]: ns.values)
			flattenExpr(val, _hoisted, _loc);
	}
	else if (type == "NamedTupleExpression")
	{
		auto& nt = static_cast<awst::NamedTupleExpression&>(*_expr);
		for (auto& [_, val]: nt.values)
			flattenExpr(val, _hoisted, _loc);
	}
	else if (type == "Emit")
	{
		auto& e = static_cast<awst::Emit&>(*_expr);
		flattenExpr(e.value, _hoisted, _loc);
	}
	else if (type == "CreateInnerTransaction")
	{
		auto& cit = static_cast<awst::CreateInnerTransaction&>(*_expr);
		for (auto& [_, val]: cit.fields)
			flattenExpr(val, _hoisted, _loc);
	}
	else if (type == "SubmitInnerTransaction")
	{
		auto& sit = static_cast<awst::SubmitInnerTransaction&>(*_expr);
		for (auto& itxn: sit.itxns)
			flattenExpr(itxn, _hoisted, _loc);
	}
	else if (type == "CommaExpression")
	{
		auto& ce = static_cast<awst::CommaExpression&>(*_expr);
		for (auto& e: ce.expressions)
			flattenExpr(e, _hoisted, _loc);
	}
}

// ─── Deep copy ──────────────────────────────────────────────────────────────

std::shared_ptr<awst::Expression> FunctionInliner::deepCopyExpr(
	std::shared_ptr<awst::Expression> const& _expr
)
{
	if (!_expr)
		return nullptr;

	std::string type = _expr->nodeType();

	// Leaf nodes
	if (type == "IntegerConstant")
	{
		auto& src = static_cast<awst::IntegerConstant const&>(*_expr);
		auto n = awst::makeIntegerConstant(src.value, src.sourceLocation, src.wtype);
		return n;
	}
	if (type == "BoolConstant")
	{
		auto& src = static_cast<awst::BoolConstant const&>(*_expr);
		return awst::makeBoolConstant(src.value, src.sourceLocation, src.wtype);
	}
	if (type == "BytesConstant")
	{
		auto& src = static_cast<awst::BytesConstant const&>(*_expr);
		return awst::makeBytesConstant(src.value, src.sourceLocation, src.encoding, src.wtype);
	}
	if (type == "StringConstant")
	{
		auto& src = static_cast<awst::StringConstant const&>(*_expr);
		auto n = std::make_shared<awst::StringConstant>();
		n->sourceLocation = src.sourceLocation;
		n->wtype = src.wtype;
		n->value = src.value;
		return n;
	}
	if (type == "VoidConstant")
	{
		auto n = std::make_shared<awst::VoidConstant>();
		n->sourceLocation = _expr->sourceLocation;
		n->wtype = _expr->wtype;
		return n;
	}
	if (type == "VarExpression")
	{
		auto& src = static_cast<awst::VarExpression const&>(*_expr);
		auto n = awst::makeVarExpression(src.name, src.wtype, src.sourceLocation);
		return n;
	}
	if (type == "ARC4Router")
	{
		auto n = std::make_shared<awst::ARC4Router>();
		n->sourceLocation = _expr->sourceLocation;
		n->wtype = _expr->wtype;
		return n;
	}
	if (type == "MethodConstant")
	{
		auto& src = static_cast<awst::MethodConstant const&>(*_expr);
		auto n = std::make_shared<awst::MethodConstant>();
		n->sourceLocation = src.sourceLocation;
		n->wtype = src.wtype;
		n->value = src.value;
		return n;
	}
	if (type == "AddressConstant")
	{
		auto& src = static_cast<awst::AddressConstant const&>(*_expr);
		auto n = std::make_shared<awst::AddressConstant>();
		n->sourceLocation = src.sourceLocation;
		n->wtype = src.wtype;
		n->value = src.value;
		return n;
	}

	// Single sub-expression nodes
	if (type == "Not")
	{
		auto& src = static_cast<awst::Not const&>(*_expr);
		auto n = std::make_shared<awst::Not>();
		n->sourceLocation = src.sourceLocation;
		n->wtype = src.wtype;
		n->expr = deepCopyExpr(src.expr);
		return n;
	}
	if (type == "BytesUnaryOperation")
	{
		auto& src = static_cast<awst::BytesUnaryOperation const&>(*_expr);
		auto n = std::make_shared<awst::BytesUnaryOperation>();
		n->sourceLocation = src.sourceLocation;
		n->wtype = src.wtype;
		n->expr = deepCopyExpr(src.expr);
		n->op = src.op;
		return n;
	}
	if (type == "AssertExpression")
	{
		auto& src = static_cast<awst::AssertExpression const&>(*_expr);
		return awst::makeAssert(
			deepCopyExpr(src.condition), src.sourceLocation, src.errorMessage, src.wtype);
	}
	if (type == "ARC4Encode")
	{
		auto& src = static_cast<awst::ARC4Encode const&>(*_expr);
		auto n = std::make_shared<awst::ARC4Encode>();
		n->sourceLocation = src.sourceLocation;
		n->wtype = src.wtype;
		n->value = deepCopyExpr(src.value);
		return n;
	}
	if (type == "ARC4Decode")
	{
		auto& src = static_cast<awst::ARC4Decode const&>(*_expr);
		auto n = std::make_shared<awst::ARC4Decode>();
		n->sourceLocation = src.sourceLocation;
		n->wtype = src.wtype;
		n->value = deepCopyExpr(src.value);
		return n;
	}
	if (type == "ReinterpretCast")
	{
		auto& src = static_cast<awst::ReinterpretCast const&>(*_expr);
		auto n = std::make_shared<awst::ReinterpretCast>();
		n->sourceLocation = src.sourceLocation;
		n->wtype = src.wtype;
		n->expr = deepCopyExpr(src.expr);
		return n;
	}
	if (type == "Copy")
	{
		auto& src = static_cast<awst::Copy const&>(*_expr);
		auto n = std::make_shared<awst::Copy>();
		n->sourceLocation = src.sourceLocation;
		n->wtype = src.wtype;
		n->value = deepCopyExpr(src.value);
		return n;
	}
	if (type == "SingleEvaluation")
	{
		auto& src = static_cast<awst::SingleEvaluation const&>(*_expr);
		auto n = std::make_shared<awst::SingleEvaluation>();
		n->sourceLocation = src.sourceLocation;
		n->wtype = src.wtype;
		n->source = deepCopyExpr(src.source);
		n->id = src.id;
		return n;
	}
	if (type == "CheckedMaybe")
	{
		auto& src = static_cast<awst::CheckedMaybe const&>(*_expr);
		auto n = std::make_shared<awst::CheckedMaybe>();
		n->sourceLocation = src.sourceLocation;
		n->wtype = src.wtype;
		n->expr = deepCopyExpr(src.expr);
		n->comment = src.comment;
		return n;
	}
	if (type == "Emit")
	{
		auto& src = static_cast<awst::Emit const&>(*_expr);
		auto n = std::make_shared<awst::Emit>();
		n->sourceLocation = src.sourceLocation;
		n->wtype = src.wtype;
		n->signature = src.signature;
		n->value = deepCopyExpr(src.value);
		return n;
	}
	if (type == "ArrayLength")
	{
		auto& src = static_cast<awst::ArrayLength const&>(*_expr);
		auto n = std::make_shared<awst::ArrayLength>();
		n->sourceLocation = src.sourceLocation;
		n->wtype = src.wtype;
		n->array = deepCopyExpr(src.array);
		return n;
	}
	if (type == "ArrayPop")
	{
		auto& src = static_cast<awst::ArrayPop const&>(*_expr);
		auto n = std::make_shared<awst::ArrayPop>();
		n->sourceLocation = src.sourceLocation;
		n->wtype = src.wtype;
		n->base = deepCopyExpr(src.base);
		return n;
	}
	if (type == "FieldExpression")
	{
		auto& src = static_cast<awst::FieldExpression const&>(*_expr);
		auto n = std::make_shared<awst::FieldExpression>();
		n->sourceLocation = src.sourceLocation;
		n->wtype = src.wtype;
		n->base = deepCopyExpr(src.base);
		n->name = src.name;
		return n;
	}
	if (type == "TupleItemExpression")
	{
		auto& src = static_cast<awst::TupleItemExpression const&>(*_expr);
		auto n = std::make_shared<awst::TupleItemExpression>();
		n->sourceLocation = src.sourceLocation;
		n->wtype = src.wtype;
		n->base = deepCopyExpr(src.base);
		n->index = src.index;
		return n;
	}
	if (type == "StateExists")
	{
		auto& src = static_cast<awst::StateExists const&>(*_expr);
		auto n = std::make_shared<awst::StateExists>();
		n->sourceLocation = src.sourceLocation;
		n->wtype = src.wtype;
		n->field = deepCopyExpr(src.field);
		return n;
	}
	if (type == "StateDelete")
	{
		auto& src = static_cast<awst::StateDelete const&>(*_expr);
		auto n = std::make_shared<awst::StateDelete>();
		n->sourceLocation = src.sourceLocation;
		n->wtype = src.wtype;
		n->field = deepCopyExpr(src.field);
		return n;
	}
	if (type == "StateGetEx")
	{
		auto& src = static_cast<awst::StateGetEx const&>(*_expr);
		auto n = std::make_shared<awst::StateGetEx>();
		n->sourceLocation = src.sourceLocation;
		n->wtype = src.wtype;
		n->field = deepCopyExpr(src.field);
		return n;
	}
	if (type == "AppStateExpression")
	{
		auto& src = static_cast<awst::AppStateExpression const&>(*_expr);
		auto n = std::make_shared<awst::AppStateExpression>();
		n->sourceLocation = src.sourceLocation;
		n->wtype = src.wtype;
		n->key = deepCopyExpr(src.key);
		n->existsAssertionMessage = src.existsAssertionMessage;
		return n;
	}
	if (type == "BoxValueExpression")
	{
		auto& src = static_cast<awst::BoxValueExpression const&>(*_expr);
		auto n = std::make_shared<awst::BoxValueExpression>();
		n->sourceLocation = src.sourceLocation;
		n->wtype = src.wtype;
		n->key = deepCopyExpr(src.key);
		n->existsAssertionMessage = src.existsAssertionMessage;
		return n;
	}

	// Two sub-expression nodes
	if (type == "BigUIntBinaryOperation")
	{
		auto& src = static_cast<awst::BigUIntBinaryOperation const&>(*_expr);
		auto n = std::make_shared<awst::BigUIntBinaryOperation>();
		n->sourceLocation = src.sourceLocation;
		n->wtype = src.wtype;
		n->left = deepCopyExpr(src.left);
		n->op = src.op;
		n->right = deepCopyExpr(src.right);
		return n;
	}
	if (type == "UInt64BinaryOperation")
	{
		auto& src = static_cast<awst::UInt64BinaryOperation const&>(*_expr);
		auto n = std::make_shared<awst::UInt64BinaryOperation>();
		n->sourceLocation = src.sourceLocation;
		n->wtype = src.wtype;
		n->left = deepCopyExpr(src.left);
		n->op = src.op;
		n->right = deepCopyExpr(src.right);
		return n;
	}
	if (type == "BytesBinaryOperation")
	{
		auto& src = static_cast<awst::BytesBinaryOperation const&>(*_expr);
		auto n = std::make_shared<awst::BytesBinaryOperation>();
		n->sourceLocation = src.sourceLocation;
		n->wtype = src.wtype;
		n->left = deepCopyExpr(src.left);
		n->op = src.op;
		n->right = deepCopyExpr(src.right);
		return n;
	}
	if (type == "NumericComparisonExpression")
	{
		auto& src = static_cast<awst::NumericComparisonExpression const&>(*_expr);
		auto n = std::make_shared<awst::NumericComparisonExpression>();
		n->sourceLocation = src.sourceLocation;
		n->wtype = src.wtype;
		n->lhs = deepCopyExpr(src.lhs);
		n->op = src.op;
		n->rhs = deepCopyExpr(src.rhs);
		return n;
	}
	if (type == "BytesComparisonExpression")
	{
		auto& src = static_cast<awst::BytesComparisonExpression const&>(*_expr);
		auto n = std::make_shared<awst::BytesComparisonExpression>();
		n->sourceLocation = src.sourceLocation;
		n->wtype = src.wtype;
		n->lhs = deepCopyExpr(src.lhs);
		n->op = src.op;
		n->rhs = deepCopyExpr(src.rhs);
		return n;
	}
	if (type == "BooleanBinaryOperation")
	{
		auto& src = static_cast<awst::BooleanBinaryOperation const&>(*_expr);
		auto n = std::make_shared<awst::BooleanBinaryOperation>();
		n->sourceLocation = src.sourceLocation;
		n->wtype = src.wtype;
		n->left = deepCopyExpr(src.left);
		n->op = src.op;
		n->right = deepCopyExpr(src.right);
		return n;
	}
	if (type == "AssignmentExpression")
	{
		auto& src = static_cast<awst::AssignmentExpression const&>(*_expr);
		auto n = std::make_shared<awst::AssignmentExpression>();
		n->sourceLocation = src.sourceLocation;
		n->wtype = src.wtype;
		n->target = deepCopyExpr(src.target);
		n->value = deepCopyExpr(src.value);
		return n;
	}
	if (type == "IndexExpression")
	{
		auto& src = static_cast<awst::IndexExpression const&>(*_expr);
		auto n = std::make_shared<awst::IndexExpression>();
		n->sourceLocation = src.sourceLocation;
		n->wtype = src.wtype;
		n->base = deepCopyExpr(src.base);
		n->index = deepCopyExpr(src.index);
		return n;
	}
	if (type == "ArrayConcat")
	{
		auto& src = static_cast<awst::ArrayConcat const&>(*_expr);
		auto n = std::make_shared<awst::ArrayConcat>();
		n->sourceLocation = src.sourceLocation;
		n->wtype = src.wtype;
		n->left = deepCopyExpr(src.left);
		n->right = deepCopyExpr(src.right);
		return n;
	}
	if (type == "ArrayExtend")
	{
		auto& src = static_cast<awst::ArrayExtend const&>(*_expr);
		auto n = std::make_shared<awst::ArrayExtend>();
		n->sourceLocation = src.sourceLocation;
		n->wtype = src.wtype;
		n->base = deepCopyExpr(src.base);
		n->other = deepCopyExpr(src.other);
		return n;
	}
	if (type == "BoxPrefixedKeyExpression")
	{
		auto& src = static_cast<awst::BoxPrefixedKeyExpression const&>(*_expr);
		auto n = std::make_shared<awst::BoxPrefixedKeyExpression>();
		n->sourceLocation = src.sourceLocation;
		n->wtype = src.wtype;
		n->prefix = deepCopyExpr(src.prefix);
		n->key = deepCopyExpr(src.key);
		return n;
	}
	if (type == "AppAccountStateExpression")
	{
		auto& src = static_cast<awst::AppAccountStateExpression const&>(*_expr);
		auto n = std::make_shared<awst::AppAccountStateExpression>();
		n->sourceLocation = src.sourceLocation;
		n->wtype = src.wtype;
		n->key = deepCopyExpr(src.key);
		n->account = deepCopyExpr(src.account);
		n->existsAssertionMessage = src.existsAssertionMessage;
		return n;
	}
	if (type == "StateGet")
	{
		auto& src = static_cast<awst::StateGet const&>(*_expr);
		auto n = std::make_shared<awst::StateGet>();
		n->sourceLocation = src.sourceLocation;
		n->wtype = src.wtype;
		n->field = deepCopyExpr(src.field);
		n->defaultValue = deepCopyExpr(src.defaultValue);
		return n;
	}

	// Three sub-expression nodes
	if (type == "ConditionalExpression")
	{
		auto& src = static_cast<awst::ConditionalExpression const&>(*_expr);
		auto n = std::make_shared<awst::ConditionalExpression>();
		n->sourceLocation = src.sourceLocation;
		n->wtype = src.wtype;
		n->condition = deepCopyExpr(src.condition);
		n->trueExpr = deepCopyExpr(src.trueExpr);
		n->falseExpr = deepCopyExpr(src.falseExpr);
		return n;
	}
	if (type == "InnerTransactionField")
	{
		auto& src = static_cast<awst::InnerTransactionField const&>(*_expr);
		auto n = std::make_shared<awst::InnerTransactionField>();
		n->sourceLocation = src.sourceLocation;
		n->wtype = src.wtype;
		n->itxn = deepCopyExpr(src.itxn);
		n->field = src.field;
		n->arrayIndex = deepCopyExpr(src.arrayIndex);
		return n;
	}

	// Vector of sub-expressions
	if (type == "TupleExpression")
	{
		auto& src = static_cast<awst::TupleExpression const&>(*_expr);
		auto n = std::make_shared<awst::TupleExpression>();
		n->sourceLocation = src.sourceLocation;
		n->wtype = src.wtype;
		for (auto const& item: src.items)
			n->items.push_back(deepCopyExpr(item));
		return n;
	}
	if (type == "NewArray")
	{
		auto& src = static_cast<awst::NewArray const&>(*_expr);
		auto n = std::make_shared<awst::NewArray>();
		n->sourceLocation = src.sourceLocation;
		n->wtype = src.wtype;
		for (auto const& v: src.values)
			n->values.push_back(deepCopyExpr(v));
		return n;
	}
	if (type == "CommaExpression")
	{
		auto& src = static_cast<awst::CommaExpression const&>(*_expr);
		auto n = std::make_shared<awst::CommaExpression>();
		n->sourceLocation = src.sourceLocation;
		n->wtype = src.wtype;
		for (auto const& e: src.expressions)
			n->expressions.push_back(deepCopyExpr(e));
		return n;
	}
	if (type == "SubmitInnerTransaction")
	{
		auto& src = static_cast<awst::SubmitInnerTransaction const&>(*_expr);
		auto n = std::make_shared<awst::SubmitInnerTransaction>();
		n->sourceLocation = src.sourceLocation;
		n->wtype = src.wtype;
		for (auto const& itxn: src.itxns)
			n->itxns.push_back(deepCopyExpr(itxn));
		return n;
	}

	// CallArg vectors
	if (type == "SubroutineCallExpression")
	{
		auto& src = static_cast<awst::SubroutineCallExpression const&>(*_expr);
		auto n = std::make_shared<awst::SubroutineCallExpression>();
		n->sourceLocation = src.sourceLocation;
		n->wtype = src.wtype;
		n->target = src.target;
		for (auto const& arg: src.args)
			n->args.push_back({arg.name, deepCopyExpr(arg.value)});
		return n;
	}
	if (type == "PuyaLibCall")
	{
		auto& src = static_cast<awst::PuyaLibCall const&>(*_expr);
		auto n = std::make_shared<awst::PuyaLibCall>();
		n->sourceLocation = src.sourceLocation;
		n->wtype = src.wtype;
		n->func = src.func;
		for (auto const& arg: src.args)
			n->args.push_back({arg.name, deepCopyExpr(arg.value)});
		return n;
	}
	if (type == "IntrinsicCall")
	{
		auto& src = static_cast<awst::IntrinsicCall const&>(*_expr);
		auto n = std::make_shared<awst::IntrinsicCall>();
		n->sourceLocation = src.sourceLocation;
		n->wtype = src.wtype;
		n->opCode = src.opCode;
		n->immediates = src.immediates;
		for (auto const& arg: src.stackArgs)
			n->stackArgs.push_back(deepCopyExpr(arg));
		return n;
	}

	// Map of sub-expressions
	if (type == "NewStruct")
	{
		auto& src = static_cast<awst::NewStruct const&>(*_expr);
		auto n = std::make_shared<awst::NewStruct>();
		n->sourceLocation = src.sourceLocation;
		n->wtype = src.wtype;
		for (auto const& [k, v]: src.values)
			n->values[k] = deepCopyExpr(v);
		return n;
	}
	if (type == "NamedTupleExpression")
	{
		auto& src = static_cast<awst::NamedTupleExpression const&>(*_expr);
		auto n = std::make_shared<awst::NamedTupleExpression>();
		n->sourceLocation = src.sourceLocation;
		n->wtype = src.wtype;
		for (auto const& [k, v]: src.values)
			n->values[k] = deepCopyExpr(v);
		return n;
	}
	if (type == "CreateInnerTransaction")
	{
		auto& src = static_cast<awst::CreateInnerTransaction const&>(*_expr);
		auto n = std::make_shared<awst::CreateInnerTransaction>();
		n->sourceLocation = src.sourceLocation;
		n->wtype = src.wtype;
		for (auto const& [k, v]: src.fields)
			n->fields[k] = deepCopyExpr(v);
		return n;
	}

	// Fallback: return the original shared_ptr (unknown type, no mutation risk for leaves)
	Logger::instance().warning("FunctionInliner: unknown expression type '" + type + "' in deepCopy, sharing");
	return _expr;
}

std::shared_ptr<awst::Block> FunctionInliner::deepCopyBlock(
	std::shared_ptr<awst::Block> const& _block
)
{
	if (!_block)
		return nullptr;
	auto n = std::make_shared<awst::Block>();
	n->sourceLocation = _block->sourceLocation;
	n->label = _block->label;
	n->comment = _block->comment;
	for (auto const& stmt: _block->body)
		n->body.push_back(deepCopyStmt(stmt));
	return n;
}

std::shared_ptr<awst::Statement> FunctionInliner::deepCopyStmt(
	std::shared_ptr<awst::Statement> const& _stmt
)
{
	if (!_stmt)
		return nullptr;

	std::string type = _stmt->nodeType();

	if (type == "ExpressionStatement")
	{
		auto& src = static_cast<awst::ExpressionStatement const&>(*_stmt);
		auto n = std::make_shared<awst::ExpressionStatement>();
		n->sourceLocation = src.sourceLocation;
		n->expr = deepCopyExpr(src.expr);
		return n;
	}
	if (type == "AssignmentStatement")
	{
		auto& src = static_cast<awst::AssignmentStatement const&>(*_stmt);
		auto n = std::make_shared<awst::AssignmentStatement>();
		n->sourceLocation = src.sourceLocation;
		n->target = deepCopyExpr(src.target);
		n->value = deepCopyExpr(src.value);
		return n;
	}
	if (type == "ReturnStatement")
	{
		auto& src = static_cast<awst::ReturnStatement const&>(*_stmt);
		auto n = std::make_shared<awst::ReturnStatement>();
		n->sourceLocation = src.sourceLocation;
		n->value = deepCopyExpr(src.value);
		return n;
	}
	if (type == "IfElse")
	{
		auto& src = static_cast<awst::IfElse const&>(*_stmt);
		auto n = std::make_shared<awst::IfElse>();
		n->sourceLocation = src.sourceLocation;
		n->condition = deepCopyExpr(src.condition);
		n->ifBranch = deepCopyBlock(src.ifBranch);
		n->elseBranch = deepCopyBlock(src.elseBranch);
		return n;
	}
	if (type == "WhileLoop")
	{
		auto& src = static_cast<awst::WhileLoop const&>(*_stmt);
		auto n = std::make_shared<awst::WhileLoop>();
		n->sourceLocation = src.sourceLocation;
		n->condition = deepCopyExpr(src.condition);
		n->loopBody = deepCopyBlock(src.loopBody);
		return n;
	}
	if (type == "Block")
	{
		auto& src = static_cast<awst::Block const&>(*_stmt);
		auto n = std::make_shared<awst::Block>();
		n->sourceLocation = src.sourceLocation;
		n->label = src.label;
		n->comment = src.comment;
		for (auto const& s: src.body)
			n->body.push_back(deepCopyStmt(s));
		return n;
	}
	if (type == "Switch")
	{
		auto& src = static_cast<awst::Switch const&>(*_stmt);
		auto n = std::make_shared<awst::Switch>();
		n->sourceLocation = src.sourceLocation;
		n->value = deepCopyExpr(src.value);
		for (auto const& [caseExpr, caseBlock]: src.cases)
			n->cases.push_back({deepCopyExpr(caseExpr), deepCopyBlock(caseBlock)});
		n->defaultCase = deepCopyBlock(src.defaultCase);
		return n;
	}
	if (type == "ForInLoop")
	{
		auto& src = static_cast<awst::ForInLoop const&>(*_stmt);
		auto n = std::make_shared<awst::ForInLoop>();
		n->sourceLocation = src.sourceLocation;
		n->sequence = deepCopyExpr(src.sequence);
		n->items = deepCopyExpr(src.items);
		n->loopBody = deepCopyBlock(src.loopBody);
		return n;
	}
	if (type == "UInt64AugmentedAssignment")
	{
		auto& src = static_cast<awst::UInt64AugmentedAssignment const&>(*_stmt);
		auto n = std::make_shared<awst::UInt64AugmentedAssignment>();
		n->sourceLocation = src.sourceLocation;
		n->target = deepCopyExpr(src.target);
		n->op = src.op;
		n->value = deepCopyExpr(src.value);
		return n;
	}
	if (type == "BigUIntAugmentedAssignment")
	{
		auto& src = static_cast<awst::BigUIntAugmentedAssignment const&>(*_stmt);
		auto n = std::make_shared<awst::BigUIntAugmentedAssignment>();
		n->sourceLocation = src.sourceLocation;
		n->target = deepCopyExpr(src.target);
		n->op = src.op;
		n->value = deepCopyExpr(src.value);
		return n;
	}
	if (type == "LoopExit")
	{
		auto n = std::make_shared<awst::LoopExit>();
		n->sourceLocation = _stmt->sourceLocation;
		return n;
	}
	if (type == "LoopContinue")
	{
		auto n = std::make_shared<awst::LoopContinue>();
		n->sourceLocation = _stmt->sourceLocation;
		return n;
	}
	if (type == "Goto")
	{
		auto& src = static_cast<awst::Goto const&>(*_stmt);
		auto n = std::make_shared<awst::Goto>();
		n->sourceLocation = src.sourceLocation;
		n->target = src.target;
		return n;
	}

	Logger::instance().warning("FunctionInliner: unknown statement type '" + type + "' in deepCopy, sharing");
	return _stmt;
}

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
