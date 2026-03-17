#include "splitter/FunctionSplitter.h"
#include "splitter/SizeEstimator.h"
#include "Logger.h"

#include <algorithm>
#include <sstream>

namespace puyasol::splitter
{

FunctionSplitter::SplitResult FunctionSplitter::splitOversizedFunctions(
	std::vector<std::shared_ptr<awst::RootNode>>& _roots,
	size_t _maxFunctionInstructions
)
{
	auto& logger = Logger::instance();
	SplitResult result;

	// Run multiple passes. On each pass:
	// 1. Expand calls to rewritten functions (inline their dispatch bodies)
	// 2. Split oversized functions
	// Repeat until no new functions are split.
	for (int pass = 0; pass < 3; ++pass) // limit passes to prevent infinite loops
	{
		// Expand calls to rewritten functions in unsplit functions
		if (pass > 0 && !result.rewrittenFunctions.empty())
		{
			bool anyExpanded = false;
			for (size_t ri = 0; ri < _roots.size(); ++ri)
			{
				auto sub = std::dynamic_pointer_cast<awst::Subroutine>(_roots[ri]);
				if (!sub || !sub->body)
					continue;
				// Don't expand rewritten functions themselves
				if (result.rewrittenFunctions.count(sub->name))
					continue;

				if (expandRewrittenCallees(sub, result.rewrittenFunctions, _roots))
				{
					anyExpanded = true;
					logger.info("  Expanded rewritten callees in '" + sub->name +
						"' (" + std::to_string(sub->body->body.size()) + " statements after expansion)");
				}
			}
			if (!anyExpanded)
				break; // nothing to expand, done
		}

		// Build subroutine ID→body map for dep-aware cost estimation.
		std::map<std::string, awst::Subroutine const*> subById;
		for (auto const& root: _roots)
		{
			if (auto const* sub = dynamic_cast<awst::Subroutine const*>(root.get()))
				subById[sub->id] = sub;
		}

		std::vector<std::shared_ptr<awst::Subroutine>> newSubs;
		bool didSplitThisPass = false;

		for (size_t ri = 0; ri < _roots.size(); ++ri)
		{
			auto sub = std::dynamic_pointer_cast<awst::Subroutine>(_roots[ri]);
			if (!sub || !sub->body)
				continue;

			// Skip already-rewritten functions
			if (result.rewrittenFunctions.count(sub->name))
				continue;

			// Estimate this function's body cost
			size_t bodyCost = SizeEstimator::estimateBlock(*sub->body);

			// Dep-aware cost: add the body costs of direct callees.
			size_t depCost = 0;
			{
				std::set<std::string> directCalleeIds;
				for (auto const& stmt: sub->body->body)
					if (stmt)
						scanStmtForCallees(*stmt, directCalleeIds);

				for (auto const& calleeId: directCalleeIds)
				{
					auto it = subById.find(calleeId);
					if (it != subById.end() && it->second->body)
						depCost += SizeEstimator::estimateBlock(*it->second->body);
				}
			}

			size_t effectiveCost = bodyCost + depCost;

			if (effectiveCost <= _maxFunctionInstructions)
				continue;

			// Need enough top-level statements to have meaningful split points
			auto const& stmts = sub->body->body;
			if (stmts.size() < 2)
				continue;

			logger.info("  Mid-function splitting '" + sub->name +
				"' (body=" + std::to_string(bodyCost) + " + deps=" +
				std::to_string(depCost) + " = " + std::to_string(effectiveCost) +
				" est. instructions, " + std::to_string(stmts.size()) + " statements)");

			// Clone the subroutine before rewriting
			auto clone = std::make_shared<awst::Subroutine>();
			clone->sourceLocation = sub->sourceLocation;
			clone->id = sub->id;
			clone->name = sub->name;
			clone->args = sub->args;
			clone->returnType = sub->returnType;
			clone->documentation = sub->documentation;
			clone->inlineOpt = sub->inlineOpt;
			clone->pure = sub->pure;
			clone->body = std::make_shared<awst::Block>();
			clone->body->sourceLocation = sub->body->sourceLocation;
			clone->body->body = sub->body->body;

			_roots[ri] = clone;
			// Update subById to point at clone (old pointer is now dangling)
			subById[clone->id] = clone.get();

			auto chunks = splitFunction(clone, _maxFunctionInstructions, subById);
			if (!chunks.empty())
			{
				didSplitThisPass = true;
				result.didSplit = true;
				result.rewrittenFunctions.insert(sub->name);

				if (hasMutableSharedParams(*sub, chunks))
				{
					convertToValueBasedIO(clone, chunks);
					logger.info("    Converted '" + sub->name +
						"' to value-based I/O — chunks now distributable");
				}

				for (auto& chunk: chunks)
					newSubs.push_back(std::move(chunk));
			}
		}

		// Append new subroutines to roots
		for (auto& sub: newSubs)
		{
			result.newSubroutines.push_back(sub);
			_roots.push_back(std::move(sub));
		}

		if (!didSplitThisPass)
			break; // no more functions to split
	}

	return result;
}

// ─── Statement analysis ─────────────────────────────────────────────────────

std::vector<FunctionSplitter::StmtInfo> FunctionSplitter::analyzeStatements(
	std::vector<std::shared_ptr<awst::Statement>> const& _stmts
)
{
	std::vector<StmtInfo> infos;
	infos.reserve(_stmts.size());

	for (auto const& stmt: _stmts)
	{
		StmtInfo info;
		if (stmt)
		{
			info.cost = SizeEstimator::estimateStatement(*stmt);
			collectStmtDefs(*stmt, info.defs);
			collectStmtUses(*stmt, info.uses);
		}
		infos.push_back(std::move(info));
	}

	return infos;
}

std::vector<size_t> FunctionSplitter::findSplitPoints(
	std::vector<StmtInfo> const& _infos,
	size_t _maxCost
)
{
	std::vector<size_t> splits;
	size_t accum = 0;

	for (size_t i = 0; i < _infos.size(); ++i)
	{
		accum += _infos[i].cost;
		if (accum > _maxCost && i > 0)
		{
			splits.push_back(i);
			accum = _infos[i].cost; // current stmt starts new chunk
		}
	}

	return splits;
}

std::vector<FunctionSplitter::VarInfo> FunctionSplitter::computeLiveVars(
	std::vector<StmtInfo> const& _infos,
	size_t _splitPoint,
	std::set<std::string> const& _paramNames
)
{
	// Variables defined in statements [0, splitPoint)
	std::set<std::string> definedBefore;
	for (size_t i = 0; i < _splitPoint; ++i)
		definedBefore.insert(_infos[i].defs.begin(), _infos[i].defs.end());

	// Variables used in statements [splitPoint, end)
	std::set<std::string> usedAfter;
	for (size_t i = _splitPoint; i < _infos.size(); ++i)
		usedAfter.insert(_infos[i].uses.begin(), _infos[i].uses.end());

	// Live across = defined before AND used after.
	// Include redefined params: if a param is reassigned in the first chunk,
	// the next chunk needs the NEW value, not the original parameter value.
	// (e.g., Yul `r := shr(127, mul(r, r))` reassigns param `r`)
	std::vector<VarInfo> live;
	for (auto const& var: definedBefore)
	{
		if (!usedAfter.count(var))
			continue;

		VarInfo vi;
		vi.name = var;
		auto it = m_varTypes.find(var);
		vi.wtype = (it != m_varTypes.end()) ? it->second : awst::WType::biguintType();
		live.push_back(vi);
	}

	// Sort for deterministic output
	std::sort(live.begin(), live.end(),
		[](VarInfo const& a, VarInfo const& b) { return a.name < b.name; });

	return live;
}

// ─── The main split logic ───────────────────────────────────────────────────

std::vector<std::shared_ptr<awst::Subroutine>> FunctionSplitter::splitFunction(
	std::shared_ptr<awst::Subroutine> _func,
	size_t _maxCost,
	std::map<std::string, awst::Subroutine const*> const& _subById
)
{
	auto& logger = Logger::instance();
	auto const& stmts = _func->body->body;

	// Reset variable type map
	m_varTypes.clear();

	// Register parameter types
	std::set<std::string> paramNames;
	for (auto const& arg: _func->args)
	{
		paramNames.insert(arg.name);
		m_varTypes[arg.name] = arg.wtype;
	}

	// Collect variable types from the body (into a throwaway set;
	// the side effect we need is populating m_varTypes)
	{
		std::set<std::string> allUses;
		for (auto const& stmt: stmts)
			if (stmt)
				collectStmtUses(*stmt, allUses);
	}

	// Re-analyze (now with type info populated)
	auto infos = analyzeStatements(stmts);

	// Compute dep-aware costs per statement: for statements containing
	// SubroutineCallExpressions, add the callee's body cost. This ensures
	// split points account for the transitive deps each chunk will bring.
	if (!_subById.empty())
	{
		for (size_t i = 0; i < stmts.size(); ++i)
		{
			if (!stmts[i])
				continue;
			std::set<std::string> stmtCalleeIds;
			scanStmtForCallees(*stmts[i], stmtCalleeIds);
			for (auto const& calleeId: stmtCalleeIds)
			{
				auto it = _subById.find(calleeId);
				if (it != _subById.end() && it->second->body)
					infos[i].cost += SizeEstimator::estimateBlock(*it->second->body);
			}
		}
	}

	// Find split points
	auto splitPoints = findSplitPoints(infos, _maxCost);

	if (splitPoints.empty())
	{
		logger.info("    No viable split points found");
		return {};
	}

	// Build chunk ranges: [0, sp0), [sp0, sp1), ..., [spN, end)
	std::vector<std::pair<size_t, size_t>> chunkRanges;
	size_t prev = 0;
	for (size_t sp: splitPoints)
	{
		chunkRanges.push_back({prev, sp});
		prev = sp;
	}
	chunkRanges.push_back({prev, stmts.size()});

	logger.info("    Splitting into " + std::to_string(chunkRanges.size()) + " chunks");

	// Compute live variables at each split point
	std::vector<std::vector<VarInfo>> liveAtSplits;
	for (size_t sp: splitPoints)
		liveAtSplits.push_back(computeLiveVars(infos, sp, paramNames));

	// Log chunk sizes and live var counts
	for (size_t c = 0; c < chunkRanges.size(); ++c)
	{
		size_t chunkCost = 0;
		for (size_t i = chunkRanges[c].first; i < chunkRanges[c].second; ++i)
			chunkCost += infos[i].cost;

		size_t liveCount = (c < liveAtSplits.size()) ? liveAtSplits[c].size() : 0;
		logger.info("    Chunk " + std::to_string(c) + ": stmts [" +
			std::to_string(chunkRanges[c].first) + ", " +
			std::to_string(chunkRanges[c].second) + "), " +
			std::to_string(chunkCost) + " instructions, " +
			std::to_string(liveCount) + " live vars out");
	}

	// ─── Generate chunk subroutines ──────────────────────────────────────

	std::vector<std::shared_ptr<awst::Subroutine>> chunks;

	for (size_t c = 0; c < chunkRanges.size(); ++c)
	{
		auto chunk = std::make_shared<awst::Subroutine>();
		chunk->sourceLocation = _func->sourceLocation;
		chunk->id = _func->id + "__chunk_" + std::to_string(c);
		chunk->name = _func->name + "__chunk_" + std::to_string(c);
		chunk->pure = _func->pure;
		chunk->inlineOpt = false; // Prevent puya from inlining chunks back

		// Args: original function params + live vars from previous split.
		// When a param is redefined and appears as a live var, skip the
		// original param (the live var provides the updated value).
		std::set<std::string> liveNames;
		if (c > 0)
			for (auto const& lv: liveAtSplits[c - 1])
				liveNames.insert(lv.name);

		for (auto const& arg: _func->args)
		{
			if (!liveNames.count(arg.name))
				chunk->args.push_back(arg);
		}
		if (c > 0)
		{
			for (auto const& lv: liveAtSplits[c - 1])
			{
				awst::SubroutineArgument arg;
				arg.name = lv.name;
				arg.wtype = lv.wtype;
				arg.sourceLocation = _func->sourceLocation;
				chunk->args.push_back(arg);
			}
		}

		// Return type: for intermediate chunks, return a tuple of live vars
		// for the final chunk, return the original function's return type
		bool isLast = (c == chunkRanges.size() - 1);

		if (isLast)
		{
			chunk->returnType = _func->returnType;
		}
		else
		{
			auto const& liveOut = liveAtSplits[c];
			if (liveOut.empty())
			{
				chunk->returnType = awst::WType::voidType();
			}
			else if (liveOut.size() == 1)
			{
				chunk->returnType = liveOut[0].wtype;
			}
			else
			{
				std::vector<awst::WType const*> types;
				for (auto const& lv: liveOut)
					types.push_back(lv.wtype);
				chunk->returnType = new awst::WTuple(types);
			}
		}

		// Body: slice of original statements
		auto body = std::make_shared<awst::Block>();
		body->sourceLocation = _func->sourceLocation;
		for (size_t i = chunkRanges[c].first; i < chunkRanges[c].second; ++i)
			body->body.push_back(stmts[i]);

		// For intermediate chunks: rewrite inner returns and add final return with live vars
		if (!isLast)
		{
			// Ensure live-out vars have default init before any inner returns
			auto const& liveOut = liveAtSplits[c];
			if (!liveOut.empty())
				ensureLiveOutVarsInitialized(*body, liveOut, _func->sourceLocation);

			// Rewrite any inner ReturnStatements to match chunk return type.
			// Do this BEFORE appending the final return statement.
			if (!liveOut.empty())
				rewriteInnerReturns(*body, chunk->returnType, liveOut, _func->sourceLocation);

			if (!liveOut.empty())
			{
				auto ret = std::make_shared<awst::ReturnStatement>();
				ret->sourceLocation = _func->sourceLocation;

				if (liveOut.size() == 1)
				{
					auto var = std::make_shared<awst::VarExpression>();
					var->sourceLocation = _func->sourceLocation;
					var->wtype = liveOut[0].wtype;
					var->name = liveOut[0].name;
					ret->value = var;
				}
				else
				{
					auto tuple = std::make_shared<awst::TupleExpression>();
					tuple->sourceLocation = _func->sourceLocation;
					tuple->wtype = chunk->returnType;
					for (auto const& lv: liveOut)
					{
						auto var = std::make_shared<awst::VarExpression>();
						var->sourceLocation = _func->sourceLocation;
						var->wtype = lv.wtype;
						var->name = lv.name;
						tuple->items.push_back(var);
					}
					ret->value = tuple;
				}

				body->body.push_back(ret);
			}
		}

		chunk->body = body;
		chunks.push_back(chunk);
	}

	// ─── Rewrite original function to call chunks ────────────────────────

	auto newBody = std::make_shared<awst::Block>();
	newBody->sourceLocation = _func->sourceLocation;

	for (size_t c = 0; c < chunks.size(); ++c)
	{
		bool isLast = (c == chunks.size() - 1);

		// Build the call expression
		auto call = std::make_shared<awst::SubroutineCallExpression>();
		call->sourceLocation = _func->sourceLocation;
		call->wtype = chunks[c]->returnType;
		call->target = awst::SubroutineID{chunks[c]->id};

		// Pass original function params (skip those replaced by live vars)
		std::set<std::string> callLiveNames;
		if (c > 0)
			for (auto const& lv: liveAtSplits[c - 1])
				callLiveNames.insert(lv.name);

		for (auto const& arg: _func->args)
		{
			if (callLiveNames.count(arg.name))
				continue;
			auto var = std::make_shared<awst::VarExpression>();
			var->sourceLocation = _func->sourceLocation;
			var->wtype = arg.wtype;
			var->name = arg.name;
			call->args.push_back(awst::CallArg{arg.name, var});
		}

		// Pass live vars from previous split
		if (c > 0)
		{
			for (auto const& lv: liveAtSplits[c - 1])
			{
				auto var = std::make_shared<awst::VarExpression>();
				var->sourceLocation = _func->sourceLocation;
				var->wtype = lv.wtype;
				var->name = lv.name;
				call->args.push_back(awst::CallArg{lv.name, var});
			}
		}

		if (isLast)
		{
			// Last chunk: if the original function returns something, return the call result.
			// Otherwise just call it as a statement.
			if (_func->returnType != awst::WType::voidType())
			{
				auto ret = std::make_shared<awst::ReturnStatement>();
				ret->sourceLocation = _func->sourceLocation;
				ret->value = call;
				newBody->body.push_back(ret);
			}
			else
			{
				auto stmt = std::make_shared<awst::ExpressionStatement>();
				stmt->sourceLocation = _func->sourceLocation;
				stmt->expr = call;
				newBody->body.push_back(stmt);
			}
		}
		else
		{
			auto const& liveOut = liveAtSplits[c];

			if (liveOut.empty())
			{
				// No live vars to unpack, just call it
				auto stmt = std::make_shared<awst::ExpressionStatement>();
				stmt->sourceLocation = _func->sourceLocation;
				stmt->expr = call;
				newBody->body.push_back(stmt);
			}
			else if (liveOut.size() == 1)
			{
				// Single live var: assign directly
				auto assign = std::make_shared<awst::AssignmentStatement>();
				assign->sourceLocation = _func->sourceLocation;
				auto target = std::make_shared<awst::VarExpression>();
				target->sourceLocation = _func->sourceLocation;
				target->wtype = liveOut[0].wtype;
				target->name = liveOut[0].name;
				assign->target = target;
				assign->value = call;
				newBody->body.push_back(assign);
			}
			else
			{
				// Multiple live vars: wrap call in SingleEvaluation, then unpack
				auto se = std::make_shared<awst::SingleEvaluation>();
				se->sourceLocation = _func->sourceLocation;
				se->wtype = call->wtype;
				se->source = call;
				se->id = m_nextSingleEvalId++;

				for (size_t j = 0; j < liveOut.size(); ++j)
				{
					auto assign = std::make_shared<awst::AssignmentStatement>();
					assign->sourceLocation = _func->sourceLocation;

					auto target = std::make_shared<awst::VarExpression>();
					target->sourceLocation = _func->sourceLocation;
					target->wtype = liveOut[j].wtype;
					target->name = liveOut[j].name;
					assign->target = target;

					auto item = std::make_shared<awst::TupleItemExpression>();
					item->sourceLocation = _func->sourceLocation;
					item->wtype = liveOut[j].wtype;
					item->base = se;
					item->index = static_cast<int>(j);
					assign->value = item;

					newBody->body.push_back(assign);
				}
			}
		}
	}

	_func->body = newBody;

	logger.info("    Rewrote '" + _func->name + "' to call " +
		std::to_string(chunks.size()) + " chunks");

	return chunks;
}

// ─── Variable collection ────────────────────────────────────────────────────

void FunctionSplitter::collectExprUses(
	awst::Expression const& _expr,
	std::set<std::string>& _uses
)
{
	std::string type = _expr.nodeType();

	if (type == "VarExpression")
	{
		auto const& var = static_cast<awst::VarExpression const&>(_expr);
		_uses.insert(var.name);
		if (var.wtype)
			m_varTypes[var.name] = var.wtype;
		return;
	}

	// Binary operations
	if (type == "UInt64BinaryOperation")
	{
		auto const& op = static_cast<awst::UInt64BinaryOperation const&>(_expr);
		if (op.left) collectExprUses(*op.left, _uses);
		if (op.right) collectExprUses(*op.right, _uses);
	}
	else if (type == "BigUIntBinaryOperation")
	{
		auto const& op = static_cast<awst::BigUIntBinaryOperation const&>(_expr);
		if (op.left) collectExprUses(*op.left, _uses);
		if (op.right) collectExprUses(*op.right, _uses);
	}
	else if (type == "BytesBinaryOperation")
	{
		auto const& op = static_cast<awst::BytesBinaryOperation const&>(_expr);
		if (op.left) collectExprUses(*op.left, _uses);
		if (op.right) collectExprUses(*op.right, _uses);
	}
	else if (type == "BytesUnaryOperation")
	{
		auto const& op = static_cast<awst::BytesUnaryOperation const&>(_expr);
		if (op.expr) collectExprUses(*op.expr, _uses);
	}
	else if (type == "NumericComparisonExpression")
	{
		auto const& cmp = static_cast<awst::NumericComparisonExpression const&>(_expr);
		if (cmp.lhs) collectExprUses(*cmp.lhs, _uses);
		if (cmp.rhs) collectExprUses(*cmp.rhs, _uses);
	}
	else if (type == "BytesComparisonExpression")
	{
		auto const& cmp = static_cast<awst::BytesComparisonExpression const&>(_expr);
		if (cmp.lhs) collectExprUses(*cmp.lhs, _uses);
		if (cmp.rhs) collectExprUses(*cmp.rhs, _uses);
	}
	else if (type == "BooleanBinaryOperation")
	{
		auto const& op = static_cast<awst::BooleanBinaryOperation const&>(_expr);
		if (op.left) collectExprUses(*op.left, _uses);
		if (op.right) collectExprUses(*op.right, _uses);
	}
	else if (type == "Not")
	{
		auto const& n = static_cast<awst::Not const&>(_expr);
		if (n.expr) collectExprUses(*n.expr, _uses);
	}
	else if (type == "AssertExpression")
	{
		auto const& a = static_cast<awst::AssertExpression const&>(_expr);
		if (a.condition) collectExprUses(*a.condition, _uses);
	}
	else if (type == "AssignmentExpression")
	{
		auto const& a = static_cast<awst::AssignmentExpression const&>(_expr);
		if (a.target) collectExprUses(*a.target, _uses);
		if (a.value) collectExprUses(*a.value, _uses);
	}
	else if (type == "ConditionalExpression")
	{
		auto const& c = static_cast<awst::ConditionalExpression const&>(_expr);
		if (c.condition) collectExprUses(*c.condition, _uses);
		if (c.trueExpr) collectExprUses(*c.trueExpr, _uses);
		if (c.falseExpr) collectExprUses(*c.falseExpr, _uses);
	}
	else if (type == "SubroutineCallExpression")
	{
		auto const& call = static_cast<awst::SubroutineCallExpression const&>(_expr);
		for (auto const& arg: call.args)
			if (arg.value) collectExprUses(*arg.value, _uses);
	}
	else if (type == "IntrinsicCall")
	{
		auto const& ic = static_cast<awst::IntrinsicCall const&>(_expr);
		for (auto const& arg: ic.stackArgs)
			if (arg) collectExprUses(*arg, _uses);
	}
	else if (type == "PuyaLibCall")
	{
		auto const& plc = static_cast<awst::PuyaLibCall const&>(_expr);
		for (auto const& arg: plc.args)
			if (arg.value) collectExprUses(*arg.value, _uses);
	}
	else if (type == "FieldExpression")
	{
		auto const& f = static_cast<awst::FieldExpression const&>(_expr);
		if (f.base) collectExprUses(*f.base, _uses);
	}
	else if (type == "IndexExpression")
	{
		auto const& idx = static_cast<awst::IndexExpression const&>(_expr);
		if (idx.base) collectExprUses(*idx.base, _uses);
		if (idx.index) collectExprUses(*idx.index, _uses);
	}
	else if (type == "TupleExpression")
	{
		auto const& t = static_cast<awst::TupleExpression const&>(_expr);
		for (auto const& item: t.items)
			if (item) collectExprUses(*item, _uses);
	}
	else if (type == "TupleItemExpression")
	{
		auto const& ti = static_cast<awst::TupleItemExpression const&>(_expr);
		if (ti.base) collectExprUses(*ti.base, _uses);
	}
	else if (type == "ARC4Encode")
	{
		auto const& e = static_cast<awst::ARC4Encode const&>(_expr);
		if (e.value) collectExprUses(*e.value, _uses);
	}
	else if (type == "ARC4Decode")
	{
		auto const& d = static_cast<awst::ARC4Decode const&>(_expr);
		if (d.value) collectExprUses(*d.value, _uses);
	}
	else if (type == "ReinterpretCast")
	{
		auto const& rc = static_cast<awst::ReinterpretCast const&>(_expr);
		if (rc.expr) collectExprUses(*rc.expr, _uses);
	}
	else if (type == "Copy")
	{
		auto const& c = static_cast<awst::Copy const&>(_expr);
		if (c.value) collectExprUses(*c.value, _uses);
	}
	else if (type == "SingleEvaluation")
	{
		auto const& se = static_cast<awst::SingleEvaluation const&>(_expr);
		if (se.source) collectExprUses(*se.source, _uses);
	}
	else if (type == "CheckedMaybe")
	{
		auto const& cm = static_cast<awst::CheckedMaybe const&>(_expr);
		if (cm.expr) collectExprUses(*cm.expr, _uses);
	}
	else if (type == "NewArray")
	{
		auto const& na = static_cast<awst::NewArray const&>(_expr);
		for (auto const& v: na.values)
			if (v) collectExprUses(*v, _uses);
	}
	else if (type == "ArrayLength")
	{
		auto const& al = static_cast<awst::ArrayLength const&>(_expr);
		if (al.array) collectExprUses(*al.array, _uses);
	}
	else if (type == "ArrayPop")
	{
		auto const& ap = static_cast<awst::ArrayPop const&>(_expr);
		if (ap.base) collectExprUses(*ap.base, _uses);
	}
	else if (type == "ArrayConcat")
	{
		auto const& ac = static_cast<awst::ArrayConcat const&>(_expr);
		if (ac.left) collectExprUses(*ac.left, _uses);
		if (ac.right) collectExprUses(*ac.right, _uses);
	}
	else if (type == "ArrayExtend")
	{
		auto const& ae = static_cast<awst::ArrayExtend const&>(_expr);
		if (ae.base) collectExprUses(*ae.base, _uses);
		if (ae.other) collectExprUses(*ae.other, _uses);
	}
	else if (type == "StateGet")
	{
		auto const& sg = static_cast<awst::StateGet const&>(_expr);
		if (sg.field) collectExprUses(*sg.field, _uses);
		if (sg.defaultValue) collectExprUses(*sg.defaultValue, _uses);
	}
	else if (type == "StateExists")
	{
		auto const& se = static_cast<awst::StateExists const&>(_expr);
		if (se.field) collectExprUses(*se.field, _uses);
	}
	else if (type == "StateDelete")
	{
		auto const& sd = static_cast<awst::StateDelete const&>(_expr);
		if (sd.field) collectExprUses(*sd.field, _uses);
	}
	else if (type == "StateGetEx")
	{
		auto const& sge = static_cast<awst::StateGetEx const&>(_expr);
		if (sge.field) collectExprUses(*sge.field, _uses);
	}
	else if (type == "BoxPrefixedKeyExpression")
	{
		auto const& bpk = static_cast<awst::BoxPrefixedKeyExpression const&>(_expr);
		if (bpk.prefix) collectExprUses(*bpk.prefix, _uses);
		if (bpk.key) collectExprUses(*bpk.key, _uses);
	}
	else if (type == "BoxValueExpression")
	{
		auto const& bve = static_cast<awst::BoxValueExpression const&>(_expr);
		if (bve.key) collectExprUses(*bve.key, _uses);
	}
	else if (type == "NewStruct")
	{
		auto const& ns = static_cast<awst::NewStruct const&>(_expr);
		for (auto const& [_, val]: ns.values)
			if (val) collectExprUses(*val, _uses);
	}
	else if (type == "NamedTupleExpression")
	{
		auto const& nt = static_cast<awst::NamedTupleExpression const&>(_expr);
		for (auto const& [_, val]: nt.values)
			if (val) collectExprUses(*val, _uses);
	}
	else if (type == "Emit")
	{
		auto const& e = static_cast<awst::Emit const&>(_expr);
		if (e.value) collectExprUses(*e.value, _uses);
	}
	else if (type == "CreateInnerTransaction")
	{
		auto const& cit = static_cast<awst::CreateInnerTransaction const&>(_expr);
		for (auto const& [_, val]: cit.fields)
			if (val) collectExprUses(*val, _uses);
	}
	else if (type == "SubmitInnerTransaction")
	{
		auto const& sit = static_cast<awst::SubmitInnerTransaction const&>(_expr);
		for (auto const& itxn: sit.itxns)
			if (itxn) collectExprUses(*itxn, _uses);
	}
	else if (type == "InnerTransactionField")
	{
		auto const& itf = static_cast<awst::InnerTransactionField const&>(_expr);
		if (itf.itxn) collectExprUses(*itf.itxn, _uses);
	}
	else if (type == "CommaExpression")
	{
		auto const& ce = static_cast<awst::CommaExpression const&>(_expr);
		for (auto const& e: ce.expressions)
			if (e) collectExprUses(*e, _uses);
	}
}

void FunctionSplitter::collectStmtUses(
	awst::Statement const& _stmt,
	std::set<std::string>& _uses
)
{
	std::string type = _stmt.nodeType();

	if (type == "Block")
	{
		auto const& block = static_cast<awst::Block const&>(_stmt);
		for (auto const& s: block.body)
			if (s) collectStmtUses(*s, _uses);
	}
	else if (type == "ExpressionStatement")
	{
		auto const& es = static_cast<awst::ExpressionStatement const&>(_stmt);
		if (es.expr) collectExprUses(*es.expr, _uses);
	}
	else if (type == "ReturnStatement")
	{
		auto const& rs = static_cast<awst::ReturnStatement const&>(_stmt);
		if (rs.value) collectExprUses(*rs.value, _uses);
	}
	else if (type == "IfElse")
	{
		auto const& ie = static_cast<awst::IfElse const&>(_stmt);
		if (ie.condition) collectExprUses(*ie.condition, _uses);
		if (ie.ifBranch) collectStmtUses(*ie.ifBranch, _uses);
		if (ie.elseBranch) collectStmtUses(*ie.elseBranch, _uses);
	}
	else if (type == "WhileLoop")
	{
		auto const& wl = static_cast<awst::WhileLoop const&>(_stmt);
		if (wl.condition) collectExprUses(*wl.condition, _uses);
		if (wl.loopBody) collectStmtUses(*wl.loopBody, _uses);
	}
	else if (type == "AssignmentStatement")
	{
		auto const& as = static_cast<awst::AssignmentStatement const&>(_stmt);
		if (as.target) collectExprUses(*as.target, _uses);
		if (as.value) collectExprUses(*as.value, _uses);
	}
	else if (type == "Switch")
	{
		auto const& sw = static_cast<awst::Switch const&>(_stmt);
		if (sw.value) collectExprUses(*sw.value, _uses);
		for (auto const& [caseExpr, caseBlock]: sw.cases)
		{
			if (caseExpr) collectExprUses(*caseExpr, _uses);
			if (caseBlock) collectStmtUses(*caseBlock, _uses);
		}
		if (sw.defaultCase) collectStmtUses(*sw.defaultCase, _uses);
	}
	else if (type == "ForInLoop")
	{
		auto const& fil = static_cast<awst::ForInLoop const&>(_stmt);
		if (fil.sequence) collectExprUses(*fil.sequence, _uses);
		if (fil.items) collectExprUses(*fil.items, _uses);
		if (fil.loopBody) collectStmtUses(*fil.loopBody, _uses);
	}
	else if (type == "UInt64AugmentedAssignment")
	{
		auto const& ua = static_cast<awst::UInt64AugmentedAssignment const&>(_stmt);
		if (ua.target) collectExprUses(*ua.target, _uses);
		if (ua.value) collectExprUses(*ua.value, _uses);
	}
	else if (type == "BigUIntAugmentedAssignment")
	{
		auto const& ba = static_cast<awst::BigUIntAugmentedAssignment const&>(_stmt);
		if (ba.target) collectExprUses(*ba.target, _uses);
		if (ba.value) collectExprUses(*ba.value, _uses);
	}
}

void FunctionSplitter::collectStmtDefs(
	awst::Statement const& _stmt,
	std::set<std::string>& _defs
)
{
	std::string type = _stmt.nodeType();

	if (type == "AssignmentStatement")
	{
		auto const& as = static_cast<awst::AssignmentStatement const&>(_stmt);
		if (as.target)
		{
			std::string targetType = as.target->nodeType();
			if (targetType == "VarExpression")
			{
				auto const& var = static_cast<awst::VarExpression const&>(*as.target);
				_defs.insert(var.name);
				if (var.wtype)
					m_varTypes[var.name] = var.wtype;
			}
		}
	}
	else if (type == "ExpressionStatement")
	{
		auto const& es = static_cast<awst::ExpressionStatement const&>(_stmt);
		if (es.expr && es.expr->nodeType() == "AssignmentExpression")
		{
			auto const& ae = static_cast<awst::AssignmentExpression const&>(*es.expr);
			if (ae.target)
			{
				std::string targetType = ae.target->nodeType();
				if (targetType == "VarExpression")
				{
					auto const& var = static_cast<awst::VarExpression const&>(*ae.target);
					_defs.insert(var.name);
					if (var.wtype)
						m_varTypes[var.name] = var.wtype;
				}
				else if (targetType == "FieldExpression")
				{
					// Modifying a struct field → the struct variable is def'd
					auto const& fe = static_cast<awst::FieldExpression const&>(*ae.target);
					if (fe.base && fe.base->nodeType() == "VarExpression")
					{
						auto const& baseVar = static_cast<awst::VarExpression const&>(*fe.base);
						_defs.insert(baseVar.name);
						if (baseVar.wtype)
							m_varTypes[baseVar.name] = baseVar.wtype;
					}
				}
				// IndexExpression targets (evals[N]) — we don't add the array to defs
				// because it's a reference array (slot-based), modifications persist
			}
		}
	}
	else if (type == "UInt64AugmentedAssignment")
	{
		auto const& ua = static_cast<awst::UInt64AugmentedAssignment const&>(_stmt);
		if (ua.target && ua.target->nodeType() == "VarExpression")
		{
			auto const& var = static_cast<awst::VarExpression const&>(*ua.target);
			_defs.insert(var.name);
		}
	}
	else if (type == "BigUIntAugmentedAssignment")
	{
		auto const& ba = static_cast<awst::BigUIntAugmentedAssignment const&>(_stmt);
		if (ba.target && ba.target->nodeType() == "VarExpression")
		{
			auto const& var = static_cast<awst::VarExpression const&>(*ba.target);
			_defs.insert(var.name);
		}
	}
	// Recurse into compound statements to find nested defs
	else if (type == "Switch")
	{
		auto const& sw = static_cast<awst::Switch const&>(_stmt);
		for (auto const& [caseExpr, caseBlock]: sw.cases)
		{
			if (caseBlock)
				for (auto const& s: caseBlock->body)
					if (s) collectStmtDefs(*s, _defs);
		}
		if (sw.defaultCase)
			for (auto const& s: sw.defaultCase->body)
				if (s) collectStmtDefs(*s, _defs);
	}
	else if (type == "IfElse")
	{
		auto const& ie = static_cast<awst::IfElse const&>(_stmt);
		if (ie.ifBranch)
			for (auto const& s: ie.ifBranch->body)
				if (s) collectStmtDefs(*s, _defs);
		if (ie.elseBranch)
			for (auto const& s: ie.elseBranch->body)
				if (s) collectStmtDefs(*s, _defs);
	}
	else if (type == "WhileLoop")
	{
		auto const& wl = static_cast<awst::WhileLoop const&>(_stmt);
		if (wl.loopBody)
			for (auto const& s: wl.loopBody->body)
				if (s) collectStmtDefs(*s, _defs);
	}
	else if (type == "Block")
	{
		auto const& bl = static_cast<awst::Block const&>(_stmt);
		for (auto const& s: bl.body)
			if (s) collectStmtDefs(*s, _defs);
	}
}

// ─── Mutable shared param detection ─────────────────────────────────────────

bool FunctionSplitter::hasMutableSharedParams(
	awst::Subroutine const& _func,
	std::vector<std::shared_ptr<awst::Subroutine>> const& _chunks
)
{
	// Find ReferenceArray-typed parameters
	std::set<std::string> refParamNames;
	for (auto const& arg: _func.args)
	{
		if (arg.wtype && arg.wtype->kind() == awst::WTypeKind::ReferenceArray)
			refParamNames.insert(arg.name);
	}

	if (refParamNames.empty())
		return false;

	// Check if any chunk modifies a ReferenceArray param
	for (auto const& chunk: _chunks)
	{
		if (!chunk->body)
			continue;
		for (auto const& stmt: chunk->body->body)
		{
			if (stmt && stmtModifiesRefParam(*stmt, refParamNames))
				return true;
		}
	}

	return false;
}

bool FunctionSplitter::stmtModifiesRefParam(
	awst::Statement const& _stmt,
	std::set<std::string> const& _refParamNames
)
{
	std::string type = _stmt.nodeType();

	if (type == "AssignmentStatement")
	{
		auto const& as = static_cast<awst::AssignmentStatement const&>(_stmt);
		if (as.target && exprModifiesRefParam(*as.target, _refParamNames))
			return true;
	}
	else if (type == "ExpressionStatement")
	{
		auto const& es = static_cast<awst::ExpressionStatement const&>(_stmt);
		if (es.expr && es.expr->nodeType() == "AssignmentExpression")
		{
			auto const& ae = static_cast<awst::AssignmentExpression const&>(*es.expr);
			if (ae.target && exprModifiesRefParam(*ae.target, _refParamNames))
				return true;
		}
	}
	else if (type == "Block")
	{
		auto const& block = static_cast<awst::Block const&>(_stmt);
		for (auto const& s: block.body)
			if (s && stmtModifiesRefParam(*s, _refParamNames))
				return true;
	}
	else if (type == "IfElse")
	{
		auto const& ie = static_cast<awst::IfElse const&>(_stmt);
		if (ie.ifBranch && stmtModifiesRefParam(*ie.ifBranch, _refParamNames))
			return true;
		if (ie.elseBranch && stmtModifiesRefParam(*ie.elseBranch, _refParamNames))
			return true;
	}
	else if (type == "WhileLoop")
	{
		auto const& wl = static_cast<awst::WhileLoop const&>(_stmt);
		if (wl.loopBody && stmtModifiesRefParam(*wl.loopBody, _refParamNames))
			return true;
	}

	return false;
}

bool FunctionSplitter::exprModifiesRefParam(
	awst::Expression const& _expr,
	std::set<std::string> const& _refParamNames
)
{
	std::string type = _expr.nodeType();

	// Direct write: target is FieldExpression on a ReferenceArray param
	if (type == "FieldExpression")
	{
		auto const& fe = static_cast<awst::FieldExpression const&>(_expr);
		if (fe.base && fe.base->nodeType() == "VarExpression")
		{
			auto const& var = static_cast<awst::VarExpression const&>(*fe.base);
			return _refParamNames.count(var.name) > 0;
		}
	}

	// Direct write: target is IndexExpression on a ReferenceArray param
	if (type == "IndexExpression")
	{
		auto const& idx = static_cast<awst::IndexExpression const&>(_expr);
		if (idx.base && idx.base->nodeType() == "VarExpression")
		{
			auto const& var = static_cast<awst::VarExpression const&>(*idx.base);
			return _refParamNames.count(var.name) > 0;
		}
	}

	return false;
}

// ─── Value-based I/O conversion ──────────────────────────────────────────────

std::vector<FunctionSplitter::RefParamWrite> FunctionSplitter::collectRefParamWrites(
	awst::Block const& _body,
	std::set<std::string> const& _refParamNames
)
{
	std::vector<RefParamWrite> writes;

	for (auto const& stmt: _body.body)
	{
		if (!stmt)
			continue;

		// Pattern 1: AssignmentStatement with IndexExpression target
		if (stmt->nodeType() == "AssignmentStatement")
		{
			auto const& as = static_cast<awst::AssignmentStatement const&>(*stmt);
			if (as.target && as.target->nodeType() == "IndexExpression")
			{
				auto const& idx = static_cast<awst::IndexExpression const&>(*as.target);
				if (idx.base && idx.base->nodeType() == "VarExpression" &&
					idx.index && idx.index->nodeType() == "IntegerConstant")
				{
					auto const& var = static_cast<awst::VarExpression const&>(*idx.base);
					if (_refParamNames.count(var.name))
					{
						auto const& ic = static_cast<awst::IntegerConstant const&>(*idx.index);
						int index = std::stoi(ic.value);
						RefParamWrite w;
						w.paramName = var.name;
						w.index = index;
						w.localVarName = "_" + var.name + "_" + std::to_string(index);
						w.wtype = idx.wtype ? idx.wtype : awst::WType::biguintType();
						writes.push_back(w);
					}
				}
			}
		}
		// Pattern 2: ExpressionStatement → AssignmentExpression
		else if (stmt->nodeType() == "ExpressionStatement")
		{
			auto const& es = static_cast<awst::ExpressionStatement const&>(*stmt);
			if (es.expr && es.expr->nodeType() == "AssignmentExpression")
			{
				auto const& ae = static_cast<awst::AssignmentExpression const&>(*es.expr);
				if (ae.target && ae.target->nodeType() == "IndexExpression")
				{
					auto const& idx = static_cast<awst::IndexExpression const&>(*ae.target);
					if (idx.base && idx.base->nodeType() == "VarExpression" &&
						idx.index && idx.index->nodeType() == "IntegerConstant")
					{
						auto const& var = static_cast<awst::VarExpression const&>(*idx.base);
						if (_refParamNames.count(var.name))
						{
							auto const& ic = static_cast<awst::IntegerConstant const&>(*idx.index);
							int index = std::stoi(ic.value);
							RefParamWrite w;
							w.paramName = var.name;
							w.index = index;
							w.localVarName = "_" + var.name + "_" + std::to_string(index);
							w.wtype = idx.wtype ? idx.wtype : awst::WType::biguintType();
							writes.push_back(w);
						}
					}
				}
			}
		}
	}

	return writes;
}

void FunctionSplitter::convertToValueBasedIO(
	std::shared_ptr<awst::Subroutine> _parent,
	std::vector<std::shared_ptr<awst::Subroutine>>& _chunks
)
{
	auto& logger = Logger::instance();

	// Find ReferenceArray params
	std::set<std::string> refParamNames;
	for (auto const& arg: _parent->args)
	{
		if (arg.wtype && arg.wtype->kind() == awst::WTypeKind::ReferenceArray)
			refParamNames.insert(arg.name);
	}

	if (refParamNames.empty())
		return;

	// Collect writes per chunk and all writes globally
	struct ChunkWriteInfo
	{
		std::vector<RefParamWrite> writes;
	};
	std::vector<ChunkWriteInfo> chunkWrites(_chunks.size());
	std::vector<RefParamWrite> allWrites; // for writeback in parent

	for (size_t c = 0; c < _chunks.size(); ++c)
	{
		if (!_chunks[c]->body)
			continue;
		chunkWrites[c].writes = collectRefParamWrites(*_chunks[c]->body, refParamNames);
		for (auto const& w: chunkWrites[c].writes)
		{
			// Deduplicate by localVarName
			bool found = false;
			for (auto const& aw: allWrites)
			{
				if (aw.localVarName == w.localVarName)
				{
					found = true;
					break;
				}
			}
			if (!found)
				allWrites.push_back(w);
		}
	}

	if (allWrites.empty())
		return;

	// Sort allWrites by paramName then index for deterministic output
	std::sort(allWrites.begin(), allWrites.end(),
		[](RefParamWrite const& a, RefParamWrite const& b)
		{
			if (a.paramName != b.paramName)
				return a.paramName < b.paramName;
			return a.index < b.index;
		});

	logger.info("    Value-based I/O: " + std::to_string(allWrites.size()) +
		" ref param writes across " + std::to_string(_chunks.size()) + " chunks");

	// ── Transform each chunk ─────────────────────────────────────────────

	for (size_t c = 0; c < _chunks.size(); ++c)
	{
		auto& chunk = _chunks[c];
		auto const& writes = chunkWrites[c].writes;

		// 1. Remove ref params from chunk args
		std::vector<awst::SubroutineArgument> newArgs;
		for (auto const& arg: chunk->args)
		{
			if (!refParamNames.count(arg.name))
				newArgs.push_back(arg);
		}
		chunk->args = std::move(newArgs);

		if (writes.empty())
			continue; // No writes in this chunk, just ref param removal

		// 2. Replace IndexExpression writes with VarExpression writes
		for (auto& stmt: chunk->body->body)
		{
			if (!stmt)
				continue;

			if (stmt->nodeType() == "AssignmentStatement")
			{
				auto& as = static_cast<awst::AssignmentStatement&>(*stmt);
				if (as.target && as.target->nodeType() == "IndexExpression")
				{
					auto const& idx = static_cast<awst::IndexExpression const&>(*as.target);
					if (idx.base && idx.base->nodeType() == "VarExpression" &&
						idx.index && idx.index->nodeType() == "IntegerConstant")
					{
						auto const& var = static_cast<awst::VarExpression const&>(*idx.base);
						if (refParamNames.count(var.name))
						{
							auto const& ic = static_cast<awst::IntegerConstant const&>(*idx.index);
							int index = std::stoi(ic.value);
							std::string localName = "_" + var.name + "_" + std::to_string(index);

							auto newTarget = std::make_shared<awst::VarExpression>();
							newTarget->sourceLocation = as.target->sourceLocation;
							newTarget->wtype = idx.wtype ? idx.wtype : awst::WType::biguintType();
							newTarget->name = localName;
							as.target = newTarget;
						}
					}
				}
			}
			else if (stmt->nodeType() == "ExpressionStatement")
			{
				auto& es = static_cast<awst::ExpressionStatement&>(*stmt);
				if (es.expr && es.expr->nodeType() == "AssignmentExpression")
				{
					auto& ae = static_cast<awst::AssignmentExpression&>(*es.expr);
					if (ae.target && ae.target->nodeType() == "IndexExpression")
					{
						auto const& idx = static_cast<awst::IndexExpression const&>(*ae.target);
						if (idx.base && idx.base->nodeType() == "VarExpression" &&
							idx.index && idx.index->nodeType() == "IntegerConstant")
						{
							auto const& var = static_cast<awst::VarExpression const&>(*idx.base);
							if (refParamNames.count(var.name))
							{
								auto const& ic = static_cast<awst::IntegerConstant const&>(*idx.index);
								int index = std::stoi(ic.value);
								std::string localName = "_" + var.name + "_" + std::to_string(index);

								auto newTarget = std::make_shared<awst::VarExpression>();
								newTarget->sourceLocation = ae.target->sourceLocation;
								newTarget->wtype = idx.wtype ? idx.wtype : awst::WType::biguintType();
								newTarget->name = localName;
								ae.target = newTarget;
							}
						}
					}
				}
			}
		}

		// 3. Extend return type with write values
		// Gather existing return items from the current return type
		std::vector<awst::WType const*> returnTypes;
		if (chunk->returnType && chunk->returnType != awst::WType::voidType())
		{
			if (chunk->returnType->kind() == awst::WTypeKind::WTuple)
			{
				auto const* tup = static_cast<awst::WTuple const*>(chunk->returnType);
				returnTypes = tup->types();
			}
			else
			{
				returnTypes.push_back(chunk->returnType);
			}
		}

		for (auto const& w: writes)
			returnTypes.push_back(w.wtype);

		if (returnTypes.size() == 1)
			chunk->returnType = returnTypes[0];
		else
			chunk->returnType = new awst::WTuple(returnTypes);

		// 4. Extend return statement at end of chunk body
		std::shared_ptr<awst::ReturnStatement> retStmt;
		if (!chunk->body->body.empty())
		{
			auto& lastStmt = chunk->body->body.back();
			if (lastStmt && lastStmt->nodeType() == "ReturnStatement")
				retStmt = std::static_pointer_cast<awst::ReturnStatement>(lastStmt);
		}

		// Build the list of new return items (eval vars)
		std::vector<std::shared_ptr<awst::Expression>> evalItems;
		for (auto const& w: writes)
		{
			auto var = std::make_shared<awst::VarExpression>();
			var->sourceLocation = chunk->sourceLocation;
			var->wtype = w.wtype;
			var->name = w.localVarName;
			evalItems.push_back(var);
		}

		if (retStmt)
		{
			// Existing return — extend it
			if (retStmt->value)
			{
				std::vector<std::shared_ptr<awst::Expression>> allItems;
				if (retStmt->value->nodeType() == "TupleExpression")
				{
					auto& tup = static_cast<awst::TupleExpression&>(*retStmt->value);
					allItems = tup.items;
				}
				else
				{
					allItems.push_back(retStmt->value);
				}
				for (auto& ei: evalItems)
					allItems.push_back(ei);

				auto newTuple = std::make_shared<awst::TupleExpression>();
				newTuple->sourceLocation = chunk->sourceLocation;
				newTuple->wtype = chunk->returnType;
				newTuple->items = std::move(allItems);
				retStmt->value = newTuple;
			}
			else
			{
				// Void return — replace with eval values
				if (evalItems.size() == 1)
				{
					retStmt->value = evalItems[0];
				}
				else
				{
					auto newTuple = std::make_shared<awst::TupleExpression>();
					newTuple->sourceLocation = chunk->sourceLocation;
					newTuple->wtype = chunk->returnType;
					newTuple->items = std::move(evalItems);
					retStmt->value = newTuple;
				}
			}
		}
		else
		{
			// No return statement — create one
			auto ret = std::make_shared<awst::ReturnStatement>();
			ret->sourceLocation = chunk->sourceLocation;
			if (evalItems.size() == 1)
			{
				ret->value = evalItems[0];
			}
			else
			{
				auto newTuple = std::make_shared<awst::TupleExpression>();
				newTuple->sourceLocation = chunk->sourceLocation;
				newTuple->wtype = chunk->returnType;
				newTuple->items = std::move(evalItems);
				ret->value = newTuple;
			}
			chunk->body->body.push_back(ret);
		}

		logger.info("    Chunk " + std::to_string(c) + ": " +
			std::to_string(writes.size()) + " writes converted to return values");
	}

	// ── Rebuild parent dispatch with writeback ───────────────────────────

	auto newBody = std::make_shared<awst::Block>();
	newBody->sourceLocation = _parent->sourceLocation;

	// Build ref-param-filtered arg list for parent call construction
	std::vector<awst::SubroutineArgument> parentNonRefArgs;
	for (auto const& arg: _parent->args)
	{
		if (!refParamNames.count(arg.name))
			parentNonRefArgs.push_back(arg);
	}

	// Find the existing live-var threading by examining the current parent body.
	// The parent body was already built by splitFunction — it contains chunk calls
	// with SingleEvaluation unpack or direct assignment patterns.
	// We rebuild it with the same structure but updated calls and added writeback.

	// Iterate through old parent body, identify chunk calls, rebuild
	auto const& oldBody = _parent->body->body;

	// Track which old-body statement indices correspond to chunk call dispatch groups.
	// Each chunk call in the old parent produces 1 statement (void/single return)
	// or N+1 statements (N unpack assignments from SingleEvaluation).
	// We need to rebuild each group with updated calls.

	size_t chunkIdx = 0;
	size_t si = 0;
	while (si < oldBody.size() && chunkIdx < _chunks.size())
	{
		auto const& stmt = oldBody[si];
		if (!stmt)
		{
			newBody->body.push_back(stmt);
			++si;
			continue;
		}

		// Check if this statement (or group) is a chunk call dispatch
		auto& chunk = _chunks[chunkIdx];
		auto const& writes = chunkWrites[chunkIdx].writes;
		std::string chunkId = chunk->id;

		// Detect chunk call pattern: look for SubroutineCallExpression with this chunk's ID
		bool isChunkCall = false;
		auto isCallToChunk = [&](awst::Expression const* _expr) -> bool
		{
			if (!_expr || _expr->nodeType() != "SubroutineCallExpression")
				return false;
			auto const& call = static_cast<awst::SubroutineCallExpression const&>(*_expr);
			if (auto const* sid = std::get_if<awst::SubroutineID>(&call.target))
				return sid->target == chunkId;
			return false;
		};

		if (stmt->nodeType() == "ExpressionStatement")
		{
			auto const& es = static_cast<awst::ExpressionStatement const&>(*stmt);
			isChunkCall = isCallToChunk(es.expr.get());
		}
		else if (stmt->nodeType() == "AssignmentStatement")
		{
			auto const& as = static_cast<awst::AssignmentStatement const&>(*stmt);
			isChunkCall = isCallToChunk(as.value.get());
			// Also check for SingleEvaluation unpack pattern: first assignment has
			// TupleItemExpression whose base is SingleEvaluation containing the call
			if (!isChunkCall && as.value && as.value->nodeType() == "TupleItemExpression")
			{
				auto const& ti = static_cast<awst::TupleItemExpression const&>(*as.value);
				if (ti.base && ti.base->nodeType() == "SingleEvaluation")
				{
					auto const& se = static_cast<awst::SingleEvaluation const&>(*ti.base);
					isChunkCall = isCallToChunk(se.source.get());
				}
			}
		}
		else if (stmt->nodeType() == "ReturnStatement")
		{
			auto const& rs = static_cast<awst::ReturnStatement const&>(*stmt);
			isChunkCall = isCallToChunk(rs.value.get());
		}

		if (!isChunkCall)
		{
			newBody->body.push_back(stmt);
			++si;
			continue;
		}

		// Found a chunk call dispatch. Build the new call with updated args.
		auto newCall = std::make_shared<awst::SubroutineCallExpression>();
		newCall->sourceLocation = _parent->sourceLocation;
		newCall->wtype = chunk->returnType;
		newCall->target = awst::SubroutineID{chunkId};

		// Pass non-ref params
		for (auto const& arg: parentNonRefArgs)
		{
			// Check if this arg exists in chunk's args (params + live vars)
			bool inChunk = false;
			for (auto const& ca: chunk->args)
			{
				if (ca.name == arg.name)
				{
					inChunk = true;
					break;
				}
			}
			if (inChunk)
			{
				auto var = std::make_shared<awst::VarExpression>();
				var->sourceLocation = _parent->sourceLocation;
				var->wtype = arg.wtype;
				var->name = arg.name;
				newCall->args.push_back(awst::CallArg{arg.name, var});
			}
		}

		// Pass live vars (chunk args beyond the parent's non-ref params)
		for (size_t ai = parentNonRefArgs.size(); ai < chunk->args.size(); ++ai)
		{
			auto const& ca = chunk->args[ai];
			auto var = std::make_shared<awst::VarExpression>();
			var->sourceLocation = _parent->sourceLocation;
			var->wtype = ca.wtype;
			var->name = ca.name;
			newCall->args.push_back(awst::CallArg{ca.name, var});
		}

		// Determine how many old-body statements this dispatch group spans.
		// For void/single return: 1 statement.
		// For multi-return (tuple unpack): count consecutive TupleItemExpression
		// assignments that share the same SingleEvaluation base.
		size_t groupEnd = si + 1;
		if (stmt->nodeType() == "AssignmentStatement")
		{
			auto const& as = static_cast<awst::AssignmentStatement const&>(*stmt);
			if (as.value && as.value->nodeType() == "TupleItemExpression")
			{
				// Multi-return unpack group — find how many consecutive assignments
				auto const& ti = static_cast<awst::TupleItemExpression const&>(*as.value);
				auto seBase = ti.base;
				while (groupEnd < oldBody.size())
				{
					auto const& nextStmt = oldBody[groupEnd];
					if (!nextStmt || nextStmt->nodeType() != "AssignmentStatement")
						break;
					auto const& nas = static_cast<awst::AssignmentStatement const&>(*nextStmt);
					if (!nas.value || nas.value->nodeType() != "TupleItemExpression")
						break;
					auto const& nti = static_cast<awst::TupleItemExpression const&>(*nas.value);
					if (nti.base.get() != seBase.get())
						break;
					++groupEnd;
				}
			}
		}

		// Count original return items (before our added eval values)
		size_t origReturnCount = 0;
		{
			// The old chunk's return type had the live vars
			// New chunk's return type has live vars + eval values
			// origReturnCount = total new return items - writes.size()
			if (chunk->returnType && chunk->returnType != awst::WType::voidType())
			{
				if (chunk->returnType->kind() == awst::WTypeKind::WTuple)
				{
					auto const* tup = static_cast<awst::WTuple const*>(chunk->returnType);
					origReturnCount = tup->types().size() - writes.size();
				}
				else
				{
					origReturnCount = writes.empty() ? 1 : 0;
				}
			}
		}

		bool isLastChunk = (chunkIdx == _chunks.size() - 1);

		if (writes.empty() && origReturnCount == 0)
		{
			// Void call, no writes — just call as expression statement
			auto exprStmt = std::make_shared<awst::ExpressionStatement>();
			exprStmt->sourceLocation = _parent->sourceLocation;
			exprStmt->expr = newCall;
			newBody->body.push_back(exprStmt);
		}
		else if (isLastChunk && _parent->returnType != awst::WType::voidType() && writes.empty())
		{
			// Last chunk returns parent's return value, no eval writes
			auto ret = std::make_shared<awst::ReturnStatement>();
			ret->sourceLocation = _parent->sourceLocation;
			ret->value = newCall;
			newBody->body.push_back(ret);
		}
		else
		{
			// Need to unpack: original live vars + eval values
			size_t totalItems = origReturnCount + writes.size();

			if (totalItems == 1 && writes.size() == 1)
			{
				// Single eval return
				auto assign = std::make_shared<awst::AssignmentStatement>();
				assign->sourceLocation = _parent->sourceLocation;
				auto target = std::make_shared<awst::VarExpression>();
				target->sourceLocation = _parent->sourceLocation;
				target->wtype = writes[0].wtype;
				target->name = writes[0].localVarName;
				assign->target = target;
				assign->value = newCall;
				newBody->body.push_back(assign);
			}
			else if (totalItems == 1 && origReturnCount == 1)
			{
				// Single live var return (no eval writes for this chunk)
				// Reproduce original single assignment
				auto assign = std::make_shared<awst::AssignmentStatement>();
				assign->sourceLocation = _parent->sourceLocation;
				// Get original target from the old statement
				if (stmt->nodeType() == "AssignmentStatement")
				{
					auto const& origAs = static_cast<awst::AssignmentStatement const&>(*stmt);
					assign->target = origAs.target;
				}
				assign->value = newCall;
				newBody->body.push_back(assign);
			}
			else
			{
				// Multi-item unpack via SingleEvaluation + TupleItemExpression
				auto se = std::make_shared<awst::SingleEvaluation>();
				se->sourceLocation = _parent->sourceLocation;
				se->wtype = newCall->wtype;
				se->source = newCall;
				se->id = m_nextSingleEvalId++;

				// Unpack original live vars (reproduce old assignments with updated index)
				// Walk old group statements to get original target var names
				size_t itemIdx = 0;
				for (size_t gi = si; gi < groupEnd; ++gi)
				{
					auto const& oldStmt = oldBody[gi];
					if (!oldStmt || oldStmt->nodeType() != "AssignmentStatement")
						continue;
					auto const& oldAs = static_cast<awst::AssignmentStatement const&>(*oldStmt);

					auto assign = std::make_shared<awst::AssignmentStatement>();
					assign->sourceLocation = _parent->sourceLocation;
					assign->target = oldAs.target; // preserve original target

					auto item = std::make_shared<awst::TupleItemExpression>();
					item->sourceLocation = _parent->sourceLocation;
					item->wtype = oldAs.target->wtype;
					item->base = se;
					item->index = static_cast<int>(itemIdx);
					assign->value = item;

					newBody->body.push_back(assign);
					++itemIdx;
				}

				// Unpack eval values
				for (size_t wi = 0; wi < writes.size(); ++wi)
				{
					auto assign = std::make_shared<awst::AssignmentStatement>();
					assign->sourceLocation = _parent->sourceLocation;

					auto target = std::make_shared<awst::VarExpression>();
					target->sourceLocation = _parent->sourceLocation;
					target->wtype = writes[wi].wtype;
					target->name = writes[wi].localVarName;
					assign->target = target;

					auto item = std::make_shared<awst::TupleItemExpression>();
					item->sourceLocation = _parent->sourceLocation;
					item->wtype = writes[wi].wtype;
					item->base = se;
					item->index = static_cast<int>(itemIdx);
					assign->value = item;

					newBody->body.push_back(assign);
					++itemIdx;
				}
			}
		}

		si = groupEnd;
		++chunkIdx;
	}

	// Copy any remaining statements (shouldn't normally happen)
	while (si < oldBody.size())
	{
		newBody->body.push_back(oldBody[si]);
		++si;
	}

	// ── Add writeback statements ─────────────────────────────────────────
	// After all chunk calls, write back: evals[N] = _evals_N

	for (auto const& w: allWrites)
	{
		auto assign = std::make_shared<awst::AssignmentStatement>();
		assign->sourceLocation = _parent->sourceLocation;

		// Target: evals[N]
		auto idxExpr = std::make_shared<awst::IndexExpression>();
		idxExpr->sourceLocation = _parent->sourceLocation;
		idxExpr->wtype = w.wtype;

		auto baseVar = std::make_shared<awst::VarExpression>();
		baseVar->sourceLocation = _parent->sourceLocation;
		// Find the ref param's type from parent args
		for (auto const& arg: _parent->args)
		{
			if (arg.name == w.paramName)
			{
				baseVar->wtype = arg.wtype;
				break;
			}
		}
		baseVar->name = w.paramName;
		idxExpr->base = baseVar;

		auto indexConst = std::make_shared<awst::IntegerConstant>();
		indexConst->sourceLocation = _parent->sourceLocation;
		indexConst->wtype = awst::WType::uint64Type();
		indexConst->value = std::to_string(w.index);
		idxExpr->index = indexConst;

		assign->target = idxExpr;

		// Value: _evals_N
		auto valVar = std::make_shared<awst::VarExpression>();
		valVar->sourceLocation = _parent->sourceLocation;
		valVar->wtype = w.wtype;
		valVar->name = w.localVarName;
		assign->value = valVar;

		newBody->body.push_back(assign);
	}

	_parent->body = newBody;

	logger.info("    Rebuilt parent dispatch with " + std::to_string(allWrites.size()) +
		" writeback assignments");
}

// ─── Rewritten callee expansion ──────────────────────────────────────────────

bool FunctionSplitter::expandRewrittenCallees(
	std::shared_ptr<awst::Subroutine> _func,
	std::set<std::string> const& _rewrittenFunctions,
	std::vector<std::shared_ptr<awst::RootNode>> const& _roots
)
{
	if (!_func->body || _rewrittenFunctions.empty())
		return false;

	// Build name→subroutine map for rewritten functions
	std::map<std::string, awst::Subroutine const*> rewrittenSubs;
	std::map<std::string, std::string> idToName;
	for (auto const& root: _roots)
	{
		if (auto const* sub = dynamic_cast<awst::Subroutine const*>(root.get()))
		{
			idToName[sub->id] = sub->name;
			if (_rewrittenFunctions.count(sub->name))
				rewrittenSubs[sub->name] = sub;
		}
	}

	bool expanded = false;
	std::vector<std::shared_ptr<awst::Statement>> newBody;

	for (auto const& stmt: _func->body->body)
	{
		if (!stmt)
		{
			newBody.push_back(stmt);
			continue;
		}

		// Check if this statement calls a rewritten function.
		// Handle multiple patterns:
		// 1. ExpressionStatement → SubroutineCallExpression
		// 2. ExpressionStatement → AssignmentExpression → value=SubroutineCallExpression
		// 3. AssignmentStatement → value=SubroutineCallExpression
		std::string calleeName;
		awst::SubroutineCallExpression const* callExpr = nullptr;
		std::shared_ptr<awst::Expression> assignTarget; // non-null if caller assigns the result

		auto tryExtractRewrittenCall = [&](awst::Expression const* _expr) -> bool
		{
			if (!_expr || _expr->nodeType() != "SubroutineCallExpression")
				return false;
			auto const& call = static_cast<awst::SubroutineCallExpression const&>(*_expr);
			if (auto const* sid = std::get_if<awst::SubroutineID>(&call.target))
			{
				auto nameIt = idToName.find(sid->target);
				if (nameIt != idToName.end() && _rewrittenFunctions.count(nameIt->second))
				{
					calleeName = nameIt->second;
					callExpr = &call;
					return true;
				}
			}
			return false;
		};

		if (stmt->nodeType() == "ExpressionStatement")
		{
			auto const& es = static_cast<awst::ExpressionStatement const&>(*stmt);
			if (!tryExtractRewrittenCall(es.expr.get()))
			{
				// Check for AssignmentExpression wrapping the call
				if (es.expr && es.expr->nodeType() == "AssignmentExpression")
				{
					auto const& ae = static_cast<awst::AssignmentExpression const&>(*es.expr);
					if (tryExtractRewrittenCall(ae.value.get()))
						assignTarget = ae.target;
				}
			}
		}
		else if (stmt->nodeType() == "AssignmentStatement")
		{
			auto const& as = static_cast<awst::AssignmentStatement const&>(*stmt);
			if (tryExtractRewrittenCall(as.value.get()))
				assignTarget = as.target;
		}

		if (calleeName.empty())
		{
			// Not a call to a rewritten function — keep as-is
			newBody.push_back(stmt);
			continue;
		}

		// Found a call to a rewritten function. Inline its body.
		auto it = rewrittenSubs.find(calleeName);
		if (it == rewrittenSubs.end() || !it->second->body)
		{
			newBody.push_back(stmt);
			continue;
		}

		auto const& rewrittenBody = it->second->body->body;

		// The rewritten function's body is a sequence of chunk calls
		// with live var threading. Copy these statements directly,
		// converting the final ReturnStatement appropriately:
		// - If the caller assigned the result (assignTarget != null),
		//   convert ReturnStatement to AssignmentStatement
		// - Otherwise convert to ExpressionStatement
		for (auto const& inlinedStmt: rewrittenBody)
		{
			if (inlinedStmt && inlinedStmt->nodeType() == "ReturnStatement")
			{
				auto const& rs = static_cast<awst::ReturnStatement const&>(*inlinedStmt);
				if (rs.value)
				{
					if (assignTarget)
					{
						// Caller was assigning: target = returnValue
						auto assign = std::make_shared<awst::AssignmentStatement>();
						assign->sourceLocation = rs.sourceLocation;
						assign->target = assignTarget;
						assign->value = rs.value;
						newBody.push_back(assign);
					}
					else
					{
						auto exprStmt = std::make_shared<awst::ExpressionStatement>();
						exprStmt->sourceLocation = rs.sourceLocation;
						exprStmt->expr = rs.value;
						newBody.push_back(exprStmt);
					}
				}
				// else: void return, skip entirely
			}
			else
			{
				newBody.push_back(inlinedStmt);
			}
		}

		expanded = true;
	}

	if (expanded)
	{
		_func->body->body = std::move(newBody);
	}

	return expanded;
}

// ─── Callee collection for dep-aware cost estimation ─────────────────────────

void FunctionSplitter::scanExprForCallees(
	awst::Expression const& _expr,
	std::set<std::string>& _calleeIds
)
{
	std::string type = _expr.nodeType();

	if (type == "SubroutineCallExpression")
	{
		auto const& call = static_cast<awst::SubroutineCallExpression const&>(_expr);
		if (auto const* sid = std::get_if<awst::SubroutineID>(&call.target))
			_calleeIds.insert(sid->target);

		for (auto const& arg: call.args)
			if (arg.value)
				scanExprForCallees(*arg.value, _calleeIds);
		return;
	}

	// Recurse into common expression types
	if (type == "AssignmentExpression")
	{
		auto const& a = static_cast<awst::AssignmentExpression const&>(_expr);
		if (a.target) scanExprForCallees(*a.target, _calleeIds);
		if (a.value) scanExprForCallees(*a.value, _calleeIds);
	}
	else if (type == "BigUIntBinaryOperation")
	{
		auto const& op = static_cast<awst::BigUIntBinaryOperation const&>(_expr);
		if (op.left) scanExprForCallees(*op.left, _calleeIds);
		if (op.right) scanExprForCallees(*op.right, _calleeIds);
	}
	else if (type == "UInt64BinaryOperation")
	{
		auto const& op = static_cast<awst::UInt64BinaryOperation const&>(_expr);
		if (op.left) scanExprForCallees(*op.left, _calleeIds);
		if (op.right) scanExprForCallees(*op.right, _calleeIds);
	}
	else if (type == "BytesBinaryOperation")
	{
		auto const& op = static_cast<awst::BytesBinaryOperation const&>(_expr);
		if (op.left) scanExprForCallees(*op.left, _calleeIds);
		if (op.right) scanExprForCallees(*op.right, _calleeIds);
	}
	else if (type == "IntrinsicCall")
	{
		auto const& ic = static_cast<awst::IntrinsicCall const&>(_expr);
		for (auto const& arg: ic.stackArgs)
			if (arg) scanExprForCallees(*arg, _calleeIds);
	}
	else if (type == "PuyaLibCall")
	{
		auto const& plc = static_cast<awst::PuyaLibCall const&>(_expr);
		for (auto const& arg: plc.args)
			if (arg.value) scanExprForCallees(*arg.value, _calleeIds);
	}
	else if (type == "FieldExpression")
	{
		auto const& f = static_cast<awst::FieldExpression const&>(_expr);
		if (f.base) scanExprForCallees(*f.base, _calleeIds);
	}
	else if (type == "IndexExpression")
	{
		auto const& idx = static_cast<awst::IndexExpression const&>(_expr);
		if (idx.base) scanExprForCallees(*idx.base, _calleeIds);
		if (idx.index) scanExprForCallees(*idx.index, _calleeIds);
	}
	else if (type == "TupleExpression")
	{
		auto const& t = static_cast<awst::TupleExpression const&>(_expr);
		for (auto const& item: t.items)
			if (item) scanExprForCallees(*item, _calleeIds);
	}
	else if (type == "TupleItemExpression")
	{
		auto const& ti = static_cast<awst::TupleItemExpression const&>(_expr);
		if (ti.base) scanExprForCallees(*ti.base, _calleeIds);
	}
	else if (type == "ARC4Encode")
	{
		auto const& e = static_cast<awst::ARC4Encode const&>(_expr);
		if (e.value) scanExprForCallees(*e.value, _calleeIds);
	}
	else if (type == "ARC4Decode")
	{
		auto const& d = static_cast<awst::ARC4Decode const&>(_expr);
		if (d.value) scanExprForCallees(*d.value, _calleeIds);
	}
	else if (type == "ReinterpretCast")
	{
		auto const& rc = static_cast<awst::ReinterpretCast const&>(_expr);
		if (rc.expr) scanExprForCallees(*rc.expr, _calleeIds);
	}
	else if (type == "Copy")
	{
		auto const& c = static_cast<awst::Copy const&>(_expr);
		if (c.value) scanExprForCallees(*c.value, _calleeIds);
	}
	else if (type == "ConditionalExpression")
	{
		auto const& c = static_cast<awst::ConditionalExpression const&>(_expr);
		if (c.condition) scanExprForCallees(*c.condition, _calleeIds);
		if (c.trueExpr) scanExprForCallees(*c.trueExpr, _calleeIds);
		if (c.falseExpr) scanExprForCallees(*c.falseExpr, _calleeIds);
	}
	else if (type == "NewArray")
	{
		auto const& na = static_cast<awst::NewArray const&>(_expr);
		for (auto const& v: na.values)
			if (v) scanExprForCallees(*v, _calleeIds);
	}
	else if (type == "SingleEvaluation")
	{
		auto const& se = static_cast<awst::SingleEvaluation const&>(_expr);
		if (se.source) scanExprForCallees(*se.source, _calleeIds);
	}
	else if (type == "CheckedMaybe")
	{
		auto const& cm = static_cast<awst::CheckedMaybe const&>(_expr);
		if (cm.expr) scanExprForCallees(*cm.expr, _calleeIds);
	}
	else if (type == "Not")
	{
		auto const& n = static_cast<awst::Not const&>(_expr);
		if (n.expr) scanExprForCallees(*n.expr, _calleeIds);
	}
	else if (type == "AssertExpression")
	{
		auto const& a = static_cast<awst::AssertExpression const&>(_expr);
		if (a.condition) scanExprForCallees(*a.condition, _calleeIds);
	}
	else if (type == "NumericComparisonExpression")
	{
		auto const& cmp = static_cast<awst::NumericComparisonExpression const&>(_expr);
		if (cmp.lhs) scanExprForCallees(*cmp.lhs, _calleeIds);
		if (cmp.rhs) scanExprForCallees(*cmp.rhs, _calleeIds);
	}
	else if (type == "BytesComparisonExpression")
	{
		auto const& cmp = static_cast<awst::BytesComparisonExpression const&>(_expr);
		if (cmp.lhs) scanExprForCallees(*cmp.lhs, _calleeIds);
		if (cmp.rhs) scanExprForCallees(*cmp.rhs, _calleeIds);
	}
	else if (type == "BooleanBinaryOperation")
	{
		auto const& op = static_cast<awst::BooleanBinaryOperation const&>(_expr);
		if (op.left) scanExprForCallees(*op.left, _calleeIds);
		if (op.right) scanExprForCallees(*op.right, _calleeIds);
	}
}

void FunctionSplitter::scanStmtForCallees(
	awst::Statement const& _stmt,
	std::set<std::string>& _calleeIds
)
{
	std::string type = _stmt.nodeType();

	if (type == "Block")
	{
		auto const& block = static_cast<awst::Block const&>(_stmt);
		for (auto const& s: block.body)
			if (s) scanStmtForCallees(*s, _calleeIds);
	}
	else if (type == "ExpressionStatement")
	{
		auto const& es = static_cast<awst::ExpressionStatement const&>(_stmt);
		if (es.expr) scanExprForCallees(*es.expr, _calleeIds);
	}
	else if (type == "ReturnStatement")
	{
		auto const& rs = static_cast<awst::ReturnStatement const&>(_stmt);
		if (rs.value) scanExprForCallees(*rs.value, _calleeIds);
	}
	else if (type == "AssignmentStatement")
	{
		auto const& as = static_cast<awst::AssignmentStatement const&>(_stmt);
		if (as.target) scanExprForCallees(*as.target, _calleeIds);
		if (as.value) scanExprForCallees(*as.value, _calleeIds);
	}
	else if (type == "IfElse")
	{
		auto const& ie = static_cast<awst::IfElse const&>(_stmt);
		if (ie.condition) scanExprForCallees(*ie.condition, _calleeIds);
		if (ie.ifBranch) scanStmtForCallees(*ie.ifBranch, _calleeIds);
		if (ie.elseBranch) scanStmtForCallees(*ie.elseBranch, _calleeIds);
	}
	else if (type == "WhileLoop")
	{
		auto const& wl = static_cast<awst::WhileLoop const&>(_stmt);
		if (wl.condition) scanExprForCallees(*wl.condition, _calleeIds);
		if (wl.loopBody) scanStmtForCallees(*wl.loopBody, _calleeIds);
	}
	else if (type == "Switch")
	{
		auto const& sw = static_cast<awst::Switch const&>(_stmt);
		if (sw.value) scanExprForCallees(*sw.value, _calleeIds);
		for (auto const& [caseExpr, caseBlock]: sw.cases)
		{
			if (caseExpr) scanExprForCallees(*caseExpr, _calleeIds);
			if (caseBlock) scanStmtForCallees(*caseBlock, _calleeIds);
		}
		if (sw.defaultCase) scanStmtForCallees(*sw.defaultCase, _calleeIds);
	}
	else if (type == "ForInLoop")
	{
		auto const& fil = static_cast<awst::ForInLoop const&>(_stmt);
		if (fil.sequence) scanExprForCallees(*fil.sequence, _calleeIds);
		if (fil.items) scanExprForCallees(*fil.items, _calleeIds);
		if (fil.loopBody) scanStmtForCallees(*fil.loopBody, _calleeIds);
	}
	else if (type == "UInt64AugmentedAssignment")
	{
		auto const& ua = static_cast<awst::UInt64AugmentedAssignment const&>(_stmt);
		if (ua.target) scanExprForCallees(*ua.target, _calleeIds);
		if (ua.value) scanExprForCallees(*ua.value, _calleeIds);
	}
	else if (type == "BigUIntAugmentedAssignment")
	{
		auto const& ba = static_cast<awst::BigUIntAugmentedAssignment const&>(_stmt);
		if (ba.target) scanExprForCallees(*ba.target, _calleeIds);
		if (ba.value) scanExprForCallees(*ba.value, _calleeIds);
	}
}

std::shared_ptr<awst::Expression> FunctionSplitter::buildDefault(
	awst::WType const* _type,
	awst::SourceLocation const& _loc
)
{
	if (_type == awst::WType::boolType())
	{
		auto val = std::make_shared<awst::BoolConstant>();
		val->value = false;
		val->wtype = awst::WType::boolType();
		val->sourceLocation = _loc;
		return val;
	}
	if (_type == awst::WType::uint64Type())
	{
		auto val = std::make_shared<awst::IntegerConstant>();
		val->value = "0";
		val->wtype = awst::WType::uint64Type();
		val->sourceLocation = _loc;
		return val;
	}
	if (_type == awst::WType::biguintType())
	{
		auto val = std::make_shared<awst::IntegerConstant>();
		val->value = "0";
		val->wtype = awst::WType::biguintType();
		val->sourceLocation = _loc;
		return val;
	}
	if (_type == awst::WType::bytesType() || _type->kind() == awst::WTypeKind::Bytes)
	{
		auto val = std::make_shared<awst::BytesConstant>();
		val->wtype = _type;
		val->sourceLocation = _loc;
		val->encoding = awst::BytesEncoding::Base16;
		if (auto const* bytesType = dynamic_cast<awst::BytesWType const*>(_type))
			if (bytesType->length().has_value())
				val->value.resize(*bytesType->length(), 0);
		return val;
	}
	if (_type == awst::WType::accountType())
	{
		auto val = std::make_shared<awst::BytesConstant>();
		val->wtype = awst::WType::accountType();
		val->sourceLocation = _loc;
		val->encoding = awst::BytesEncoding::Base16;
		val->value.resize(32, 0);
		return val;
	}
	if (_type->kind() == awst::WTypeKind::ARC4Struct)
	{
		auto const* structType = dynamic_cast<awst::ARC4Struct const*>(_type);
		if (structType)
		{
			// Compute total byte size of the struct (all fields concatenated)
			std::function<size_t(awst::WType const*)> arc4Size = [&](awst::WType const* t) -> size_t {
				if (auto const* uintN = dynamic_cast<awst::ARC4UIntN const*>(t))
					return uintN->n() / 8;
				if (auto const* fixedBytes = dynamic_cast<awst::BytesWType const*>(t))
					if (fixedBytes->length().has_value())
						return *fixedBytes->length();
				if (t == awst::WType::boolType())
					return 1;
				if (auto const* innerStruct = dynamic_cast<awst::ARC4Struct const*>(t))
				{
					size_t total = 0;
					for (auto const& [fname, ftype]: innerStruct->fields())
						total += arc4Size(ftype);
					return total;
				}
				return 32; // fallback
			};
			size_t totalSize = 0;
			for (auto const& [fname, ftype]: structType->fields())
				totalSize += arc4Size(ftype);
			auto val = std::make_shared<awst::BytesConstant>();
			val->wtype = _type;
			val->sourceLocation = _loc;
			val->encoding = awst::BytesEncoding::Base16;
			val->value.resize(totalSize, 0);
			return val;
		}
	}
	if (_type->kind() == awst::WTypeKind::ARC4Tuple ||
		_type->kind() == awst::WTypeKind::WTuple)
	{
		auto const* tupleType = dynamic_cast<awst::WTuple const*>(_type);
		if (tupleType)
		{
			auto tuple = std::make_shared<awst::TupleExpression>();
			tuple->sourceLocation = _loc;
			tuple->wtype = _type;
			for (auto const* elemType: tupleType->types())
				tuple->items.push_back(buildDefault(elemType, _loc));
			return tuple;
		}
	}
	// Fallback: zero biguint
	auto val = std::make_shared<awst::IntegerConstant>();
	val->value = "0";
	val->wtype = awst::WType::biguintType();
	val->sourceLocation = _loc;
	return val;
}

void FunctionSplitter::rewriteInnerReturns(
	awst::Block& _body,
	awst::WType const* _chunkReturnType,
	std::vector<VarInfo> const& _liveOut,
	awst::SourceLocation const& _loc
)
{
	if (!_chunkReturnType || _chunkReturnType == awst::WType::voidType())
		return;
	if (_liveOut.empty())
		return;

	// Build the replacement return: a tuple of all live-out variables.
	// This is the same as the end-of-chunk return — any inner return in a
	// non-last chunk should return the current values of all live variables,
	// not the original function's return value.
	auto buildLiveReturnValue = [&]() -> std::shared_ptr<awst::Expression> {
		if (_liveOut.size() == 1)
		{
			auto var = std::make_shared<awst::VarExpression>();
			var->sourceLocation = _loc;
			var->wtype = _liveOut[0].wtype;
			var->name = _liveOut[0].name;
			return var;
		}
		auto tuple = std::make_shared<awst::TupleExpression>();
		tuple->sourceLocation = _loc;
		tuple->wtype = _chunkReturnType;
		for (auto const& lv: _liveOut)
		{
			auto var = std::make_shared<awst::VarExpression>();
			var->sourceLocation = _loc;
			var->wtype = lv.wtype;
			var->name = lv.name;
			tuple->items.push_back(var);
		}
		return tuple;
	};

	// Recursive lambda to walk all statements and rewrite return statements
	std::function<void(awst::Block&)> walkBlock = [&](awst::Block& block) {
		for (auto& stmt: block.body)
		{
			if (!stmt) continue;
			std::string type = stmt->nodeType();
			if (type == "ReturnStatement")
			{
				auto& rs = static_cast<awst::ReturnStatement&>(*stmt);
				// Replace the return value with the live variable tuple
				rs.value = buildLiveReturnValue();
			}
			else if (type == "IfElse")
			{
				auto& ie = static_cast<awst::IfElse&>(*stmt);
				if (ie.ifBranch)
					walkBlock(static_cast<awst::Block&>(*ie.ifBranch));
				if (ie.elseBranch)
					walkBlock(static_cast<awst::Block&>(*ie.elseBranch));
			}
			else if (type == "WhileLoop")
			{
				auto& wl = static_cast<awst::WhileLoop&>(*stmt);
				if (wl.loopBody)
					walkBlock(static_cast<awst::Block&>(*wl.loopBody));
			}
			else if (type == "Block")
			{
				walkBlock(static_cast<awst::Block&>(*stmt));
			}
			else if (type == "Switch")
			{
				auto& sw = static_cast<awst::Switch&>(*stmt);
				for (auto& [caseExpr, caseBlock]: sw.cases)
					if (caseBlock)
						walkBlock(static_cast<awst::Block&>(*caseBlock));
				if (sw.defaultCase)
					walkBlock(static_cast<awst::Block&>(*sw.defaultCase));
			}
			else if (type == "ForInLoop")
			{
				auto& fil = static_cast<awst::ForInLoop&>(*stmt);
				if (fil.loopBody)
					walkBlock(static_cast<awst::Block&>(*fil.loopBody));
			}
		}
	};

	walkBlock(_body);
}

void FunctionSplitter::ensureLiveOutVarsInitialized(
	awst::Block& _body,
	std::vector<VarInfo> const& _liveOut,
	awst::SourceLocation const& _loc
)
{
	if (_liveOut.empty())
		return;

	// Check if any statements in the body (before an inner return) might
	// reference a live-out variable that hasn't been assigned yet.
	// Collect the set of variables assigned by top-level statements.
	// If we hit a conditional with a ReturnStatement before a variable is assigned,
	// that variable needs a default initialization.

	std::set<std::string> liveOutNames;
	for (auto const& lv: _liveOut)
		liveOutNames.insert(lv.name);

	// Collect all assigned vars in order, looking for inner returns
	std::set<std::string> assignedVars;
	bool hasInnerReturn = false;

	// Check if a block (recursively) contains a ReturnStatement
	std::function<bool(awst::Block const&)> containsReturn = [&](awst::Block const& block) -> bool {
		for (auto const& stmt: block.body)
		{
			if (!stmt) continue;
			if (stmt->nodeType() == "ReturnStatement") return true;
			if (stmt->nodeType() == "IfElse")
			{
				auto const& ie = static_cast<awst::IfElse const&>(*stmt);
				if (ie.ifBranch && containsReturn(static_cast<awst::Block const&>(*ie.ifBranch)))
					return true;
				if (ie.elseBranch && containsReturn(static_cast<awst::Block const&>(*ie.elseBranch)))
					return true;
			}
		}
		return false;
	};

	for (auto const& stmt: _body.body)
	{
		if (!stmt) continue;
		std::string type = stmt->nodeType();
		// Track assignments
		if (type == "AssignmentStatement")
		{
			auto const& as = static_cast<awst::AssignmentStatement const&>(*stmt);
			if (as.target && as.target->nodeType() == "VarExpression")
			{
				auto const& ve = static_cast<awst::VarExpression const&>(*as.target);
				assignedVars.insert(ve.name);
			}
		}
		// Check for inner returns in IfElse
		if (type == "IfElse")
		{
			auto const& ie = static_cast<awst::IfElse const&>(*stmt);
			if ((ie.ifBranch && containsReturn(static_cast<awst::Block const&>(*ie.ifBranch))) ||
				(ie.elseBranch && containsReturn(static_cast<awst::Block const&>(*ie.elseBranch))))
			{
				hasInnerReturn = true;
				break;
			}
		}
	}

	if (!hasInnerReturn)
		return;

	// Find live-out vars that haven't been assigned before the inner return
	std::vector<VarInfo const*> uninitVars;
	for (auto const& lv: _liveOut)
	{
		if (!assignedVars.count(lv.name))
			uninitVars.push_back(&lv);
	}

	if (uninitVars.empty())
		return;

	auto& logger = Logger::instance();
	// Insert default initialization assignments at the beginning of the body
	std::vector<std::shared_ptr<awst::Statement>> inits;
	for (auto const* lv: uninitVars)
	{
		logger.info("    Inserting default init for live-out var '" + lv->name +
			"' (type: " + lv->wtype->name() + ") before inner return");

		auto assign = std::make_shared<awst::AssignmentStatement>();
		assign->sourceLocation = _loc;

		auto target = std::make_shared<awst::VarExpression>();
		target->sourceLocation = _loc;
		target->wtype = lv->wtype;
		target->name = lv->name;
		assign->target = target;

		assign->value = buildDefault(lv->wtype, _loc);

		inits.push_back(assign);
	}

	// Prepend inits before the existing body
	_body.body.insert(_body.body.begin(), inits.begin(), inits.end());
}

} // namespace puyasol::splitter
