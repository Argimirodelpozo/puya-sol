#include "splitter/ContractSplitter.h"
#include "awst/WType.h"
#include "Logger.h"

#include <algorithm>
#include <queue>
#include <sstream>

namespace puyasol::splitter
{

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

	// Build subroutine name→node map
	auto subMap = buildSubroutineMap(_roots);

	// Also build ID→name map for dependency scanning
	std::map<std::string, std::string> idToName;
	for (auto const& [name, sub]: subMap)
		idToName[sub->id] = name;

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
		for (auto const& funcName: assignedFuncs)
		{
			auto it = subMap.find(funcName);
			if (it != subMap.end())
				helperAWST.roots.push_back(it->second);
		}

		// Add the Contract node
		helperAWST.roots.push_back(helper);

		result.contracts.push_back(std::move(helperAWST));

		logger.info("  Helper " + std::to_string(i) + ": " + helper->id +
			" (" + std::to_string(assignedFuncs.size()) + " subroutines)");
	}

	// ─── Create thin orchestrator (partition 0) ───────────────────────────
	// The thin orchestrator exposes the original ABI interface but with stub
	// method bodies (return default values). This keeps it small (~100-200
	// TEAL lines) instead of duplicating all subroutines (~17K+ lines).
	// The actual computation happens in the helper contracts.
	{
		auto thinOrch = buildThinOrchestrator(*_original);

		ContractAWST orchAWST;
		orchAWST.contractId = thinOrch->id;
		orchAWST.contractName = thinOrch->name;

		// No subroutines — the thin orchestrator only has stub methods
		orchAWST.roots.push_back(thinOrch);

		result.contracts.push_back(std::move(orchAWST));

		logger.info("  Orchestrator (thin): " + _original->id +
			" (stub methods only, no subroutines)");
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

	// No app state — helpers are stateless

	// Wrap each assigned subroutine as an ABI method on the helper.
	// This makes the subroutines callable and ensures puya includes them.
	for (auto const& funcName: _functionNames)
	{
		auto it = _subMap.find(funcName);
		if (it == _subMap.end())
			continue;

		auto const& sub = it->second;

		awst::ContractMethod method;
		method.sourceLocation = sub->sourceLocation;
		method.args = sub->args;
		method.returnType = sub->returnType;
		method.cref = helper->id;
		method.memberName = sub->name;
		method.pure = sub->pure;
		method.documentation = sub->documentation;

		// Build method body: call the subroutine and return its result
		auto body = std::make_shared<awst::Block>();
		body->sourceLocation = sub->sourceLocation;

		auto callExpr = std::make_shared<awst::SubroutineCallExpression>();
		callExpr->sourceLocation = sub->sourceLocation;
		callExpr->wtype = sub->returnType;
		callExpr->target = awst::SubroutineID{sub->id};

		// Pass all args through
		for (auto const& arg: sub->args)
		{
			auto varExpr = std::make_shared<awst::VarExpression>();
			varExpr->sourceLocation = arg.sourceLocation;
			varExpr->wtype = arg.wtype;
			varExpr->name = arg.name;
			callExpr->args.push_back(awst::CallArg{arg.name, varExpr});
		}

		if (sub->returnType != awst::WType::voidType())
		{
			auto ret = std::make_shared<awst::ReturnStatement>();
			ret->sourceLocation = sub->sourceLocation;
			ret->value = callExpr;
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

	// Add a bare create method so the helper can be deployed
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
	orch->description = "Thin orchestrator for " + _original.name +
		" — stub methods only, computation in helpers";
	orch->methodResolutionOrder = _original.methodResolutionOrder;
	orch->avmVersion = _original.avmVersion;

	// Copy app state declarations (so ARC56 metadata is preserved)
	orch->appState = _original.appState;

	// Stub each original method: same ABI signature, but body returns default value
	for (auto const& method: _original.methods)
	{
		awst::ContractMethod stub;
		stub.sourceLocation = method.sourceLocation;
		stub.args = method.args;
		stub.returnType = method.returnType;
		stub.cref = orch->id;
		stub.memberName = method.memberName;
		stub.pure = method.pure;
		stub.documentation = method.documentation;
		stub.arc4MethodConfig = method.arc4MethodConfig;

		// Build stub body: return default value for the return type
		stub.body = buildStubBody(method.sourceLocation, method.returnType);

		orch->methods.push_back(std::move(stub));
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

/// Build a default expression for a given type (recursively handles tuples/structs).
static std::shared_ptr<awst::Expression> buildDefaultExpression(
	awst::SourceLocation const& _loc,
	awst::WType const* _type
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
		return val;
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
				int size = ra->arraySize().value();
				for (int i = 0; i < size; ++i)
					arr->values.push_back(buildDefaultExpression(_loc, ra->elementType()));
			}
			return arr;
		}
	}
	if (_type->kind() == awst::WTypeKind::ARC4DynamicArray ||
		_type->kind() == awst::WTypeKind::ARC4StaticArray)
	{
		// Return empty bytes for ARC4 array types
		auto val = std::make_shared<awst::BytesConstant>();
		val->wtype = awst::WType::bytesType();
		val->sourceLocation = _loc;
		val->encoding = awst::BytesEncoding::Base16;
		return val;
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
	auto trueConst = std::make_shared<awst::BoolConstant>();
	trueConst->value = true;
	trueConst->wtype = awst::WType::boolType();
	trueConst->sourceLocation = _loc;
	returnStmt->value = trueConst;
	body->body.push_back(returnStmt);

	clearProg.body = body;
	return clearProg;
}

} // namespace puyasol::splitter
