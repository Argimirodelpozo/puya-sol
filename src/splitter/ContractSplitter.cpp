#include "splitter/ContractSplitter.h"
#include "awst/WType.h"
#include "Logger.h"

#include <algorithm>
#include <queue>
#include <sstream>

namespace puyasol::splitter
{

// Forward declaration (defined below buildThinOrchestrator)
static std::shared_ptr<awst::Expression> buildDefaultExpression(
	awst::SourceLocation const& _loc,
	awst::WType const* _type
);

ContractSplitter::SplitResult ContractSplitter::split(
	std::shared_ptr<awst::Contract> _original,
	std::vector<std::shared_ptr<awst::RootNode>>& _roots,
	CallGraphAnalyzer::SplitRecommendation const& _recommendation
)
{
	auto& logger = Logger::instance();
	SplitResult result;

	if (_recommendation.partitions.size() <= 1)
	{
		result.didSplit = false;
		return result;
	}

	logger.info("Splitting contract '" + _original->name + "' into " +
		std::to_string(_recommendation.partitions.size()) + " parts");

	// Build subroutine name→node map (for unique names)
	auto subMap = buildSubroutineMap(_roots);

	// Build ID→subroutine map for overloaded functions
	std::map<std::string, std::shared_ptr<awst::Subroutine>> subById;
	for (auto const& root: _roots)
		if (auto sub = std::dynamic_pointer_cast<awst::Subroutine>(root))
			subById[sub->id] = sub;

	// Also build ID→name map for dependency scanning
	std::map<std::string, std::string> idToName;
	for (auto const& [name, sub]: subMap)
		idToName[sub->id] = name;
	// Also add overloaded entries that were shadowed in subMap
	for (auto const& [id, sub]: subById)
		if (idToName.find(id) == idToName.end())
			idToName[id] = sub->name;

	int numHelpers = static_cast<int>(_recommendation.partitions.size()) - 1;

	// Build set of functions that are EXCLUSIVELY in another partition
	// (not shared utilities). These should NOT be pulled in as transitive deps
	// because they'll be cross-contract ABI calls.
	std::set<std::string> sharedSet(
		_recommendation.sharedUtilities.begin(),
		_recommendation.sharedUtilities.end()
	);

	std::set<std::string> allPartitionedFuncs;
	for (int i = 1; i <= numHelpers; ++i)
		for (auto const& fn: _recommendation.partitions[i])
			allPartitionedFuncs.insert(fn);

	// ─── Create helper contracts (partitions 1..N) ─────────────────────────
	for (int i = 1; i <= numHelpers; ++i)
	{
		auto const& partitionFuncs = _recommendation.partitions[i];

		// Collect assigned functions + shared utilities needed by this partition.
		// Exclude non-shared functions from OTHER partitions (cross-contract calls).
		std::set<std::string> assignedFuncs(partitionFuncs.begin(), partitionFuncs.end());

		// Build exclusion set: partitioned funcs NOT in this partition AND NOT shared
		std::set<std::string> otherPartitionFuncs;
		for (auto const& fn: allPartitionedFuncs)
			if (!assignedFuncs.count(fn) && !sharedSet.count(fn))
				otherPartitionFuncs.insert(fn);

		// Add shared utilities that are transitively needed
		auto allDeps = collectDependencies(assignedFuncs, subMap, otherPartitionFuncs);
		assignedFuncs.insert(allDeps.begin(), allDeps.end());

		// Create the helper contract
		auto helper = createHelperContract(*_original, i, partitionFuncs, subMap);

		// Build the helper's AWST: its Contract + assigned subroutines
		ContractAWST helperAWST;
		helperAWST.contractId = helper->id;
		helperAWST.contractName = helper->name;

		// Add subroutines this helper needs
		std::set<std::string> addedIds;
		for (auto const& funcName: assignedFuncs)
		{
			auto it = subMap.find(funcName);
			if (it != subMap.end())
			{
				helperAWST.roots.push_back(it->second);
				addedIds.insert(it->second->id);
			}
		}
		// Also add ALL overloaded variants with matching names
		for (auto const& funcName: assignedFuncs)
		{
			for (auto const& [id, sub]: subById)
			{
				if (sub->name == funcName && !addedIds.count(id))
				{
					helperAWST.roots.push_back(sub);
					addedIds.insert(id);
				}
			}
		}
		// Finally, scan the added subroutines for SubroutineID references
		// and include any referenced subroutines not yet added.
		// Use a self-mapping idToName (id→id) so scanExprForSubroutineIds
		// (which is comprehensive) collects raw IDs directly.
		std::map<std::string, std::string> selfIdMap;
		for (auto const& [id, sub]: subById)
			selfIdMap[id] = id;

		bool changed = true;
		while (changed)
		{
			changed = false;
			std::set<std::string> referencedIds;
			for (auto const& root: helperAWST.roots)
			{
				if (auto sub = std::dynamic_pointer_cast<awst::Subroutine>(root))
				{
					if (sub->body)
					{
						for (auto const& stmt: sub->body->body)
							if (stmt)
								scanStmtForSubroutineIds(*stmt, referencedIds, selfIdMap);
					}
				}
			}
			for (auto const& refId: referencedIds)
			{
				if (!addedIds.count(refId))
				{
					auto it = subById.find(refId);
					if (it != subById.end())
					{
						helperAWST.roots.push_back(it->second);
						addedIds.insert(refId);
						changed = true;
					}
				}
			}
		}

		// Add the Contract node
		helperAWST.roots.push_back(helper);

		result.contracts.push_back(std::move(helperAWST));

		logger.info("  Helper " + std::to_string(i) + ": " + helper->id +
			" (" + std::to_string(assignedFuncs.size()) + " subroutines)");
	}

	// ─── Create orchestrator (partition 0) ──────────────────────────────
	// Determine which methods are delegated to helpers vs kept locally.
	// Delegated methods get stubs + __finish pairs.
	// Local methods keep their original implementations.
	{
		std::set<std::string> delegatedMethods(allPartitionedFuncs);
		// Only ARC4 methods that moved to helpers are "delegated"
		// Pure subroutines in helpers are just dependencies, not delegated ARC4 methods
		std::set<std::string> delegatedARC4Methods;
		for (auto const& method: _original->methods)
		{
			if (delegatedMethods.count(method.memberName))
				delegatedARC4Methods.insert(method.memberName);
		}

		bool hasAnyDelegated = !delegatedARC4Methods.empty();

		if (hasAnyDelegated)
		{
			// Hybrid orchestrator: local methods keep original bodies,
			// delegated methods get stub + __finish pairs
			auto hybridOrch = buildHybridOrchestrator(*_original, delegatedARC4Methods);

			ContractAWST orchAWST;
			orchAWST.contractId = hybridOrch->id;
			orchAWST.contractName = hybridOrch->name;

			// Add subroutines needed by local (non-delegated) methods.
			// Use a self-mapping (id→id) to collect raw subroutine IDs directly.
			std::map<std::string, std::string> selfIdMap;
			for (auto const& [id, sub]: subById)
				selfIdMap[id] = id;

			// Scan local method bodies for subroutine references
			std::set<std::string> neededIds;
			for (auto const& method: _original->methods)
			{
				if (!delegatedARC4Methods.count(method.memberName) && method.body)
				{
					for (auto const& stmt: method.body->body)
						if (stmt)
							scanStmtForSubroutineIds(*stmt, neededIds, selfIdMap);
				}
			}

			// Transitively resolve deps
			bool changed = true;
			while (changed)
			{
				changed = false;
				std::set<std::string> newIds;
				for (auto const& id: neededIds)
				{
					auto it = subById.find(id);
					if (it != subById.end() && it->second->body)
					{
						std::set<std::string> refs;
						for (auto const& stmt: it->second->body->body)
							if (stmt)
								scanStmtForSubroutineIds(*stmt, refs, selfIdMap);
						for (auto const& r: refs)
							if (!neededIds.count(r))
								newIds.insert(r);
					}
				}
				if (!newIds.empty())
				{
					neededIds.insert(newIds.begin(), newIds.end());
					changed = true;
				}
			}

			// Add needed subroutines by ID
			std::set<std::string> addedIds;
			for (auto const& id: neededIds)
			{
				auto it = subById.find(id);
				if (it != subById.end() && !addedIds.count(id))
				{
					orchAWST.roots.push_back(it->second);
					addedIds.insert(id);
				}
			}

			orchAWST.roots.push_back(hybridOrch);
			result.contracts.push_back(std::move(orchAWST));

			size_t localCount = _original->methods.size() - delegatedARC4Methods.size();
			logger.info("  Orchestrator (hybrid): " + _original->id +
				" (" + std::to_string(localCount) + " local, " +
				std::to_string(delegatedARC4Methods.size()) + " delegated)");
		}
		else
		{
			// All methods are local — no delegation needed
			// But the orchestrator still might need subroutines, so use original
			auto thinOrch = buildThinOrchestrator(*_original);

			ContractAWST orchAWST;
			orchAWST.contractId = thinOrch->id;
			orchAWST.contractName = thinOrch->name;
			orchAWST.roots.push_back(thinOrch);

			result.contracts.push_back(std::move(orchAWST));

			logger.info("  Orchestrator (thin): " + _original->id +
				" (stub methods only, no subroutines)");
		}
	}

	result.didSplit = true;
	logger.info("Split complete: " + std::to_string(numHelpers) +
		" helpers + 1 orchestrator");

	return result;
}

std::map<std::string, std::shared_ptr<awst::Subroutine>> ContractSplitter::buildSubroutineMap(
	std::vector<std::shared_ptr<awst::RootNode>> const& _roots
)
{
	std::map<std::string, std::shared_ptr<awst::Subroutine>> result;
	for (auto const& root: _roots)
	{
		if (auto sub = std::dynamic_pointer_cast<awst::Subroutine>(root))
			result[sub->name] = sub;
	}
	return result;
}

std::set<std::string> ContractSplitter::collectDependencies(
	std::set<std::string> const& _funcNames,
	std::map<std::string, std::shared_ptr<awst::Subroutine>> const& _subMap,
	std::set<std::string> const& _otherPartitionFuncs
)
{
	// Build ID→name map
	std::map<std::string, std::string> idToName;
	for (auto const& [name, sub]: _subMap)
		idToName[sub->id] = name;

	std::set<std::string> visited;
	std::queue<std::string> worklist;

	for (auto const& name: _funcNames)
		worklist.push(name);

	while (!worklist.empty())
	{
		std::string current = worklist.front();
		worklist.pop();

		if (visited.count(current))
			continue;

		// Skip functions assigned to other partitions — they will be called
		// via cross-contract ABI calls, not included as local dependencies.
		if (_otherPartitionFuncs.count(current) && !_funcNames.count(current))
			continue;

		visited.insert(current);

		auto it = _subMap.find(current);
		if (it == _subMap.end() || !it->second->body)
			continue;

		// Scan the subroutine body for SubroutineCallExpression targets
		std::set<std::string> callees;
		for (auto const& stmt: it->second->body->body)
			if (stmt)
				scanStmtForSubroutineIds(*stmt, callees, idToName);

		for (auto const& callee: callees)
		{
			if (!visited.count(callee))
				worklist.push(callee);
		}
	}

	return visited;
}

void ContractSplitter::scanSubroutineForRawIds(
	awst::Subroutine const& _sub,
	std::set<std::string>& _ids
)
{
	if (!_sub.body)
		return;

	// Build an identity map (ID→ID) so scanExprForSubroutineIds collects raw IDs
	std::map<std::string, std::string> identityMap;
	// We'll populate as we find them — scanExprForSubroutineIds inserts
	// the id→name lookup result. With an empty map, unresolved IDs are skipped.
	// So instead, we pass a special map where every ID maps to itself.
	// But we don't know all IDs upfront. Instead, modify the scan to also
	// collect raw IDs via the _names set when _idToName is empty.

	// Use a modified approach: scan statements, collecting names that ARE the raw IDs
	// by using an idToName map where each id maps to itself.
	// Actually the simplest approach: just walk the statements and look for SubroutineCallExpression
	for (auto const& stmt: _sub.body->body)
		if (stmt)
			scanStmtForRawIds(*stmt, _ids);
}

void ContractSplitter::scanExprForRawIds(
	awst::Expression const& _expr,
	std::set<std::string>& _ids
)
{
	std::string type = _expr.nodeType();
	if (type == "SubroutineCallExpression")
	{
		auto const& call = static_cast<awst::SubroutineCallExpression const&>(_expr);
		if (auto const* sid = std::get_if<awst::SubroutineID>(&call.target))
			_ids.insert(sid->target);
		for (auto const& arg: call.args)
			if (arg.value)
				scanExprForRawIds(*arg.value, _ids);
		return;
	}
	// Reuse the same recursive pattern as scanExprForSubroutineIds
	std::map<std::string, std::string> dummy;
	std::set<std::string> dummyNames;
	// Call the existing scanner which handles all expression types
	scanExprForSubroutineIds(_expr, dummyNames, dummy);
	// That won't capture IDs because dummy is empty. Let's do it directly.
	// Just handle the few key expression types that can contain SubroutineCallExpression
	if (type == "ConditionalExpression")
	{
		auto const& cond = static_cast<awst::ConditionalExpression const&>(_expr);
		if (cond.condition) scanExprForRawIds(*cond.condition, _ids);
		if (cond.trueExpr) scanExprForRawIds(*cond.trueExpr, _ids);
		if (cond.falseExpr) scanExprForRawIds(*cond.falseExpr, _ids);
	}
}

void ContractSplitter::scanStmtForRawIds(
	awst::Statement const& _stmt,
	std::set<std::string>& _ids
)
{
	std::string type = _stmt.nodeType();
	if (type == "ExpressionStatement")
	{
		auto const& es = static_cast<awst::ExpressionStatement const&>(_stmt);
		if (es.expr) scanExprForRawIds(*es.expr, _ids);
	}
	else if (type == "AssignmentStatement")
	{
		auto const& as = static_cast<awst::AssignmentStatement const&>(_stmt);
		if (as.value) scanExprForRawIds(*as.value, _ids);
	}
	else if (type == "ReturnStatement")
	{
		auto const& rs = static_cast<awst::ReturnStatement const&>(_stmt);
		if (rs.value) scanExprForRawIds(*rs.value, _ids);
	}
	else if (type == "IfElse")
	{
		auto const& ie = static_cast<awst::IfElse const&>(_stmt);
		if (ie.condition) scanExprForRawIds(*ie.condition, _ids);
		if (ie.ifBranch)
			for (auto const& s: ie.ifBranch->body)
				if (s) scanStmtForRawIds(*s, _ids);
		if (ie.elseBranch)
			for (auto const& s: ie.elseBranch->body)
				if (s) scanStmtForRawIds(*s, _ids);
	}
	else if (type == "Block")
	{
		auto const& block = static_cast<awst::Block const&>(_stmt);
		for (auto const& s: block.body)
			if (s) scanStmtForRawIds(*s, _ids);
	}
	else if (type == "WhileLoop")
	{
		auto const& wl = static_cast<awst::WhileLoop const&>(_stmt);
		if (wl.condition) scanExprForRawIds(*wl.condition, _ids);
		if (wl.loopBody)
			for (auto const& s: wl.loopBody->body)
				if (s) scanStmtForRawIds(*s, _ids);
	}
}

void ContractSplitter::scanExprForSubroutineIds(
	awst::Expression const& _expr,
	std::set<std::string>& _names,
	std::map<std::string, std::string> const& _idToName
)
{
	std::string type = _expr.nodeType();

	if (type == "SubroutineCallExpression")
	{
		auto const& call = static_cast<awst::SubroutineCallExpression const&>(_expr);
		if (auto const* sid = std::get_if<awst::SubroutineID>(&call.target))
		{
			auto it = _idToName.find(sid->target);
			if (it != _idToName.end())
				_names.insert(it->second);
		}
		else if (auto const* imt = std::get_if<awst::InstanceMethodTarget>(&call.target))
			_names.insert(imt->memberName);

		for (auto const& arg: call.args)
			if (arg.value)
				scanExprForSubroutineIds(*arg.value, _names, _idToName);
		return;
	}

	// Recurse into child expressions
	if (type == "UInt64BinaryOperation")
	{
		auto const& op = static_cast<awst::UInt64BinaryOperation const&>(_expr);
		if (op.left) scanExprForSubroutineIds(*op.left, _names, _idToName);
		if (op.right) scanExprForSubroutineIds(*op.right, _names, _idToName);
	}
	else if (type == "BigUIntBinaryOperation")
	{
		auto const& op = static_cast<awst::BigUIntBinaryOperation const&>(_expr);
		if (op.left) scanExprForSubroutineIds(*op.left, _names, _idToName);
		if (op.right) scanExprForSubroutineIds(*op.right, _names, _idToName);
	}
	else if (type == "BytesBinaryOperation")
	{
		auto const& op = static_cast<awst::BytesBinaryOperation const&>(_expr);
		if (op.left) scanExprForSubroutineIds(*op.left, _names, _idToName);
		if (op.right) scanExprForSubroutineIds(*op.right, _names, _idToName);
	}
	else if (type == "BytesUnaryOperation")
	{
		auto const& op = static_cast<awst::BytesUnaryOperation const&>(_expr);
		if (op.expr) scanExprForSubroutineIds(*op.expr, _names, _idToName);
	}
	else if (type == "NumericComparisonExpression")
	{
		auto const& cmp = static_cast<awst::NumericComparisonExpression const&>(_expr);
		if (cmp.lhs) scanExprForSubroutineIds(*cmp.lhs, _names, _idToName);
		if (cmp.rhs) scanExprForSubroutineIds(*cmp.rhs, _names, _idToName);
	}
	else if (type == "BytesComparisonExpression")
	{
		auto const& cmp = static_cast<awst::BytesComparisonExpression const&>(_expr);
		if (cmp.lhs) scanExprForSubroutineIds(*cmp.lhs, _names, _idToName);
		if (cmp.rhs) scanExprForSubroutineIds(*cmp.rhs, _names, _idToName);
	}
	else if (type == "BooleanBinaryOperation")
	{
		auto const& op = static_cast<awst::BooleanBinaryOperation const&>(_expr);
		if (op.left) scanExprForSubroutineIds(*op.left, _names, _idToName);
		if (op.right) scanExprForSubroutineIds(*op.right, _names, _idToName);
	}
	else if (type == "Not")
	{
		auto const& n = static_cast<awst::Not const&>(_expr);
		if (n.expr) scanExprForSubroutineIds(*n.expr, _names, _idToName);
	}
	else if (type == "AssertExpression")
	{
		auto const& a = static_cast<awst::AssertExpression const&>(_expr);
		if (a.condition) scanExprForSubroutineIds(*a.condition, _names, _idToName);
	}
	else if (type == "AssignmentExpression")
	{
		auto const& a = static_cast<awst::AssignmentExpression const&>(_expr);
		if (a.target) scanExprForSubroutineIds(*a.target, _names, _idToName);
		if (a.value) scanExprForSubroutineIds(*a.value, _names, _idToName);
	}
	else if (type == "ConditionalExpression")
	{
		auto const& c = static_cast<awst::ConditionalExpression const&>(_expr);
		if (c.condition) scanExprForSubroutineIds(*c.condition, _names, _idToName);
		if (c.trueExpr) scanExprForSubroutineIds(*c.trueExpr, _names, _idToName);
		if (c.falseExpr) scanExprForSubroutineIds(*c.falseExpr, _names, _idToName);
	}
	else if (type == "IntrinsicCall")
	{
		auto const& ic = static_cast<awst::IntrinsicCall const&>(_expr);
		for (auto const& arg: ic.stackArgs)
			if (arg) scanExprForSubroutineIds(*arg, _names, _idToName);
	}
	else if (type == "PuyaLibCall")
	{
		auto const& plc = static_cast<awst::PuyaLibCall const&>(_expr);
		for (auto const& arg: plc.args)
			if (arg.value) scanExprForSubroutineIds(*arg.value, _names, _idToName);
	}
	else if (type == "FieldExpression")
	{
		auto const& f = static_cast<awst::FieldExpression const&>(_expr);
		if (f.base) scanExprForSubroutineIds(*f.base, _names, _idToName);
	}
	else if (type == "IndexExpression")
	{
		auto const& idx = static_cast<awst::IndexExpression const&>(_expr);
		if (idx.base) scanExprForSubroutineIds(*idx.base, _names, _idToName);
		if (idx.index) scanExprForSubroutineIds(*idx.index, _names, _idToName);
	}
	else if (type == "TupleExpression")
	{
		auto const& t = static_cast<awst::TupleExpression const&>(_expr);
		for (auto const& item: t.items)
			if (item) scanExprForSubroutineIds(*item, _names, _idToName);
	}
	else if (type == "TupleItemExpression")
	{
		auto const& ti = static_cast<awst::TupleItemExpression const&>(_expr);
		if (ti.base) scanExprForSubroutineIds(*ti.base, _names, _idToName);
	}
	else if (type == "ARC4Encode")
	{
		auto const& e = static_cast<awst::ARC4Encode const&>(_expr);
		if (e.value) scanExprForSubroutineIds(*e.value, _names, _idToName);
	}
	else if (type == "ARC4Decode")
	{
		auto const& d = static_cast<awst::ARC4Decode const&>(_expr);
		if (d.value) scanExprForSubroutineIds(*d.value, _names, _idToName);
	}
	else if (type == "ReinterpretCast")
	{
		auto const& rc = static_cast<awst::ReinterpretCast const&>(_expr);
		if (rc.expr) scanExprForSubroutineIds(*rc.expr, _names, _idToName);
	}
	else if (type == "Copy")
	{
		auto const& c = static_cast<awst::Copy const&>(_expr);
		if (c.value) scanExprForSubroutineIds(*c.value, _names, _idToName);
	}
	else if (type == "SingleEvaluation")
	{
		auto const& se = static_cast<awst::SingleEvaluation const&>(_expr);
		if (se.source) scanExprForSubroutineIds(*se.source, _names, _idToName);
	}
	else if (type == "CheckedMaybe")
	{
		auto const& cm = static_cast<awst::CheckedMaybe const&>(_expr);
		if (cm.expr) scanExprForSubroutineIds(*cm.expr, _names, _idToName);
	}
	else if (type == "NewArray")
	{
		auto const& na = static_cast<awst::NewArray const&>(_expr);
		for (auto const& v: na.values)
			if (v) scanExprForSubroutineIds(*v, _names, _idToName);
	}
	else if (type == "ArrayLength")
	{
		auto const& al = static_cast<awst::ArrayLength const&>(_expr);
		if (al.array) scanExprForSubroutineIds(*al.array, _names, _idToName);
	}
	else if (type == "ArrayPop")
	{
		auto const& ap = static_cast<awst::ArrayPop const&>(_expr);
		if (ap.base) scanExprForSubroutineIds(*ap.base, _names, _idToName);
	}
	else if (type == "ArrayConcat")
	{
		auto const& ac = static_cast<awst::ArrayConcat const&>(_expr);
		if (ac.left) scanExprForSubroutineIds(*ac.left, _names, _idToName);
		if (ac.right) scanExprForSubroutineIds(*ac.right, _names, _idToName);
	}
	else if (type == "ArrayExtend")
	{
		auto const& ae = static_cast<awst::ArrayExtend const&>(_expr);
		if (ae.base) scanExprForSubroutineIds(*ae.base, _names, _idToName);
		if (ae.other) scanExprForSubroutineIds(*ae.other, _names, _idToName);
	}
	else if (type == "StateGet")
	{
		auto const& sg = static_cast<awst::StateGet const&>(_expr);
		if (sg.field) scanExprForSubroutineIds(*sg.field, _names, _idToName);
		if (sg.defaultValue) scanExprForSubroutineIds(*sg.defaultValue, _names, _idToName);
	}
	else if (type == "StateExists")
	{
		auto const& se = static_cast<awst::StateExists const&>(_expr);
		if (se.field) scanExprForSubroutineIds(*se.field, _names, _idToName);
	}
	else if (type == "StateDelete")
	{
		auto const& sd = static_cast<awst::StateDelete const&>(_expr);
		if (sd.field) scanExprForSubroutineIds(*sd.field, _names, _idToName);
	}
	else if (type == "StateGetEx")
	{
		auto const& sge = static_cast<awst::StateGetEx const&>(_expr);
		if (sge.field) scanExprForSubroutineIds(*sge.field, _names, _idToName);
	}
	else if (type == "BoxPrefixedKeyExpression")
	{
		auto const& bpk = static_cast<awst::BoxPrefixedKeyExpression const&>(_expr);
		if (bpk.prefix) scanExprForSubroutineIds(*bpk.prefix, _names, _idToName);
		if (bpk.key) scanExprForSubroutineIds(*bpk.key, _names, _idToName);
	}
	else if (type == "BoxValueExpression")
	{
		auto const& bve = static_cast<awst::BoxValueExpression const&>(_expr);
		if (bve.key) scanExprForSubroutineIds(*bve.key, _names, _idToName);
	}
	else if (type == "NewStruct")
	{
		auto const& ns = static_cast<awst::NewStruct const&>(_expr);
		for (auto const& [_, val]: ns.values)
			if (val) scanExprForSubroutineIds(*val, _names, _idToName);
	}
	else if (type == "NamedTupleExpression")
	{
		auto const& nt = static_cast<awst::NamedTupleExpression const&>(_expr);
		for (auto const& [_, val]: nt.values)
			if (val) scanExprForSubroutineIds(*val, _names, _idToName);
	}
	else if (type == "Emit")
	{
		auto const& e = static_cast<awst::Emit const&>(_expr);
		if (e.value) scanExprForSubroutineIds(*e.value, _names, _idToName);
	}
	else if (type == "CreateInnerTransaction")
	{
		auto const& cit = static_cast<awst::CreateInnerTransaction const&>(_expr);
		for (auto const& [_, val]: cit.fields)
			if (val) scanExprForSubroutineIds(*val, _names, _idToName);
	}
	else if (type == "SubmitInnerTransaction")
	{
		auto const& sit = static_cast<awst::SubmitInnerTransaction const&>(_expr);
		for (auto const& itxn: sit.itxns)
			if (itxn) scanExprForSubroutineIds(*itxn, _names, _idToName);
	}
	else if (type == "InnerTransactionField")
	{
		auto const& itf = static_cast<awst::InnerTransactionField const&>(_expr);
		if (itf.itxn) scanExprForSubroutineIds(*itf.itxn, _names, _idToName);
	}
	else if (type == "CommaExpression")
	{
		auto const& ce = static_cast<awst::CommaExpression const&>(_expr);
		for (auto const& e: ce.expressions)
			if (e) scanExprForSubroutineIds(*e, _names, _idToName);
	}
}

void ContractSplitter::scanStmtForSubroutineIds(
	awst::Statement const& _stmt,
	std::set<std::string>& _names,
	std::map<std::string, std::string> const& _idToName
)
{
	std::string type = _stmt.nodeType();

	if (type == "Block")
	{
		auto const& block = static_cast<awst::Block const&>(_stmt);
		for (auto const& s: block.body)
			if (s) scanStmtForSubroutineIds(*s, _names, _idToName);
	}
	else if (type == "ExpressionStatement")
	{
		auto const& es = static_cast<awst::ExpressionStatement const&>(_stmt);
		if (es.expr) scanExprForSubroutineIds(*es.expr, _names, _idToName);
	}
	else if (type == "ReturnStatement")
	{
		auto const& rs = static_cast<awst::ReturnStatement const&>(_stmt);
		if (rs.value) scanExprForSubroutineIds(*rs.value, _names, _idToName);
	}
	else if (type == "IfElse")
	{
		auto const& ie = static_cast<awst::IfElse const&>(_stmt);
		if (ie.condition) scanExprForSubroutineIds(*ie.condition, _names, _idToName);
		if (ie.ifBranch) scanStmtForSubroutineIds(*ie.ifBranch, _names, _idToName);
		if (ie.elseBranch) scanStmtForSubroutineIds(*ie.elseBranch, _names, _idToName);
	}
	else if (type == "WhileLoop")
	{
		auto const& wl = static_cast<awst::WhileLoop const&>(_stmt);
		if (wl.condition) scanExprForSubroutineIds(*wl.condition, _names, _idToName);
		if (wl.loopBody) scanStmtForSubroutineIds(*wl.loopBody, _names, _idToName);
	}
	else if (type == "AssignmentStatement")
	{
		auto const& as = static_cast<awst::AssignmentStatement const&>(_stmt);
		if (as.target) scanExprForSubroutineIds(*as.target, _names, _idToName);
		if (as.value) scanExprForSubroutineIds(*as.value, _names, _idToName);
	}
	else if (type == "Switch")
	{
		auto const& sw = static_cast<awst::Switch const&>(_stmt);
		if (sw.value) scanExprForSubroutineIds(*sw.value, _names, _idToName);
		for (auto const& [caseExpr, caseBlock]: sw.cases)
		{
			if (caseExpr) scanExprForSubroutineIds(*caseExpr, _names, _idToName);
			if (caseBlock) scanStmtForSubroutineIds(*caseBlock, _names, _idToName);
		}
		if (sw.defaultCase) scanStmtForSubroutineIds(*sw.defaultCase, _names, _idToName);
	}
	else if (type == "ForInLoop")
	{
		auto const& fil = static_cast<awst::ForInLoop const&>(_stmt);
		if (fil.sequence) scanExprForSubroutineIds(*fil.sequence, _names, _idToName);
		if (fil.items) scanExprForSubroutineIds(*fil.items, _names, _idToName);
		if (fil.loopBody) scanStmtForSubroutineIds(*fil.loopBody, _names, _idToName);
	}
	else if (type == "UInt64AugmentedAssignment")
	{
		auto const& ua = static_cast<awst::UInt64AugmentedAssignment const&>(_stmt);
		if (ua.target) scanExprForSubroutineIds(*ua.target, _names, _idToName);
		if (ua.value) scanExprForSubroutineIds(*ua.value, _names, _idToName);
	}
	else if (type == "BigUIntAugmentedAssignment")
	{
		auto const& ba = static_cast<awst::BigUIntAugmentedAssignment const&>(_stmt);
		if (ba.target) scanExprForSubroutineIds(*ba.target, _names, _idToName);
		if (ba.value) scanExprForSubroutineIds(*ba.value, _names, _idToName);
	}
}

std::shared_ptr<awst::Contract> ContractSplitter::createHelperContract(
	awst::Contract const& _original,
	int _helperIndex,
	std::vector<std::string> const& _functionNames,
	std::map<std::string, std::shared_ptr<awst::Subroutine>> const& _subMap
)
{
	auto helper = std::make_shared<awst::Contract>();
	awst::SourceLocation loc = _original.sourceLocation;

	helper->id = _original.id + "__Helper" + std::to_string(_helperIndex);
	helper->name = _original.name + "__Helper" + std::to_string(_helperIndex);
	helper->description = "Helper contract " + std::to_string(_helperIndex) +
		" for " + _original.name;
	helper->methodResolutionOrder = {helper->id};
	helper->avmVersion = _original.avmVersion;

	// Auth state: orchestrator app_id, prev chunk app_id, prev method selector
	{
		auto makeKey = [&](std::string const& k) {
			return awst::makeUtf8BytesConstant(k, loc);
		};
		// "o" = orchestrator_app_id (uint64)
		awst::AppStorageDefinition oDef;
		oDef.sourceLocation = loc;
		oDef.memberName = "o";
		oDef.storageKind = awst::AppStorageKind::AppGlobal;
		oDef.storageWType = awst::WType::uint64Type();
		oDef.key = makeKey("o");
		helper->appState.push_back(oDef);

		// "p" = prev_chunk_app_id (uint64, 0 for first chunks)
		awst::AppStorageDefinition pDef;
		pDef.sourceLocation = loc;
		pDef.memberName = "p";
		pDef.storageKind = awst::AppStorageKind::AppGlobal;
		pDef.storageWType = awst::WType::uint64Type();
		pDef.key = makeKey("p");
		helper->appState.push_back(pDef);

		// "s" = prev_method_selector (bytes, empty for first chunks)
		awst::AppStorageDefinition sDef;
		sDef.sourceLocation = loc;
		sDef.memberName = "s";
		sDef.storageKind = awst::AppStorageKind::AppGlobal;
		sDef.storageWType = awst::WType::bytesType();
		sDef.key = makeKey("s");
		helper->appState.push_back(sDef);
	}

	// Wrap each assigned subroutine as an ABI method on the helper.
	// This makes the subroutines callable and ensures puya includes them.
	for (auto const& funcName: _functionNames)
	{
		auto it = _subMap.find(funcName);
		if (it == _subMap.end())
		{
			// No matching subroutine — check if it's a contract method
			// (e.g., thin wrappers like `commonExp2` that call library functions)
			for (auto const& origMethod: _original.methods)
			{
				if (origMethod.memberName == funcName && origMethod.body)
				{
					awst::ContractMethod method = origMethod;
					method.cref = helper->id;

					// Add scratch slot 0 store before each return statement
					// so the orchestrator's __finish method can read the result via gloads.
					if (method.returnType != awst::WType::voidType() && method.body)
					{
						auto newBody = std::make_shared<awst::Block>();
						newBody->sourceLocation = method.body->sourceLocation;
						for (auto const& stmt: method.body->body)
						{
							if (stmt && stmt->nodeType() == "ReturnStatement")
							{
								auto const& rs = static_cast<awst::ReturnStatement const&>(*stmt);
								if (rs.value)
								{
									auto sloc = rs.sourceLocation;
									// Store return value to scratch slot 0 as bytes
									std::shared_ptr<awst::Expression> storeVal = rs.value;
									if (method.returnType == awst::WType::biguintType())
									{
										auto cast = std::make_shared<awst::ReinterpretCast>();
										cast->sourceLocation = sloc;
										cast->expr = storeVal;
										cast->wtype = awst::WType::bytesType();
										storeVal = cast;
									}
									else if (method.returnType == awst::WType::uint64Type())
									{
										auto itob = std::make_shared<awst::IntrinsicCall>();
										itob->sourceLocation = sloc;
										itob->opCode = "itob";
										itob->wtype = awst::WType::bytesType();
										itob->stackArgs.push_back(storeVal);
										storeVal = itob;
									}
									auto storeIntr = std::make_shared<awst::IntrinsicCall>();
									storeIntr->sourceLocation = sloc;
									storeIntr->opCode = "store";
									storeIntr->immediates = {0};
									storeIntr->stackArgs.push_back(storeVal);
									storeIntr->wtype = awst::WType::voidType();
									auto storeStmt = std::make_shared<awst::ExpressionStatement>();
									storeStmt->sourceLocation = sloc;
									storeStmt->expr = storeIntr;
									newBody->body.push_back(storeStmt);
								}
							}
							newBody->body.push_back(stmt);
						}
						method.body = newBody;
					}

					helper->methods.push_back(std::move(method));
					break;
				}
			}
			continue;
		}

		auto const& sub = it->second;

		awst::ContractMethod method;
		method.sourceLocation = sub->sourceLocation;
		method.returnType = sub->returnType;
		method.cref = helper->id;
		method.memberName = sub->name;
		method.pure = sub->pure;
		method.documentation = sub->documentation;

		// Detect oversized ABI arguments that won't fit in ApplicationArgs
		// (AVM limit: 2048 bytes total). Move them to scratch slot loading
		// via gload from loader transactions earlier in the group.
		struct ScratchLoadedArg
		{
			size_t originalIndex;
			std::string name;
			awst::WType const* wtype;
			size_t encodedSize;
		};
		std::vector<ScratchLoadedArg> scratchArgs;

		{
			size_t totalABISize = 4; // 4-byte method selector
			std::vector<std::pair<size_t, size_t>> argSizes; // (encodedSize, index)
			for (size_t i = 0; i < sub->args.size(); ++i)
			{
				size_t sz = SizeEstimator::estimateABIEncodedSize(sub->args[i].wtype);
				totalABISize += sz;
				argSizes.push_back({sz, i});
			}

			if (totalABISize > 2048)
			{
				// Sort by size descending, remove largest args until total fits.
				// Skip ReferenceArray types — puya can't ReinterpretCast bytes
				// to ref_array; these stay as normal ABI args (they're usually small).
				std::sort(argSizes.begin(), argSizes.end(), std::greater<>());
				size_t remaining = totalABISize;
				for (auto const& [sz, idx]: argSizes)
				{
					if (remaining <= 2048)
						break;
					auto const* wtype = sub->args[idx].wtype;
					if (wtype && wtype->kind() == awst::WTypeKind::ReferenceArray)
						continue; // skip — can't scratch-load ReferenceArray
					scratchArgs.push_back(
						{idx, sub->args[idx].name, sub->args[idx].wtype, sz}
					);
					remaining -= sz;
				}

				// Build filtered args list (excluding scratch-loaded)
				std::set<size_t> scratchIndices;
				for (auto const& sa: scratchArgs)
					scratchIndices.insert(sa.originalIndex);

				for (size_t i = 0; i < sub->args.size(); ++i)
				{
					if (!scratchIndices.count(i))
						method.args.push_back(sub->args[i]);
				}

				auto& logger = Logger::instance();
				logger.info("    Method '" + sub->name + "': " +
					std::to_string(scratchArgs.size()) +
					" arg(s) moved to scratch slots (" +
					std::to_string(totalABISize) + " → " +
					std::to_string(remaining) + " bytes)");
			}
			else
			{
				method.args = sub->args;
			}
		}

		// Build method body: call the subroutine and return its result
		auto body = std::make_shared<awst::Block>();
		body->sourceLocation = sub->sourceLocation;

		auto callExpr = std::make_shared<awst::SubroutineCallExpression>();
		callExpr->sourceLocation = sub->sourceLocation;
		callExpr->wtype = sub->returnType;
		callExpr->target = awst::SubroutineID{sub->id};

		// Pass all args through (including scratch-loaded ones as VarExpressions
		// that will be assigned from gload reconstruction below)
		for (auto const& arg: sub->args)
		{
			auto varExpr = std::make_shared<awst::VarExpression>();
			varExpr->sourceLocation = arg.sourceLocation;
			varExpr->wtype = arg.wtype;
			varExpr->name = arg.name;
			callExpr->args.push_back(awst::CallArg{arg.name, varExpr});
		}

		// Insert scratch slot reconstruction code for oversized args.
		// Convention: loader transactions at group indices 1, 2, ...
		// Each loader stores up to 2 scratch slots (max 4096 bytes each).
		// Chunks read via gload(loaderGroupIdx, slotIdx) and concatenate.
		if (!scratchArgs.empty())
		{
			constexpr int SLOT_MAX_BYTES = 4096;
			constexpr int SLOTS_PER_LOADER = 2;

			// Sort by original index for consistent slot assignment
			auto sortedScratchArgs = scratchArgs;
			std::sort(sortedScratchArgs.begin(), sortedScratchArgs.end(),
				[](auto const& a, auto const& b) {
					return a.originalIndex < b.originalIndex;
				});

			int nextLoaderGroupIdx = 1;
			int currentSlotInLoader = 0;

			for (auto const& sa: sortedScratchArgs)
			{
				int numSlots = static_cast<int>(
					(sa.encodedSize + SLOT_MAX_BYTES - 1) / SLOT_MAX_BYTES
				);

				// Generate gload expressions for each scratch slot
				std::vector<std::shared_ptr<awst::Expression>> parts;
				for (int s = 0; s < numSlots; ++s)
				{
					auto gloadExpr = std::make_shared<awst::IntrinsicCall>();
					gloadExpr->sourceLocation = sub->sourceLocation;
					gloadExpr->opCode = "gload";
					gloadExpr->immediates = {nextLoaderGroupIdx, currentSlotInLoader};
					gloadExpr->wtype = awst::WType::bytesType();
					parts.push_back(gloadExpr);

					currentSlotInLoader++;
					if (currentSlotInLoader >= SLOTS_PER_LOADER)
					{
						currentSlotInLoader = 0;
						nextLoaderGroupIdx++;
					}
				}

				// Concatenate all parts: concat(concat(p0, p1), p2) ...
				std::shared_ptr<awst::Expression> concatenated = parts[0];
				for (size_t p = 1; p < parts.size(); ++p)
				{
					auto concatOp = std::make_shared<awst::BytesBinaryOperation>();
					concatOp->sourceLocation = sub->sourceLocation;
					concatOp->op = awst::BytesBinaryOperator::Add;
					concatOp->left = concatenated;
					concatOp->right = parts[p];
					concatOp->wtype = awst::WType::bytesType();
					concatenated = concatOp;
				}

				// ReinterpretCast to the original arg type.
				// The gload bytes are raw ARC4-encoded data — same format
				// as ApplicationArgs — so reinterpreting as the target type
				// is correct (ARC4Struct/ReferenceArray are bytes at runtime).
				auto cast = std::make_shared<awst::ReinterpretCast>();
				cast->sourceLocation = sub->sourceLocation;
				cast->expr = concatenated;
				cast->wtype = sa.wtype;

				// Assign to local variable with the original arg name
				auto assign = std::make_shared<awst::AssignmentStatement>();
				assign->sourceLocation = sub->sourceLocation;
				auto target = std::make_shared<awst::VarExpression>();
				target->sourceLocation = sub->sourceLocation;
				target->wtype = sa.wtype;
				target->name = sa.name;
				assign->target = target;
				assign->value = cast;

				body->body.push_back(assign);
			}
		}

		if (sub->returnType != awst::WType::voidType())
		{
			// Store result in a temp variable, then store to scratch 0 (for gloads
			// by orchestrator __finish method), then return it (for ARC4 log).
			std::string tmpName = "__result__";
			auto tmpVar = [&]() {
				auto v = std::make_shared<awst::VarExpression>();
				v->sourceLocation = sub->sourceLocation;
				v->wtype = sub->returnType;
				v->name = tmpName;
				return v;
			};

			// Assign result to temp
			auto assign = std::make_shared<awst::AssignmentStatement>();
			assign->sourceLocation = sub->sourceLocation;
			assign->target = tmpVar();
			assign->value = callExpr;
			body->body.push_back(assign);

			// Store result to scratch slot 0 as bytes (for gloads)
			{
				std::shared_ptr<awst::Expression> storeVal = tmpVar();
				// Convert to bytes if not already
				if (sub->returnType == awst::WType::biguintType())
				{
					auto cast = std::make_shared<awst::ReinterpretCast>();
					cast->sourceLocation = sub->sourceLocation;
					cast->expr = storeVal;
					cast->wtype = awst::WType::bytesType();
					storeVal = cast;
				}
				else if (sub->returnType == awst::WType::uint64Type())
				{
					auto itob = std::make_shared<awst::IntrinsicCall>();
					itob->sourceLocation = sub->sourceLocation;
					itob->opCode = "itob";
					itob->wtype = awst::WType::bytesType();
					itob->stackArgs.push_back(storeVal);
					storeVal = itob;
				}
				else if (sub->returnType == awst::WType::boolType())
				{
					// bool → itob(btoi equivalent)
					auto boolToInt = std::make_shared<awst::ReinterpretCast>();
					boolToInt->sourceLocation = sub->sourceLocation;
					boolToInt->expr = storeVal;
					boolToInt->wtype = awst::WType::uint64Type();
					auto itob = std::make_shared<awst::IntrinsicCall>();
					itob->sourceLocation = sub->sourceLocation;
					itob->opCode = "itob";
					itob->wtype = awst::WType::bytesType();
					itob->stackArgs.push_back(boolToInt);
					storeVal = itob;
				}
				// else: bytes/ARC4 types are already bytes at runtime

				auto storeIntr = std::make_shared<awst::IntrinsicCall>();
				storeIntr->sourceLocation = sub->sourceLocation;
				storeIntr->opCode = "store";
				storeIntr->immediates = {0};
				storeIntr->stackArgs.push_back(storeVal);
				storeIntr->wtype = awst::WType::voidType();

				auto storeStmt = std::make_shared<awst::ExpressionStatement>();
				storeStmt->sourceLocation = sub->sourceLocation;
				storeStmt->expr = storeIntr;
				body->body.push_back(storeStmt);
			}

			auto ret = std::make_shared<awst::ReturnStatement>();
			ret->sourceLocation = sub->sourceLocation;
			ret->value = tmpVar();
			body->body.push_back(ret);
		}
		else
		{
			// For void subroutines, we need to prevent puya's dead code
			// elimination from removing the call. Void functions that mutate
			// array parameters (like relation accumulators) would otherwise
			// be eliminated because ABI copy semantics make the mutations
			// invisible after the method returns.
			//
			// Fix: find the first ReferenceArray parameter and return it
			// after the call. This creates a data dependency that forces
			// puya to keep the subroutine call.
			awst::WType const* arrayParamType = nullptr;
			std::string arrayParamName;
			for (auto const& arg: sub->args)
			{
				if (arg.wtype && arg.wtype->kind() == awst::WTypeKind::ReferenceArray)
				{
					arrayParamType = arg.wtype;
					arrayParamName = arg.name;
					break;
				}
			}

			auto stmt = std::make_shared<awst::ExpressionStatement>();
			stmt->sourceLocation = sub->sourceLocation;
			stmt->expr = callExpr;
			body->body.push_back(stmt);

			if (arrayParamType != nullptr)
			{
				// Return the mutated array to prevent DCE
				method.returnType = arrayParamType;

				auto varExpr = std::make_shared<awst::VarExpression>();
				varExpr->sourceLocation = sub->sourceLocation;
				varExpr->wtype = arrayParamType;
				varExpr->name = arrayParamName;

				auto ret = std::make_shared<awst::ReturnStatement>();
				ret->sourceLocation = sub->sourceLocation;
				ret->value = varExpr;
				body->body.push_back(ret);
			}
			else
			{
				auto ret = std::make_shared<awst::ReturnStatement>();
				ret->sourceLocation = sub->sourceLocation;
				body->body.push_back(ret);
			}
		}

		// Prepend validation assertions to the method body
		{
			auto validationStmts = buildValidationBlock(sub->sourceLocation);
			body->body.insert(body->body.begin(), validationStmts.begin(), validationStmts.end());
		}

		method.body = body;

		// ABI config: allow NoOp calls
		awst::ARC4ABIMethodConfig abiConfig;
		abiConfig.sourceLocation = sub->sourceLocation;
		abiConfig.allowedCompletionTypes = {0}; // NoOp
		abiConfig.create = 3; // Disallow
		abiConfig.name = sub->name;
		abiConfig.readonly = sub->pure;
		method.arc4MethodConfig = abiConfig;

		helper->methods.push_back(std::move(method));
	}

	// Add a bare create method (empty body, just deploys the contract)
	{
		awst::ContractMethod bareCreate;
		bareCreate.sourceLocation = loc;
		bareCreate.returnType = awst::WType::voidType();
		bareCreate.cref = helper->id;
		bareCreate.memberName = "__bare_create__";

		auto body = std::make_shared<awst::Block>();
		body->sourceLocation = loc;
		auto ret = std::make_shared<awst::ReturnStatement>();
		ret->sourceLocation = loc;
		body->body.push_back(ret);
		bareCreate.body = body;

		awst::ARC4BareMethodConfig bareConfig;
		bareConfig.sourceLocation = loc;
		bareConfig.allowedCompletionTypes = {0}; // NoOp
		bareConfig.create = 2; // Require (create-only)
		bareCreate.arc4MethodConfig = bareConfig;

		helper->methods.push_back(std::move(bareCreate));
	}

	// Add __init__(uint64,uint64,byte[])void ABI method to set auth state after creation.
	// Called once: __init__(orchestrator_app_id, prev_chunk_app_id, prev_method_selector)
	{
		awst::ContractMethod initMethod;
		initMethod.sourceLocation = loc;
		initMethod.returnType = awst::WType::voidType();
		initMethod.cref = helper->id;
		initMethod.memberName = "__init__";

		// Args: o (uint64), p (uint64), s (bytes)
		initMethod.args = {
			awst::SubroutineArgument{"o", loc, awst::WType::uint64Type()},
			awst::SubroutineArgument{"p", loc, awst::WType::uint64Type()},
			awst::SubroutineArgument{"s", loc, awst::WType::bytesType()},
		};

		auto body = std::make_shared<awst::Block>();
		body->sourceLocation = loc;

		auto makeIntrinsic = [&](
			std::string op,
			std::vector<std::variant<std::string, int>> imm,
			std::vector<std::shared_ptr<awst::Expression>> args,
			awst::WType const* type
		) {
			auto ic = std::make_shared<awst::IntrinsicCall>();
			ic->sourceLocation = loc;
			ic->opCode = std::move(op);
			ic->immediates = std::move(imm);
			ic->stackArgs = std::move(args);
			ic->wtype = type;
			return ic;
		};
		auto makeBytesKey = [&](std::string const& k) {
			return awst::makeUtf8BytesConstant(k, loc);
		};
		auto makeVar = [&](std::string name, awst::WType const* type) {
			auto v = std::make_shared<awst::VarExpression>();
			v->sourceLocation = loc;
			v->wtype = type;
			v->name = std::move(name);
			return v;
		};

		// Guard: only callable once (before auth state is set)
		{
			auto currentO = makeIntrinsic("app_global_get", {}, {makeBytesKey("o")}, awst::WType::uint64Type());
			auto zero = awst::makeIntegerConstant("0", loc);
			auto cmp = std::make_shared<awst::NumericComparisonExpression>();
			cmp->sourceLocation = loc;
			cmp->wtype = awst::WType::boolType();
			cmp->lhs = currentO;
			cmp->op = awst::NumericComparison::Eq;
			cmp->rhs = zero;
			auto es = std::make_shared<awst::ExpressionStatement>();
			es->sourceLocation = loc;
			es->expr = awst::makeAssert(cmp, loc, "helper: already initialized",
				awst::WType::boolType());
			body->body.push_back(es);
		}

		// app_global_put("o", o)
		{
			auto put = makeIntrinsic("app_global_put", {}, {makeBytesKey("o"), makeVar("o", awst::WType::uint64Type())}, awst::WType::voidType());
			auto stmt = std::make_shared<awst::ExpressionStatement>();
			stmt->sourceLocation = loc;
			stmt->expr = put;
			body->body.push_back(stmt);
		}
		// app_global_put("p", p)
		{
			auto put = makeIntrinsic("app_global_put", {}, {makeBytesKey("p"), makeVar("p", awst::WType::uint64Type())}, awst::WType::voidType());
			auto stmt = std::make_shared<awst::ExpressionStatement>();
			stmt->sourceLocation = loc;
			stmt->expr = put;
			body->body.push_back(stmt);
		}
		// app_global_put("s", s)
		{
			auto put = makeIntrinsic("app_global_put", {}, {makeBytesKey("s"), makeVar("s", awst::WType::bytesType())}, awst::WType::voidType());
			auto stmt = std::make_shared<awst::ExpressionStatement>();
			stmt->sourceLocation = loc;
			stmt->expr = put;
			body->body.push_back(stmt);
		}

		auto ret = std::make_shared<awst::ReturnStatement>();
		ret->sourceLocation = loc;
		body->body.push_back(ret);
		initMethod.body = body;

		awst::ARC4ABIMethodConfig abiConfig;
		abiConfig.sourceLocation = loc;
		abiConfig.allowedCompletionTypes = {0}; // NoOp
		abiConfig.create = 3; // Disallow
		abiConfig.name = "__init__";
		initMethod.arc4MethodConfig = abiConfig;

		helper->methods.push_back(std::move(initMethod));
	}

	// Build approval program with ARC4 router
	{
		auto body = std::make_shared<awst::Block>();
		body->sourceLocation = loc;

		auto routerExpr = std::make_shared<awst::ARC4Router>();
		routerExpr->sourceLocation = loc;
		routerExpr->wtype = awst::WType::boolType();

		auto ret = std::make_shared<awst::ReturnStatement>();
		ret->sourceLocation = loc;
		ret->value = routerExpr;
		body->body.push_back(ret);

		helper->approvalProgram.sourceLocation = loc;
		helper->approvalProgram.cref = helper->id;
		helper->approvalProgram.memberName = "__puya_arc4_router__";
		helper->approvalProgram.returnType = awst::WType::boolType();
		helper->approvalProgram.body = body;
	}

	// Clear program: always approve
	helper->clearProgram = buildClearProgram(loc, helper->id);

	return helper;
}

std::shared_ptr<awst::Contract> ContractSplitter::buildThinOrchestrator(
	awst::Contract const& _original
)
{
	auto orch = std::make_shared<awst::Contract>();
	awst::SourceLocation loc = _original.sourceLocation;

	orch->id = _original.id;
	orch->name = _original.name;
	orch->description = "Orchestrator (entrypoint) for " + _original.name +
		" — receives calls, dispatches to helper chain via group txns";
	orch->methodResolutionOrder = _original.methodResolutionOrder;
	orch->avmVersion = _original.avmVersion;

	// Copy app state declarations (so ARC56 metadata is preserved)
	orch->appState = _original.appState;

	// Add global state for the active-call flag "f"
	{
		auto makeKey = [&](std::string const& k) {
			return awst::makeUtf8BytesConstant(k, loc);
		};
		awst::AppStorageDefinition fDef;
		fDef.sourceLocation = loc;
		fDef.memberName = "f";
		fDef.storageKind = awst::AppStorageKind::AppGlobal;
		fDef.storageWType = awst::WType::bytesType();
		fDef.key = makeKey("f");
		orch->appState.push_back(fDef);
	}

	// AWST helper lambdas (reused across method generation)
	auto makeIntrinsic = [&](
		std::string op,
		std::vector<std::variant<std::string, int>> imm,
		std::vector<std::shared_ptr<awst::Expression>> args,
		awst::WType const* type
	) {
		auto ic = std::make_shared<awst::IntrinsicCall>();
		ic->sourceLocation = loc;
		ic->opCode = std::move(op);
		ic->immediates = std::move(imm);
		ic->stackArgs = std::move(args);
		ic->wtype = type;
		return ic;
	};
	auto makeBytesKey = [&](std::string const& k) {
		return awst::makeUtf8BytesConstant(k, loc);
	};
	auto makeBytesHex = [&](std::vector<unsigned char> const& v) {
		return awst::makeBytesConstant(v, loc);
	};
	auto makeUint64 = [&](std::string const& v) {
		auto val = awst::makeIntegerConstant(v, loc);
		return val;
	};
	auto makeAssertExpr = [&](std::shared_ptr<awst::Expression> cond, std::string msg) {
		return awst::makeAssert(std::move(cond), loc, std::move(msg), awst::WType::boolType());
	};
	auto makeAssertStmt = [&](std::shared_ptr<awst::Expression> cond, std::string msg) {
		auto es = std::make_shared<awst::ExpressionStatement>();
		es->sourceLocation = loc;
		es->expr = makeAssertExpr(std::move(cond), std::move(msg));
		return es;
	};
	auto makeBytesCmp = [&](
		std::shared_ptr<awst::Expression> lhs,
		awst::EqualityComparison op,
		std::shared_ptr<awst::Expression> rhs
	) {
		auto cmp = std::make_shared<awst::BytesComparisonExpression>();
		cmp->sourceLocation = loc;
		cmp->wtype = awst::WType::boolType();
		cmp->lhs = std::move(lhs);
		cmp->op = op;
		cmp->rhs = std::move(rhs);
		return cmp;
	};
	auto makeNumericCmp = [&](
		std::shared_ptr<awst::Expression> lhs,
		awst::NumericComparison op,
		std::shared_ptr<awst::Expression> rhs
	) {
		auto cmp = std::make_shared<awst::NumericComparisonExpression>();
		cmp->sourceLocation = loc;
		cmp->wtype = awst::WType::boolType();
		cmp->lhs = std::move(lhs);
		cmp->op = op;
		cmp->rhs = std::move(rhs);
		return cmp;
	};
	auto makeVar = [&](std::string name, awst::WType const* type) {
		auto v = std::make_shared<awst::VarExpression>();
		v->sourceLocation = loc;
		v->wtype = type;
		v->name = std::move(name);
		return v;
	};

	// For each original method, generate:
	// 1. The entrypoint method (same ABI signature): sets flag, stores args to scratch, returns true
	// 2. A __finish_<name>() method: reads result from last helper, clears flag, returns result
	for (auto const& method: _original.methods)
	{
		// ─── Entrypoint method: foo(args...) -> RetType ──────────────────
		// Body: assert flag is clear, set flag, store args to scratch, return true/default
		{
			awst::ContractMethod entry;
			entry.sourceLocation = method.sourceLocation;
			entry.args = method.args;
			entry.returnType = method.returnType;
			entry.cref = orch->id;
			entry.memberName = method.memberName;
			entry.pure = method.pure;
			entry.documentation = method.documentation;
			entry.arc4MethodConfig = method.arc4MethodConfig;

			auto body = std::make_shared<awst::Block>();
			body->sourceLocation = loc;

			// 1. assert(app_global_get("f") == "") — flag must be clear
			{
				auto flag = makeIntrinsic("app_global_get", {}, {makeBytesKey("f")}, awst::WType::bytesType());
				auto empty = makeBytesHex({});
				body->body.push_back(makeAssertStmt(
					makeBytesCmp(flag, awst::EqualityComparison::Eq, empty),
					"orchestrator: reentrant call"
				));
			}

			// 2. app_global_put("f", txna ApplicationArgs 0) — set flag to method selector
			{
				auto selector = makeIntrinsic("txna", {std::string("ApplicationArgs"), 0}, {}, awst::WType::bytesType());
				auto put = makeIntrinsic("app_global_put", {}, {makeBytesKey("f"), selector}, awst::WType::voidType());
				auto stmt = std::make_shared<awst::ExpressionStatement>();
				stmt->sourceLocation = loc;
				stmt->expr = put;
				body->body.push_back(stmt);
			}

			// 3. Store each arg to a scratch slot: store(slotIdx, argValue)
			// For bytes/biguint args: use IntrinsicCall("store", {slotIdx}, {arg})
			for (size_t i = 0; i < method.args.size(); ++i)
			{
				auto argVar = makeVar(method.args[i].name, method.args[i].wtype);

				// If arg is not bytes, cast to bytes first (scratch stores bytes)
				std::shared_ptr<awst::Expression> storeValue = argVar;
				if (method.args[i].wtype != awst::WType::bytesType() &&
					method.args[i].wtype->kind() != awst::WTypeKind::Bytes &&
					method.args[i].wtype->kind() != awst::WTypeKind::ReferenceArray &&
					method.args[i].wtype->kind() != awst::WTypeKind::ARC4DynamicArray &&
					method.args[i].wtype->kind() != awst::WTypeKind::ARC4StaticArray &&
					method.args[i].wtype->kind() != awst::WTypeKind::ARC4Struct &&
					method.args[i].wtype->kind() != awst::WTypeKind::ARC4Tuple)
				{
					// For uint64: use itob to convert to bytes
					if (method.args[i].wtype == awst::WType::uint64Type())
					{
						storeValue = makeIntrinsic("itob", {}, {argVar}, awst::WType::bytesType());
					}
					else if (method.args[i].wtype == awst::WType::biguintType())
					{
						// biguint is already bytes at runtime, just reinterpret
						auto cast = std::make_shared<awst::ReinterpretCast>();
						cast->sourceLocation = loc;
						cast->expr = argVar;
						cast->wtype = awst::WType::bytesType();
						storeValue = cast;
					}
				}

				auto store = makeIntrinsic("store", {static_cast<int>(i)}, {storeValue}, awst::WType::voidType());
				auto stmt = std::make_shared<awst::ExpressionStatement>();
				stmt->sourceLocation = loc;
				stmt->expr = store;
				body->body.push_back(stmt);
			}

			// 4. Return true (for bool) or default value (keeps group alive)
			auto ret = std::make_shared<awst::ReturnStatement>();
			ret->sourceLocation = loc;
			if (method.returnType == awst::WType::boolType())
			{
				ret->value = awst::makeBoolConstant(true, loc);
			}
			else if (method.returnType != awst::WType::voidType() && method.returnType != nullptr)
			{
				ret->value = buildDefaultExpression(loc, method.returnType);
			}
			body->body.push_back(ret);

			entry.body = body;
			orch->methods.push_back(std::move(entry));
		}

		// ─── Finish method: __finish_<name>() -> RetType ─────────────────
		// Body: assert flag matches this method, read result from last helper scratch, clear flag
		{
			std::string finishName = "__finish_" + method.memberName;

			awst::ContractMethod finish;
			finish.sourceLocation = method.sourceLocation;
			finish.returnType = method.returnType;
			finish.cref = orch->id;
			finish.memberName = finishName;

			auto body = std::make_shared<awst::Block>();
			body->sourceLocation = loc;

			// 1. assert(app_global_get("f") != "") — flag must be set
			{
				auto flag = makeIntrinsic("app_global_get", {}, {makeBytesKey("f")}, awst::WType::bytesType());
				auto empty = makeBytesHex({});
				body->body.push_back(makeAssertStmt(
					makeBytesCmp(flag, awst::EqualityComparison::Ne, empty),
					"orchestrator: no active call"
				));
			}

			// 2. Read result from previous txn's scratch slot 0: gload(GroupIndex-1, 0)
			// GroupIndex-1 is the last helper in the chain
			std::shared_ptr<awst::Expression> resultExpr;
			if (method.returnType != awst::WType::voidType() && method.returnType != nullptr)
			{
				auto groupIdx = makeIntrinsic("txn", {std::string("GroupIndex")}, {}, awst::WType::uint64Type());
				auto one = makeUint64("1");
				auto prevIdx = std::make_shared<awst::UInt64BinaryOperation>();
				prevIdx->sourceLocation = loc;
				prevIdx->wtype = awst::WType::uint64Type();
				prevIdx->left = groupIdx;
				prevIdx->op = awst::UInt64BinaryOperator::Sub;
				prevIdx->right = one;

				// gloads slot 0 from the previous txn
				auto gloadResult = makeIntrinsic("gloads", {0}, {prevIdx}, awst::WType::bytesType());

				// Cast bytes back to the return type
				if (method.returnType == awst::WType::boolType())
				{
					// btoi(gloads(prevIdx, 0)) != 0
					auto asUint = makeIntrinsic("btoi", {}, {gloadResult}, awst::WType::uint64Type());
					resultExpr = makeNumericCmp(asUint, awst::NumericComparison::Ne, makeUint64("0"));
				}
				else if (method.returnType == awst::WType::uint64Type())
				{
					resultExpr = makeIntrinsic("btoi", {}, {gloadResult}, awst::WType::uint64Type());
				}
				else if (method.returnType == awst::WType::biguintType())
				{
					auto cast = std::make_shared<awst::ReinterpretCast>();
					cast->sourceLocation = loc;
					cast->expr = gloadResult;
					cast->wtype = awst::WType::biguintType();
					resultExpr = cast;
				}
				else
				{
					// For bytes, structs, arrays: just use the raw bytes
					auto cast = std::make_shared<awst::ReinterpretCast>();
					cast->sourceLocation = loc;
					cast->expr = gloadResult;
					cast->wtype = method.returnType;
					resultExpr = cast;
				}
			}

			// 3. Clear flag: app_global_put("f", "")
			{
				auto empty = makeBytesHex({});
				auto put = makeIntrinsic("app_global_put", {}, {makeBytesKey("f"), empty}, awst::WType::voidType());
				auto stmt = std::make_shared<awst::ExpressionStatement>();
				stmt->sourceLocation = loc;
				stmt->expr = put;
				body->body.push_back(stmt);
			}

			// 4. Return the result
			auto ret = std::make_shared<awst::ReturnStatement>();
			ret->sourceLocation = loc;
			if (resultExpr)
				ret->value = resultExpr;
			body->body.push_back(ret);

			finish.body = body;

			awst::ARC4ABIMethodConfig abiConfig;
			abiConfig.sourceLocation = loc;
			abiConfig.allowedCompletionTypes = {0}; // NoOp
			abiConfig.create = 3; // Disallow
			abiConfig.name = finishName;
			abiConfig.readonly = false;
			finish.arc4MethodConfig = abiConfig;

			orch->methods.push_back(std::move(finish));
		}
	}

	// Add a bare create method so the orchestrator can be deployed
	{
		awst::ContractMethod bareCreate;
		bareCreate.sourceLocation = loc;
		bareCreate.returnType = awst::WType::voidType();
		bareCreate.cref = orch->id;
		bareCreate.memberName = "__bare_create__";

		auto body = std::make_shared<awst::Block>();
		body->sourceLocation = loc;

		// Initialize "f" flag to empty bytes so app_global_get returns bytes, not uint64
		{
			auto initPut = makeIntrinsic("app_global_put", {}, {makeBytesKey("f"), makeBytesHex({})}, awst::WType::voidType());
			auto stmt = std::make_shared<awst::ExpressionStatement>();
			stmt->sourceLocation = loc;
			stmt->expr = initPut;
			body->body.push_back(stmt);
		}

		auto ret = std::make_shared<awst::ReturnStatement>();
		ret->sourceLocation = loc;
		body->body.push_back(ret);
		bareCreate.body = body;

		awst::ARC4BareMethodConfig bareConfig;
		bareConfig.sourceLocation = loc;
		bareConfig.allowedCompletionTypes = {0}; // NoOp
		bareConfig.create = 2; // Require (create-only)
		bareCreate.arc4MethodConfig = bareConfig;

		orch->methods.push_back(std::move(bareCreate));
	}

	// Add an ABI auth stamp method so helpers can reference orchestrator in group calls.
	// Helpers validate that gtxn[0].ApplicationID == orchestrator, so any successful call works.
	{
		awst::ContractMethod authMethod;
		authMethod.sourceLocation = loc;
		authMethod.returnType = awst::WType::voidType();
		authMethod.cref = orch->id;
		authMethod.memberName = "__auth__";

		auto body = std::make_shared<awst::Block>();
		body->sourceLocation = loc;
		auto ret = std::make_shared<awst::ReturnStatement>();
		ret->sourceLocation = loc;
		body->body.push_back(ret);
		authMethod.body = body;

		awst::ARC4ABIMethodConfig abiConfig;
		abiConfig.sourceLocation = loc;
		abiConfig.allowedCompletionTypes = {0}; // NoOp
		abiConfig.create = 3; // Disallow
		abiConfig.name = "__auth__";
		abiConfig.readonly = true;
		authMethod.arc4MethodConfig = abiConfig;

		orch->methods.push_back(std::move(authMethod));
	}

	// Approval program: ARC4 router (same as helpers)
	{
		auto body = std::make_shared<awst::Block>();
		body->sourceLocation = loc;

		auto routerExpr = std::make_shared<awst::ARC4Router>();
		routerExpr->sourceLocation = loc;
		routerExpr->wtype = awst::WType::boolType();

		auto ret = std::make_shared<awst::ReturnStatement>();
		ret->sourceLocation = loc;
		ret->value = routerExpr;
		body->body.push_back(ret);

		orch->approvalProgram.sourceLocation = loc;
		orch->approvalProgram.cref = orch->id;
		orch->approvalProgram.memberName = "__puya_arc4_router__";
		orch->approvalProgram.returnType = awst::WType::boolType();
		orch->approvalProgram.body = body;
	}

	// Clear program: always approve
	orch->clearProgram = buildClearProgram(loc, orch->id);

	return orch;
}

std::shared_ptr<awst::Contract> ContractSplitter::buildHybridOrchestrator(
	awst::Contract const& _original,
	std::set<std::string> const& _delegatedMethods
)
{
	auto orch = std::make_shared<awst::Contract>();
	awst::SourceLocation loc = _original.sourceLocation;

	orch->id = _original.id;
	orch->name = _original.name;
	orch->description = "Hybrid orchestrator for " + _original.name +
		" — local methods + delegated stubs";
	orch->methodResolutionOrder = _original.methodResolutionOrder;
	orch->avmVersion = _original.avmVersion;
	orch->appState = _original.appState;

	// Need the "f" flag only if we have delegated methods
	bool hasDelegated = !_delegatedMethods.empty();
	if (hasDelegated)
	{
		auto makeKey = [&](std::string const& k) {
			return awst::makeUtf8BytesConstant(k, loc);
		};
		awst::AppStorageDefinition fDef;
		fDef.sourceLocation = loc;
		fDef.memberName = "f";
		fDef.storageKind = awst::AppStorageKind::AppGlobal;
		fDef.storageWType = awst::WType::bytesType();
		fDef.key = makeKey("f");
		orch->appState.push_back(fDef);
	}

	// AWST helper lambdas (same as in buildThinOrchestrator)
	auto makeIntrinsic = [&](
		std::string op,
		std::vector<std::variant<std::string, int>> imm,
		std::vector<std::shared_ptr<awst::Expression>> args,
		awst::WType const* type
	) {
		auto ic = std::make_shared<awst::IntrinsicCall>();
		ic->sourceLocation = loc;
		ic->opCode = std::move(op);
		ic->immediates = std::move(imm);
		ic->stackArgs = std::move(args);
		ic->wtype = type;
		return ic;
	};
	auto makeBytesKey = [&](std::string const& k) {
		return awst::makeUtf8BytesConstant(k, loc);
	};
	auto makeBytesHex = [&](std::vector<unsigned char> const& v) {
		return awst::makeBytesConstant(v, loc);
	};
	auto makeUint64 = [&](std::string const& v) {
		auto val = awst::makeIntegerConstant(v, loc);
		return val;
	};
	auto makeAssertExpr = [&](std::shared_ptr<awst::Expression> cond, std::string msg) {
		return awst::makeAssert(std::move(cond), loc, std::move(msg), awst::WType::boolType());
	};
	auto makeAssertStmt = [&](std::shared_ptr<awst::Expression> cond, std::string msg) {
		auto es = std::make_shared<awst::ExpressionStatement>();
		es->sourceLocation = loc;
		es->expr = makeAssertExpr(std::move(cond), std::move(msg));
		return es;
	};
	auto makeBytesCmp = [&](
		std::shared_ptr<awst::Expression> lhs,
		awst::EqualityComparison op,
		std::shared_ptr<awst::Expression> rhs
	) {
		auto cmp = std::make_shared<awst::BytesComparisonExpression>();
		cmp->sourceLocation = loc;
		cmp->wtype = awst::WType::boolType();
		cmp->lhs = std::move(lhs);
		cmp->op = op;
		cmp->rhs = std::move(rhs);
		return cmp;
	};
	auto makeNumericCmp = [&](
		std::shared_ptr<awst::Expression> lhs,
		awst::NumericComparison op,
		std::shared_ptr<awst::Expression> rhs
	) {
		auto cmp = std::make_shared<awst::NumericComparisonExpression>();
		cmp->sourceLocation = loc;
		cmp->wtype = awst::WType::boolType();
		cmp->lhs = std::move(lhs);
		cmp->op = op;
		cmp->rhs = std::move(rhs);
		return cmp;
	};
	auto makeVar = [&](std::string name, awst::WType const* type) {
		auto v = std::make_shared<awst::VarExpression>();
		v->sourceLocation = loc;
		v->wtype = type;
		v->name = std::move(name);
		return v;
	};

	for (auto const& method: _original.methods)
	{
		if (_delegatedMethods.count(method.memberName))
		{
			// ─── Delegated method: create stub + __finish pair ──────────
			// Entrypoint stub (same as buildThinOrchestrator)
			{
				awst::ContractMethod entry;
				entry.sourceLocation = method.sourceLocation;
				entry.args = method.args;
				entry.returnType = method.returnType;
				entry.cref = orch->id;
				entry.memberName = method.memberName;
				entry.pure = method.pure;
				entry.documentation = method.documentation;
				entry.arc4MethodConfig = method.arc4MethodConfig;

				auto body = std::make_shared<awst::Block>();
				body->sourceLocation = loc;

				// assert(app_global_get("f") == "")
				{
					auto flag = makeIntrinsic("app_global_get", {}, {makeBytesKey("f")}, awst::WType::bytesType());
					body->body.push_back(makeAssertStmt(
						makeBytesCmp(flag, awst::EqualityComparison::Eq, makeBytesHex({})),
						"orchestrator: reentrant call"
					));
				}

				// app_global_put("f", txna ApplicationArgs 0)
				{
					auto selector = makeIntrinsic("txna", {std::string("ApplicationArgs"), 0}, {}, awst::WType::bytesType());
					auto put = makeIntrinsic("app_global_put", {}, {makeBytesKey("f"), selector}, awst::WType::voidType());
					auto stmt = std::make_shared<awst::ExpressionStatement>();
					stmt->sourceLocation = loc;
					stmt->expr = put;
					body->body.push_back(stmt);
				}

				// Store args to scratch
				for (size_t i = 0; i < method.args.size(); ++i)
				{
					auto argVar = makeVar(method.args[i].name, method.args[i].wtype);
					std::shared_ptr<awst::Expression> storeValue = argVar;

					if (method.args[i].wtype != awst::WType::bytesType() &&
						method.args[i].wtype->kind() != awst::WTypeKind::Bytes &&
						method.args[i].wtype->kind() != awst::WTypeKind::ReferenceArray &&
						method.args[i].wtype->kind() != awst::WTypeKind::ARC4DynamicArray &&
						method.args[i].wtype->kind() != awst::WTypeKind::ARC4StaticArray &&
						method.args[i].wtype->kind() != awst::WTypeKind::ARC4Struct &&
						method.args[i].wtype->kind() != awst::WTypeKind::ARC4Tuple)
					{
						if (method.args[i].wtype == awst::WType::uint64Type())
							storeValue = makeIntrinsic("itob", {}, {argVar}, awst::WType::bytesType());
						else if (method.args[i].wtype == awst::WType::biguintType())
						{
							auto cast = std::make_shared<awst::ReinterpretCast>();
							cast->sourceLocation = loc;
							cast->expr = argVar;
							cast->wtype = awst::WType::bytesType();
							storeValue = cast;
						}
					}

					auto store = makeIntrinsic("store", {static_cast<int>(i)}, {storeValue}, awst::WType::voidType());
					auto stmt = std::make_shared<awst::ExpressionStatement>();
					stmt->sourceLocation = loc;
					stmt->expr = store;
					body->body.push_back(stmt);
				}

				// Return default value
				auto ret = std::make_shared<awst::ReturnStatement>();
				ret->sourceLocation = loc;
				if (method.returnType == awst::WType::boolType())
				{
					ret->value = awst::makeBoolConstant(true, loc);
				}
				else if (method.returnType != awst::WType::voidType() && method.returnType != nullptr)
					ret->value = buildDefaultExpression(loc, method.returnType);
				body->body.push_back(ret);

				entry.body = body;
				orch->methods.push_back(std::move(entry));
			}

			// __finish method
			{
				std::string finishName = "__finish_" + method.memberName;
				awst::ContractMethod finish;
				finish.sourceLocation = method.sourceLocation;
				finish.returnType = method.returnType;
				finish.cref = orch->id;
				finish.memberName = finishName;

				auto body = std::make_shared<awst::Block>();
				body->sourceLocation = loc;

				// assert(app_global_get("f") != "")
				{
					auto flag = makeIntrinsic("app_global_get", {}, {makeBytesKey("f")}, awst::WType::bytesType());
					body->body.push_back(makeAssertStmt(
						makeBytesCmp(flag, awst::EqualityComparison::Ne, makeBytesHex({})),
						"orchestrator: no active call"
					));
				}

				// Read result from previous txn
				std::shared_ptr<awst::Expression> resultExpr;
				if (method.returnType != awst::WType::voidType() && method.returnType != nullptr)
				{
					auto groupIdx = makeIntrinsic("txn", {std::string("GroupIndex")}, {}, awst::WType::uint64Type());
					auto prevIdx = std::make_shared<awst::UInt64BinaryOperation>();
					prevIdx->sourceLocation = loc;
					prevIdx->wtype = awst::WType::uint64Type();
					prevIdx->left = groupIdx;
					prevIdx->op = awst::UInt64BinaryOperator::Sub;
					prevIdx->right = makeUint64("1");

					auto gloadResult = makeIntrinsic("gloads", {0}, {prevIdx}, awst::WType::bytesType());

					if (method.returnType == awst::WType::boolType())
					{
						auto asUint = makeIntrinsic("btoi", {}, {gloadResult}, awst::WType::uint64Type());
						resultExpr = makeNumericCmp(asUint, awst::NumericComparison::Ne, makeUint64("0"));
					}
					else if (method.returnType == awst::WType::uint64Type())
						resultExpr = makeIntrinsic("btoi", {}, {gloadResult}, awst::WType::uint64Type());
					else if (method.returnType == awst::WType::biguintType())
					{
						auto cast = std::make_shared<awst::ReinterpretCast>();
						cast->sourceLocation = loc;
						cast->expr = gloadResult;
						cast->wtype = awst::WType::biguintType();
						resultExpr = cast;
					}
					else
					{
						auto cast = std::make_shared<awst::ReinterpretCast>();
						cast->sourceLocation = loc;
						cast->expr = gloadResult;
						cast->wtype = method.returnType;
						resultExpr = cast;
					}
				}

				// Clear flag
				{
					auto put = makeIntrinsic("app_global_put", {}, {makeBytesKey("f"), makeBytesHex({})}, awst::WType::voidType());
					auto stmt = std::make_shared<awst::ExpressionStatement>();
					stmt->sourceLocation = loc;
					stmt->expr = put;
					body->body.push_back(stmt);
				}

				auto ret = std::make_shared<awst::ReturnStatement>();
				ret->sourceLocation = loc;
				if (resultExpr)
					ret->value = resultExpr;
				body->body.push_back(ret);

				finish.body = body;

				awst::ARC4ABIMethodConfig abiConfig;
				abiConfig.sourceLocation = loc;
				abiConfig.allowedCompletionTypes = {0};
				abiConfig.create = 3;
				abiConfig.name = finishName;
				abiConfig.readonly = false;
				finish.arc4MethodConfig = abiConfig;

				orch->methods.push_back(std::move(finish));
			}
		}
		else
		{
			// ─── Local method: keep original implementation ─────────────
			awst::ContractMethod localMethod;
			localMethod.sourceLocation = method.sourceLocation;
			localMethod.args = method.args;
			localMethod.returnType = method.returnType;
			localMethod.cref = orch->id;
			localMethod.memberName = method.memberName;
			localMethod.pure = method.pure;
			localMethod.documentation = method.documentation;
			localMethod.arc4MethodConfig = method.arc4MethodConfig;
			localMethod.body = method.body; // Keep original body!
			orch->methods.push_back(std::move(localMethod));
		}
	}

	// Bare create method (initialize "f" flag if we have delegated methods)
	{
		awst::ContractMethod bareCreate;
		bareCreate.sourceLocation = loc;
		bareCreate.returnType = awst::WType::voidType();
		bareCreate.cref = orch->id;
		bareCreate.memberName = "__bare_create__";

		auto body = std::make_shared<awst::Block>();
		body->sourceLocation = loc;

		if (hasDelegated)
		{
			auto initPut = makeIntrinsic("app_global_put", {}, {makeBytesKey("f"), makeBytesHex({})}, awst::WType::voidType());
			auto stmt = std::make_shared<awst::ExpressionStatement>();
			stmt->sourceLocation = loc;
			stmt->expr = initPut;
			body->body.push_back(stmt);
		}

		// Copy original bare create body statements if present
		for (auto const& origMethod: _original.methods)
		{
			if (origMethod.memberName == "__bare_create__" && origMethod.body)
			{
				for (auto const& stmt: origMethod.body->body)
					body->body.push_back(stmt);
				break;
			}
		}

		auto ret = std::make_shared<awst::ReturnStatement>();
		ret->sourceLocation = loc;
		body->body.push_back(ret);
		bareCreate.body = body;

		awst::ARC4BareMethodConfig bareConfig;
		bareConfig.sourceLocation = loc;
		bareConfig.allowedCompletionTypes = {0};
		bareConfig.create = 2;
		bareCreate.arc4MethodConfig = bareConfig;

		orch->methods.push_back(std::move(bareCreate));
	}

	// Auth stamp method (for helper validation)
	if (hasDelegated)
	{
		awst::ContractMethod authMethod;
		authMethod.sourceLocation = loc;
		authMethod.returnType = awst::WType::voidType();
		authMethod.cref = orch->id;
		authMethod.memberName = "__auth__";

		auto body = std::make_shared<awst::Block>();
		body->sourceLocation = loc;
		auto ret = std::make_shared<awst::ReturnStatement>();
		ret->sourceLocation = loc;
		body->body.push_back(ret);
		authMethod.body = body;

		awst::ARC4ABIMethodConfig abiConfig;
		abiConfig.sourceLocation = loc;
		abiConfig.allowedCompletionTypes = {0};
		abiConfig.create = 3;
		abiConfig.name = "__auth__";
		abiConfig.readonly = true;
		authMethod.arc4MethodConfig = abiConfig;

		orch->methods.push_back(std::move(authMethod));
	}

	// Approval program: ARC4 router
	{
		auto body = std::make_shared<awst::Block>();
		body->sourceLocation = loc;

		auto routerExpr = std::make_shared<awst::ARC4Router>();
		routerExpr->sourceLocation = loc;
		routerExpr->wtype = awst::WType::boolType();

		auto ret = std::make_shared<awst::ReturnStatement>();
		ret->sourceLocation = loc;
		ret->value = routerExpr;
		body->body.push_back(ret);

		orch->approvalProgram.sourceLocation = loc;
		orch->approvalProgram.cref = orch->id;
		orch->approvalProgram.memberName = "__puya_arc4_router__";
		orch->approvalProgram.returnType = awst::WType::boolType();
		orch->approvalProgram.body = body;
	}

	// Clear program
	orch->clearProgram = buildClearProgram(loc, orch->id);

	return orch;
}

/// Build a default expression for a given type (recursively handles tuples/structs).
static std::shared_ptr<awst::Expression> buildDefaultExpression(
	awst::SourceLocation const& _loc,
	awst::WType const* _type
)
{
	if (_type == awst::WType::boolType())
	{
		return awst::makeBoolConstant(false, _loc);
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
		std::vector<uint8_t> bytes;
		// For fixed-size bytes types (bytes[N]), fill with N zero bytes
		if (auto const* bytesType = dynamic_cast<awst::BytesWType const*>(_type))
			if (bytesType->length().has_value())
				bytes.resize(*bytesType->length(), 0);
		return awst::makeBytesConstant(std::move(bytes), _loc, awst::BytesEncoding::Base16, _type);
	}
	if (_type == awst::WType::stringType())
	{
		auto val = std::make_shared<awst::StringConstant>();
		val->value = "";
		val->wtype = awst::WType::stringType();
		val->sourceLocation = _loc;
		return val;
	}
	if (_type->kind() == awst::WTypeKind::WTuple)
	{
		auto const* tupleType = dynamic_cast<awst::WTuple const*>(_type);
		if (tupleType)
		{
			auto tuple = std::make_shared<awst::TupleExpression>();
			tuple->sourceLocation = _loc;
			tuple->wtype = _type;
			for (auto const* elemType: tupleType->types())
				tuple->items.push_back(buildDefaultExpression(_loc, elemType));
			return tuple;
		}
	}
	if (_type->kind() == awst::WTypeKind::ARC4Struct ||
		_type->kind() == awst::WTypeKind::ARC4Tuple)
	{
		auto const* tupleType = dynamic_cast<awst::WTuple const*>(_type);
		if (tupleType)
		{
			// Build a NamedTupleExpression for structs or an ARC4-compatible tuple
			if (tupleType->names().has_value())
			{
				auto ns = std::make_shared<awst::NamedTupleExpression>();
				ns->sourceLocation = _loc;
				ns->wtype = _type;
				auto const& names = tupleType->names().value();
				for (size_t i = 0; i < tupleType->types().size(); ++i)
					ns->values[names[i]] = buildDefaultExpression(_loc, tupleType->types()[i]);
				return ns;
			}
			else
			{
				auto tuple = std::make_shared<awst::TupleExpression>();
				tuple->sourceLocation = _loc;
				tuple->wtype = _type;
				for (auto const* elemType: tupleType->types())
					tuple->items.push_back(buildDefaultExpression(_loc, elemType));
				return tuple;
			}
		}
	}
	if (_type->kind() == awst::WTypeKind::ReferenceArray)
	{
		auto const* ra = dynamic_cast<awst::ReferenceArray const*>(_type);
		if (ra)
		{
			auto arr = std::make_shared<awst::NewArray>();
			arr->sourceLocation = _loc;
			arr->wtype = _type;
			// For fixed-size arrays, populate with default elements
			if (ra->arraySize().has_value())
			{
				int64_t size = ra->arraySize().value();
				for (int64_t i = 0; i < size; ++i)
					arr->values.push_back(buildDefaultExpression(_loc, ra->elementType()));
			}
			return arr;
		}
	}
	if (_type->kind() == awst::WTypeKind::ARC4DynamicArray ||
		_type->kind() == awst::WTypeKind::ARC4StaticArray)
	{
		// Return empty bytes for ARC4 array types
		return awst::makeBytesConstant({}, _loc);
	}
	// Fallback: return 0 as biguint (most general numeric type)
	auto val = std::make_shared<awst::IntegerConstant>();
	val->value = "0";
	val->wtype = awst::WType::biguintType();
	val->sourceLocation = _loc;
	return val;
}

std::shared_ptr<awst::Block> ContractSplitter::buildStubBody(
	awst::SourceLocation const& _loc,
	awst::WType const* _returnType
)
{
	auto body = std::make_shared<awst::Block>();
	body->sourceLocation = _loc;

	auto ret = std::make_shared<awst::ReturnStatement>();
	ret->sourceLocation = _loc;

	if (_returnType == awst::WType::voidType() || _returnType == nullptr)
	{
		body->body.push_back(ret);
		return body;
	}

	ret->value = buildDefaultExpression(_loc, _returnType);
	body->body.push_back(ret);
	return body;
}

std::vector<std::shared_ptr<awst::Statement>> ContractSplitter::buildValidationBlock(
	awst::SourceLocation const& _loc
)
{
	std::vector<std::shared_ptr<awst::Statement>> stmts;

	// Helper lambdas
	auto makeIntrinsic = [&](
		std::string op,
		std::vector<std::variant<std::string, int>> imm,
		std::vector<std::shared_ptr<awst::Expression>> args,
		awst::WType const* type
	) {
		auto ic = std::make_shared<awst::IntrinsicCall>();
		ic->sourceLocation = _loc;
		ic->opCode = std::move(op);
		ic->immediates = std::move(imm);
		ic->stackArgs = std::move(args);
		ic->wtype = type;
		return ic;
	};
	auto makeBytesKey = [&](std::string const& k) {
		return awst::makeUtf8BytesConstant(k, _loc);
	};
	auto makeUint64 = [&](std::string const& v) {
		auto val = awst::makeIntegerConstant(v, _loc);
		return val;
	};
	auto makeAssert = [&](std::shared_ptr<awst::Expression> cond, std::string msg) {
		auto es = std::make_shared<awst::ExpressionStatement>();
		es->sourceLocation = _loc;
		es->expr = awst::makeAssert(std::move(cond), _loc, std::move(msg), awst::WType::boolType());
		return es;
	};
	auto makeNumericCmp = [&](
		std::shared_ptr<awst::Expression> lhs,
		awst::NumericComparison op,
		std::shared_ptr<awst::Expression> rhs
	) {
		auto cmp = std::make_shared<awst::NumericComparisonExpression>();
		cmp->sourceLocation = _loc;
		cmp->wtype = awst::WType::boolType();
		cmp->lhs = std::move(lhs);
		cmp->op = op;
		cmp->rhs = std::move(rhs);
		return cmp;
	};
	auto makeBytesCmp = [&](
		std::shared_ptr<awst::Expression> lhs,
		awst::EqualityComparison op,
		std::shared_ptr<awst::Expression> rhs
	) {
		auto cmp = std::make_shared<awst::BytesComparisonExpression>();
		cmp->sourceLocation = _loc;
		cmp->wtype = awst::WType::boolType();
		cmp->lhs = std::move(lhs);
		cmp->op = op;
		cmp->rhs = std::move(rhs);
		return cmp;
	};

	// All validation is wrapped in: if (app_global_get("o") > 0) { ... }
	// This makes validation opt-in per deployment. Deploying with orch_app_id=0
	// disables checks (for testing). Production deploys set orch_app_id > 0.
	auto orchId = makeIntrinsic("app_global_get", {}, {makeBytesKey("o")}, awst::WType::uint64Type());
	auto outerCond = makeNumericCmp(orchId, awst::NumericComparison::Gt, makeUint64("0"));

	auto outerBody = std::make_shared<awst::Block>();
	outerBody->sourceLocation = _loc;

	// 1. assert(global GroupSize >= 2)
	{
		auto groupSize = makeIntrinsic("global", {std::string("GroupSize")}, {}, awst::WType::uint64Type());
		outerBody->body.push_back(makeAssert(
			makeNumericCmp(groupSize, awst::NumericComparison::Gte, makeUint64("2")),
			"helper: must be called in group"
		));
	}

	// 2. assert(gtxn 0 ApplicationID == app_global_get("o"))
	{
		auto gtxn0AppId = makeIntrinsic("gtxn", {0, std::string("ApplicationID")}, {}, awst::WType::uint64Type());
		auto orchIdInner = makeIntrinsic("app_global_get", {}, {makeBytesKey("o")}, awst::WType::uint64Type());
		outerBody->body.push_back(makeAssert(
			makeNumericCmp(gtxn0AppId, awst::NumericComparison::Eq, orchIdInner),
			"helper: unauthorized caller"
		));
	}

	// 3. assert(gtxn 0 Sender == txn Sender)
	{
		auto gtxn0Sender = makeIntrinsic("gtxn", {0, std::string("Sender")}, {}, awst::WType::bytesType());
		auto txnSender = makeIntrinsic("txn", {std::string("Sender")}, {}, awst::WType::bytesType());
		outerBody->body.push_back(makeAssert(
			makeBytesCmp(gtxn0Sender, awst::EqualityComparison::Eq, txnSender),
			"helper: sender mismatch"
		));
	}

	// 4. Conditional: if (app_global_get("p") > 0) { check prev chunk }
	{
		auto prevChunkId = makeIntrinsic("app_global_get", {}, {makeBytesKey("p")}, awst::WType::uint64Type());
		auto cond = makeNumericCmp(prevChunkId, awst::NumericComparison::Gt, makeUint64("0"));

		auto ifBody = std::make_shared<awst::Block>();
		ifBody->sourceLocation = _loc;

		// assert(gtxns (txn GroupIndex - 1) ApplicationID == app_global_get("p"))
		{
			auto groupIdx = makeIntrinsic("txn", {std::string("GroupIndex")}, {}, awst::WType::uint64Type());
			auto one = makeUint64("1");
			auto prevIdx = std::make_shared<awst::UInt64BinaryOperation>();
			prevIdx->sourceLocation = _loc;
			prevIdx->wtype = awst::WType::uint64Type();
			prevIdx->left = groupIdx;
			prevIdx->op = awst::UInt64BinaryOperator::Sub;
			prevIdx->right = one;

			auto prevAppId = makeIntrinsic("gtxns", {std::string("ApplicationID")}, {prevIdx}, awst::WType::uint64Type());
			auto expectedPrev = makeIntrinsic("app_global_get", {}, {makeBytesKey("p")}, awst::WType::uint64Type());
			ifBody->body.push_back(makeAssert(
				makeNumericCmp(prevAppId, awst::NumericComparison::Eq, expectedPrev),
				"helper: wrong prev chunk"
			));
		}

		// assert(gtxnsa ApplicationArgs 0 [GroupIndex-1] == app_global_get("s"))
		{
			auto groupIdx = makeIntrinsic("txn", {std::string("GroupIndex")}, {}, awst::WType::uint64Type());
			auto one = makeUint64("1");
			auto prevIdx = std::make_shared<awst::UInt64BinaryOperation>();
			prevIdx->sourceLocation = _loc;
			prevIdx->wtype = awst::WType::uint64Type();
			prevIdx->left = groupIdx;
			prevIdx->op = awst::UInt64BinaryOperator::Sub;
			prevIdx->right = one;

			auto prevArgs0 = makeIntrinsic("gtxnsa", {std::string("ApplicationArgs"), 0}, {prevIdx}, awst::WType::bytesType());
			auto expectedSel = makeIntrinsic("app_global_get", {}, {makeBytesKey("s")}, awst::WType::bytesType());
			ifBody->body.push_back(makeAssert(
				makeBytesCmp(prevArgs0, awst::EqualityComparison::Eq, expectedSel),
				"helper: wrong prev method"
			));
		}

		auto ifElse = std::make_shared<awst::IfElse>();
		ifElse->sourceLocation = _loc;
		ifElse->condition = cond;
		ifElse->ifBranch = ifBody;
		outerBody->body.push_back(ifElse);
	}

	auto outerIf = std::make_shared<awst::IfElse>();
	outerIf->sourceLocation = _loc;
	outerIf->condition = outerCond;
	outerIf->ifBranch = outerBody;
	stmts.push_back(outerIf);

	return stmts;
}

awst::ContractMethod ContractSplitter::buildClearProgram(
	awst::SourceLocation const& _loc,
	std::string const& _cref
)
{
	awst::ContractMethod clearProg;
	clearProg.sourceLocation = _loc;
	clearProg.cref = _cref;
	clearProg.memberName = "clear_state_program";
	clearProg.returnType = awst::WType::boolType();

	auto body = std::make_shared<awst::Block>();
	body->sourceLocation = _loc;

	auto returnStmt = std::make_shared<awst::ReturnStatement>();
	returnStmt->sourceLocation = _loc;
	returnStmt->value = awst::makeBoolConstant(true, _loc);
	body->body.push_back(returnStmt);

	clearProg.body = body;
	return clearProg;
}

} // namespace puyasol::splitter
