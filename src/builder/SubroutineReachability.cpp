#include "builder/SubroutineReachability.h"

#include <queue>
#include <set>
#include <unordered_map>

namespace puyasol::builder
{
namespace
{

/// Collect SubroutineID targets reachable from an expression subtree.
void collectRefs(awst::Expression const& _expr, std::set<std::string>& _refs)
{
	if (auto const* call = dynamic_cast<awst::SubroutineCallExpression const*>(&_expr))
	{
		if (auto const* sid = std::get_if<awst::SubroutineID>(&call->target))
			_refs.insert(sid->target);
		for (auto const& arg: call->args)
			if (arg.value) collectRefs(*arg.value, _refs);
	}
	if (auto const* e = dynamic_cast<awst::UInt64BinaryOperation const*>(&_expr))
	{
		if (e->left) collectRefs(*e->left, _refs);
		if (e->right) collectRefs(*e->right, _refs);
	}
	if (auto const* e = dynamic_cast<awst::BigUIntBinaryOperation const*>(&_expr))
	{
		if (e->left) collectRefs(*e->left, _refs);
		if (e->right) collectRefs(*e->right, _refs);
	}
	if (auto const* e = dynamic_cast<awst::NumericComparisonExpression const*>(&_expr))
	{
		if (e->lhs) collectRefs(*e->lhs, _refs);
		if (e->rhs) collectRefs(*e->rhs, _refs);
	}
	if (auto const* e = dynamic_cast<awst::BytesComparisonExpression const*>(&_expr))
	{
		if (e->lhs) collectRefs(*e->lhs, _refs);
		if (e->rhs) collectRefs(*e->rhs, _refs);
	}
	if (auto const* e = dynamic_cast<awst::BooleanBinaryOperation const*>(&_expr))
	{
		if (e->left) collectRefs(*e->left, _refs);
		if (e->right) collectRefs(*e->right, _refs);
	}
	if (auto const* e = dynamic_cast<awst::Not const*>(&_expr))
	{
		if (e->expr) collectRefs(*e->expr, _refs);
	}
	if (auto const* e = dynamic_cast<awst::AssertExpression const*>(&_expr))
	{
		if (e->condition) collectRefs(*e->condition, _refs);
	}
	if (auto const* e = dynamic_cast<awst::AssignmentExpression const*>(&_expr))
	{
		if (e->target) collectRefs(*e->target, _refs);
		if (e->value) collectRefs(*e->value, _refs);
	}
	if (auto const* e = dynamic_cast<awst::ReinterpretCast const*>(&_expr))
	{
		if (e->expr) collectRefs(*e->expr, _refs);
	}
	if (auto const* e = dynamic_cast<awst::ConditionalExpression const*>(&_expr))
	{
		if (e->condition) collectRefs(*e->condition, _refs);
		if (e->trueExpr) collectRefs(*e->trueExpr, _refs);
		if (e->falseExpr) collectRefs(*e->falseExpr, _refs);
	}
	if (auto const* e = dynamic_cast<awst::FieldExpression const*>(&_expr))
	{
		if (e->base) collectRefs(*e->base, _refs);
	}
	if (auto const* e = dynamic_cast<awst::IndexExpression const*>(&_expr))
	{
		if (e->base) collectRefs(*e->base, _refs);
		if (e->index) collectRefs(*e->index, _refs);
	}
	if (auto const* e = dynamic_cast<awst::IntrinsicCall const*>(&_expr))
	{
		for (auto const& arg: e->stackArgs)
			if (arg) collectRefs(*arg, _refs);
	}
	if (auto const* e = dynamic_cast<awst::TupleExpression const*>(&_expr))
	{
		for (auto const& item: e->items)
			if (item) collectRefs(*item, _refs);
	}
	if (auto const* e = dynamic_cast<awst::NamedTupleExpression const*>(&_expr))
	{
		for (auto const& [_, v]: e->values)
			if (v) collectRefs(*v, _refs);
	}
	if (auto const* e = dynamic_cast<awst::NewStruct const*>(&_expr))
	{
		for (auto const& [_, v]: e->values)
			if (v) collectRefs(*v, _refs);
	}
	if (auto const* e = dynamic_cast<awst::NewArray const*>(&_expr))
	{
		for (auto const& v: e->values)
			if (v) collectRefs(*v, _refs);
	}
	if (auto const* e = dynamic_cast<awst::ArrayLength const*>(&_expr))
	{
		if (e->array) collectRefs(*e->array, _refs);
	}
	if (auto const* e = dynamic_cast<awst::ArrayPop const*>(&_expr))
	{
		if (e->base) collectRefs(*e->base, _refs);
	}
	if (auto const* e = dynamic_cast<awst::BoxValueExpression const*>(&_expr))
	{
		if (e->key) collectRefs(*e->key, _refs);
	}
	if (auto const* e = dynamic_cast<awst::BytesBinaryOperation const*>(&_expr))
	{
		if (e->left) collectRefs(*e->left, _refs);
		if (e->right) collectRefs(*e->right, _refs);
	}
	if (auto const* e = dynamic_cast<awst::TupleItemExpression const*>(&_expr))
	{
		if (e->base) collectRefs(*e->base, _refs);
	}
	if (auto const* e = dynamic_cast<awst::ARC4Encode const*>(&_expr))
	{
		if (e->value) collectRefs(*e->value, _refs);
	}
	if (auto const* e = dynamic_cast<awst::ARC4Decode const*>(&_expr))
	{
		if (e->value) collectRefs(*e->value, _refs);
	}
	if (auto const* e = dynamic_cast<awst::Copy const*>(&_expr))
	{
		if (e->value) collectRefs(*e->value, _refs);
	}
	if (auto const* e = dynamic_cast<awst::SingleEvaluation const*>(&_expr))
	{
		if (e->source) collectRefs(*e->source, _refs);
	}
	if (auto const* e = dynamic_cast<awst::CheckedMaybe const*>(&_expr))
	{
		if (e->expr) collectRefs(*e->expr, _refs);
	}
	if (auto const* e = dynamic_cast<awst::Emit const*>(&_expr))
	{
		if (e->value) collectRefs(*e->value, _refs);
	}
	if (auto const* e = dynamic_cast<awst::ArrayConcat const*>(&_expr))
	{
		if (e->left) collectRefs(*e->left, _refs);
		if (e->right) collectRefs(*e->right, _refs);
	}
	if (auto const* e = dynamic_cast<awst::ArrayExtend const*>(&_expr))
	{
		if (e->base) collectRefs(*e->base, _refs);
		if (e->other) collectRefs(*e->other, _refs);
	}
	if (auto const* e = dynamic_cast<awst::StateGet const*>(&_expr))
	{
		if (e->field) collectRefs(*e->field, _refs);
		if (e->defaultValue) collectRefs(*e->defaultValue, _refs);
	}
	if (auto const* e = dynamic_cast<awst::StateExists const*>(&_expr))
	{
		if (e->field) collectRefs(*e->field, _refs);
	}
	if (auto const* e = dynamic_cast<awst::StateDelete const*>(&_expr))
	{
		if (e->field) collectRefs(*e->field, _refs);
	}
	if (auto const* e = dynamic_cast<awst::StateGetEx const*>(&_expr))
	{
		if (e->field) collectRefs(*e->field, _refs);
	}
	if (auto const* e = dynamic_cast<awst::AppStateExpression const*>(&_expr))
	{
		if (e->key) collectRefs(*e->key, _refs);
	}
	if (auto const* e = dynamic_cast<awst::AppAccountStateExpression const*>(&_expr))
	{
		if (e->key) collectRefs(*e->key, _refs);
		if (e->account) collectRefs(*e->account, _refs);
	}
	if (auto const* e = dynamic_cast<awst::BoxPrefixedKeyExpression const*>(&_expr))
	{
		if (e->prefix) collectRefs(*e->prefix, _refs);
		if (e->key) collectRefs(*e->key, _refs);
	}
	if (auto const* e = dynamic_cast<awst::CreateInnerTransaction const*>(&_expr))
	{
		for (auto const& [_, v]: e->fields)
			if (v) collectRefs(*v, _refs);
	}
	if (auto const* e = dynamic_cast<awst::SubmitInnerTransaction const*>(&_expr))
	{
		for (auto const& itxn: e->itxns)
			if (itxn) collectRefs(*itxn, _refs);
	}
	if (auto const* e = dynamic_cast<awst::InnerTransactionField const*>(&_expr))
	{
		if (e->itxn) collectRefs(*e->itxn, _refs);
		if (e->arrayIndex) collectRefs(*e->arrayIndex, _refs);
	}
	if (auto const* e = dynamic_cast<awst::CommaExpression const*>(&_expr))
	{
		for (auto const& ex: e->expressions)
			if (ex) collectRefs(*ex, _refs);
	}
}

/// Collect SubroutineID targets reachable from a statement subtree.
void collectStmtRefs(awst::Statement const& _stmt, std::set<std::string>& _refs)
{
	if (auto const* block = dynamic_cast<awst::Block const*>(&_stmt))
	{
		for (auto const& s: block->body)
			if (s) collectStmtRefs(*s, _refs);
	}
	if (auto const* es = dynamic_cast<awst::ExpressionStatement const*>(&_stmt))
	{
		if (es->expr) collectRefs(*es->expr, _refs);
	}
	if (auto const* as = dynamic_cast<awst::AssignmentStatement const*>(&_stmt))
	{
		if (as->target) collectRefs(*as->target, _refs);
		if (as->value) collectRefs(*as->value, _refs);
	}
	if (auto const* rs = dynamic_cast<awst::ReturnStatement const*>(&_stmt))
	{
		if (rs->value) collectRefs(*rs->value, _refs);
	}
	if (auto const* ie = dynamic_cast<awst::IfElse const*>(&_stmt))
	{
		if (ie->condition) collectRefs(*ie->condition, _refs);
		if (ie->ifBranch) collectStmtRefs(*ie->ifBranch, _refs);
		if (ie->elseBranch) collectStmtRefs(*ie->elseBranch, _refs);
	}
	if (auto const* wl = dynamic_cast<awst::WhileLoop const*>(&_stmt))
	{
		if (wl->condition) collectRefs(*wl->condition, _refs);
		if (wl->loopBody) collectStmtRefs(*wl->loopBody, _refs);
	}
	if (auto const* sw = dynamic_cast<awst::Switch const*>(&_stmt))
	{
		if (sw->value) collectRefs(*sw->value, _refs);
		for (auto const& [caseExpr, caseBlock]: sw->cases)
		{
			if (caseExpr) collectRefs(*caseExpr, _refs);
			if (caseBlock) collectStmtRefs(*caseBlock, _refs);
		}
		if (sw->defaultCase) collectStmtRefs(*sw->defaultCase, _refs);
	}
	if (auto const* fl = dynamic_cast<awst::ForInLoop const*>(&_stmt))
	{
		if (fl->sequence) collectRefs(*fl->sequence, _refs);
		if (fl->items) collectRefs(*fl->items, _refs);
		if (fl->loopBody) collectStmtRefs(*fl->loopBody, _refs);
	}
	if (auto const* ua = dynamic_cast<awst::UInt64AugmentedAssignment const*>(&_stmt))
	{
		if (ua->target) collectRefs(*ua->target, _refs);
		if (ua->value) collectRefs(*ua->value, _refs);
	}
	if (auto const* ba = dynamic_cast<awst::BigUIntAugmentedAssignment const*>(&_stmt))
	{
		if (ba->target) collectRefs(*ba->target, _refs);
		if (ba->value) collectRefs(*ba->value, _refs);
	}
}

void collectMethodRefs(awst::ContractMethod const& _method, std::set<std::string>& _refs)
{
	if (_method.body)
		collectStmtRefs(*_method.body, _refs);
}

} // anonymous namespace

std::vector<std::shared_ptr<awst::RootNode>> filterToReachableSubroutines(
	std::vector<std::shared_ptr<awst::RootNode>> _roots)
{
	std::set<std::string> reachable;
	std::queue<std::string> worklist;

	// Seed with contract method references.
	for (auto const& root: _roots)
	{
		auto const* contract = dynamic_cast<awst::Contract const*>(root.get());
		if (!contract)
			continue;
		std::set<std::string> refs;
		collectMethodRefs(contract->approvalProgram, refs);
		collectMethodRefs(contract->clearProgram, refs);
		for (auto const& method: contract->methods)
			collectMethodRefs(method, refs);
		for (auto const& id: refs)
		{
			reachable.insert(id);
			worklist.push(id);
		}
	}

	// Build ID → Subroutine map for the worklist phase.
	std::unordered_map<std::string, awst::Subroutine const*> subMap;
	for (auto const& root: _roots)
		if (auto const* sub = dynamic_cast<awst::Subroutine const*>(root.get()))
			subMap[sub->id] = sub;

	// Transitively find all reachable subroutines.
	while (!worklist.empty())
	{
		std::string id = worklist.front();
		worklist.pop();
		auto it = subMap.find(id);
		if (it == subMap.end())
			continue;
		std::set<std::string> refs;
		if (it->second->body)
			collectStmtRefs(*it->second->body, refs);
		for (auto const& ref: refs)
		{
			if (reachable.find(ref) == reachable.end())
			{
				reachable.insert(ref);
				worklist.push(ref);
			}
		}
	}

	// Filter: keep contracts (always) + reachable subroutines + everything else.
	std::vector<std::shared_ptr<awst::RootNode>> filtered;
	filtered.reserve(_roots.size());
	for (auto& root: _roots)
	{
		if (dynamic_cast<awst::Contract const*>(root.get()))
			filtered.push_back(std::move(root));
		else if (auto const* sub = dynamic_cast<awst::Subroutine const*>(root.get()))
		{
			if (reachable.count(sub->id))
				filtered.push_back(std::move(root));
		}
		else
			filtered.push_back(std::move(root));
	}
	return filtered;
}

} // namespace puyasol::builder
