#include "splitter/CallGraphAnalyzer.h"
#include "Logger.h"

#include <algorithm>
#include <queue>
#include <sstream>

namespace puyasol::splitter
{

CallGraphAnalyzer::SplitRecommendation CallGraphAnalyzer::analyze(
	awst::Contract const& _contract,
	std::vector<std::shared_ptr<awst::RootNode>> const& _subroutines,
	SizeEstimator::Estimate const& _sizes,
	std::set<std::string> const& _rewrittenFunctions,
	std::set<std::string> const& _mutableSharedFunctions
)
{
	SplitRecommendation result;

	// Build the call graph
	buildCallGraph(_contract, _subroutines);

	// Log the call graph
	auto& logger = Logger::instance();
	logger.debug("Call graph analysis:");
	for (auto const& [name, info]: m_functions)
	{
		std::ostringstream oss;
		oss << "  " << name << " (size=" << info.estimatedSize
			<< ", state=" << (info.hasStateAccess ? "yes" : "no")
			<< ", method=" << (info.isContractMethod ? "yes" : "no") << ")";
		if (!info.calls.empty())
		{
			oss << " -> [";
			bool first = true;
			for (auto const& callee: info.calls)
			{
				if (!first)
					oss << ", ";
				oss << callee;
				first = false;
			}
			oss << "]";
		}
		logger.debug(oss.str());
	}

	// Update sizes from the estimate
	for (auto& [name, info]: m_functions)
	{
		auto it = _sizes.methodSizes.find(name);
		if (it != _sizes.methodSizes.end())
			info.estimatedSize = it->second;
	}

	// Check if splitting is needed
	if (_sizes.estimatedBytes <= SizeEstimator::SplitThresholdBytes)
	{
		result.shouldSplit = false;
		logger.info("Contract fits within AVM limits (" +
			std::to_string(_sizes.estimatedBytes) + " estimated bytes)");
		return result;
	}

	result.shouldSplit = true;
	logger.info("Contract exceeds AVM limit (" +
		std::to_string(_sizes.estimatedBytes) + " estimated bytes > " +
		std::to_string(SizeEstimator::AVMMaxBytes) + " max)");

	// Perform bin-packing with tighter threshold for better distribution.
	// With the 3x byte multiplier in SizeEstimator, target ~1300 instruction units
	// per partition to stay under 8KB after shared dep duplication and ARC4 overhead.
	// The /6 divisor accounts for biguint-heavy code (wrapping patterns) having
	// higher actual-to-estimated byte ratios.
	// Non-split code uses a higher threshold since ABI codec overhead is smaller.
	size_t packingThreshold = _rewrittenFunctions.empty()
		? SizeEstimator::SplitThresholdBytes / 3
		: SizeEstimator::SplitThresholdBytes / 6;
	auto partitions = binPack(_sizes, packingThreshold, _rewrittenFunctions, _mutableSharedFunctions);

	if (partitions.empty())
	{
		// Couldn't partition — single-method too large
		logger.warning("Unable to partition contract — individual methods may exceed AVM limit");
		result.shouldSplit = false;
		return result;
	}

	result.partitions = std::move(partitions);

	// Identify shared utilities (functions needed by multiple HELPER partitions).
	// Skip partition 0 (orchestrator) — it gets stub method bodies, not actual
	// subroutine code, so its transitive deps should not inflate use counts.
	std::map<std::string, int> partitionUseCount;
	for (size_t i = 1; i < result.partitions.size(); ++i)
	{
		std::set<std::string> allDeps;
		for (auto const& funcName: result.partitions[i])
		{
			auto deps = transitiveDeps(funcName);
			allDeps.insert(deps.begin(), deps.end());
		}
		for (auto const& dep: allDeps)
			partitionUseCount[dep]++;
	}

	for (auto const& [name, count]: partitionUseCount)
	{
		if (count > 1)
		{
			// This function is needed by multiple helper partitions — share it
			auto it = m_functions.find(name);
			if (it != m_functions.end() && !it->second.isContractMethod)
				result.sharedUtilities.push_back(name);
		}
	}

	// Ensure direct callees across partition boundaries are shared.
	// This catches functions used by only ONE partition but assigned to
	// a DIFFERENT partition (not caught by multi-partition detection above).
	{
		std::set<std::string> orchestratorFuncs(
			result.partitions[0].begin(), result.partitions[0].end());
		std::set<std::string> sharedSet(
			result.sharedUtilities.begin(), result.sharedUtilities.end());

		for (size_t i = 1; i < result.partitions.size(); ++i)
		{
			std::set<std::string> partFuncs(
				result.partitions[i].begin(), result.partitions[i].end());

			for (auto const& funcName: result.partitions[i])
			{
				auto it = m_functions.find(funcName);
				if (it == m_functions.end())
					continue;

				// Check DIRECT callees only (not transitive deps)
				for (auto const& callee: it->second.calls)
				{
					if (!partFuncs.count(callee) && !sharedSet.count(callee) &&
						!orchestratorFuncs.count(callee))
					{
						auto cit = m_functions.find(callee);
						if (cit != m_functions.end() && !cit->second.isContractMethod)
						{
							result.sharedUtilities.push_back(callee);
							sharedSet.insert(callee);
							logger.debug("Force-sharing '" + callee +
								"' (direct cross-partition callee)");
						}
					}
				}
			}
		}
	}

	// Estimate cross-partition data (rough: 32 bytes per inter-partition edge)
	size_t crossEdges = 0;
	for (size_t i = 0; i < result.partitions.size(); ++i)
	{
		std::set<std::string> partFuncs(result.partitions[i].begin(), result.partitions[i].end());
		for (auto const& funcName: result.partitions[i])
		{
			auto it = m_functions.find(funcName);
			if (it == m_functions.end())
				continue;
			for (auto const& callee: it->second.calls)
			{
				if (partFuncs.find(callee) == partFuncs.end())
					crossEdges++;
			}
		}
	}
	result.estimatedCrossPartitionDataBytes = crossEdges * 32;

	// Log recommendations
	logger.info("Split recommendation: " + std::to_string(result.partitions.size()) + " partitions");
	for (size_t i = 0; i < result.partitions.size(); ++i)
	{
		std::ostringstream oss;
		oss << "  Partition " << i << ": [";
		bool first = true;
		for (auto const& name: result.partitions[i])
		{
			if (!first)
				oss << ", ";
			oss << name;
			first = false;
		}
		oss << "]";
		logger.info(oss.str());
	}

	if (!result.sharedUtilities.empty())
	{
		std::ostringstream oss;
		oss << "  Shared utilities (duplicated): [";
		bool first = true;
		for (auto const& name: result.sharedUtilities)
		{
			if (!first)
				oss << ", ";
			oss << name;
			first = false;
		}
		oss << "]";
		logger.info(oss.str());
	}

	return result;
}

void CallGraphAnalyzer::buildCallGraph(
	awst::Contract const& _contract,
	std::vector<std::shared_ptr<awst::RootNode>> const& _subroutines
)
{
	m_functions.clear();
	m_subroutineIdToName.clear();

	// Register subroutines first (for ID→name mapping)
	for (auto const& root: _subroutines)
	{
		if (auto const* sub = dynamic_cast<awst::Subroutine const*>(root.get()))
		{
			m_subroutineIdToName[sub->id] = sub->name;
			auto& info = m_functions[sub->name];
			info.name = sub->name;
			info.isContractMethod = false;
			info.isARC4Method = false;
		}
	}

	// Register contract methods
	for (auto const& method: _contract.methods)
	{
		auto& info = m_functions[method.memberName];
		info.name = method.memberName;
		info.isContractMethod = true;
		info.isARC4Method = method.arc4MethodConfig.has_value();
	}

	// Scan approval program
	{
		auto& info = m_functions["__approval__"];
		info.name = "__approval__";
		info.isContractMethod = true;
		if (_contract.approvalProgram.body)
			scanBlock(*_contract.approvalProgram.body, "__approval__");
	}

	// Scan clear program
	{
		auto& info = m_functions["__clear__"];
		info.name = "__clear__";
		info.isContractMethod = true;
		if (_contract.clearProgram.body)
			scanBlock(*_contract.clearProgram.body, "__clear__");
	}

	// Scan contract methods
	for (auto const& method: _contract.methods)
	{
		if (method.body)
			scanBlock(*method.body, method.memberName);
	}

	// Scan subroutines
	for (auto const& root: _subroutines)
	{
		if (auto const* sub = dynamic_cast<awst::Subroutine const*>(root.get()))
		{
			if (sub->body)
				scanBlock(*sub->body, sub->name);
		}
	}

	// Build reverse edges (calledBy)
	for (auto const& [name, info]: m_functions)
	{
		for (auto const& callee: info.calls)
		{
			auto it = m_functions.find(callee);
			if (it != m_functions.end())
				it->second.calledBy.insert(name);
		}
	}
}

void CallGraphAnalyzer::scanExpression(awst::Expression const& _expr, std::string const& _caller)
{
	std::string type = _expr.nodeType();

	// Detect subroutine calls
	if (type == "SubroutineCallExpression")
	{
		auto const& call = static_cast<awst::SubroutineCallExpression const&>(_expr);

		std::string targetName;
		if (auto const* sid = std::get_if<awst::SubroutineID>(&call.target))
		{
			auto it = m_subroutineIdToName.find(sid->target);
			if (it != m_subroutineIdToName.end())
				targetName = it->second;
			else
				targetName = sid->target; // use raw ID as fallback
		}
		else if (auto const* imt = std::get_if<awst::InstanceMethodTarget>(&call.target))
			targetName = imt->memberName;
		else if (auto const* ismt = std::get_if<awst::InstanceSuperMethodTarget>(&call.target))
			targetName = ismt->memberName;
		else if (auto const* cmt = std::get_if<awst::ContractMethodTarget>(&call.target))
			targetName = cmt->memberName;

		if (!targetName.empty())
		{
			m_functions[_caller].calls.insert(targetName);
			// Ensure target exists in the map
			if (m_functions.find(targetName) == m_functions.end())
			{
				auto& info = m_functions[targetName];
				info.name = targetName;
			}
		}

		// Scan arguments
		for (auto const& arg: call.args)
			if (arg.value)
				scanExpression(*arg.value, _caller);
		return;
	}

	// Detect state access
	if (type == "StateGet" || type == "StateExists" || type == "StateDelete" ||
		type == "StateGetEx" || type == "AppStateExpression" ||
		type == "AppAccountStateExpression" || type == "BoxValueExpression" ||
		type == "BoxPrefixedKeyExpression")
	{
		m_functions[_caller].hasStateAccess = true;
	}

	// Recursively scan child expressions
	if (type == "UInt64BinaryOperation")
	{
		auto const& op = static_cast<awst::UInt64BinaryOperation const&>(_expr);
		if (op.left) scanExpression(*op.left, _caller);
		if (op.right) scanExpression(*op.right, _caller);
	}
	else if (type == "BigUIntBinaryOperation")
	{
		auto const& op = static_cast<awst::BigUIntBinaryOperation const&>(_expr);
		if (op.left) scanExpression(*op.left, _caller);
		if (op.right) scanExpression(*op.right, _caller);
	}
	else if (type == "BytesBinaryOperation")
	{
		auto const& op = static_cast<awst::BytesBinaryOperation const&>(_expr);
		if (op.left) scanExpression(*op.left, _caller);
		if (op.right) scanExpression(*op.right, _caller);
	}
	else if (type == "BytesUnaryOperation")
	{
		auto const& op = static_cast<awst::BytesUnaryOperation const&>(_expr);
		if (op.expr) scanExpression(*op.expr, _caller);
	}
	else if (type == "NumericComparisonExpression")
	{
		auto const& cmp = static_cast<awst::NumericComparisonExpression const&>(_expr);
		if (cmp.lhs) scanExpression(*cmp.lhs, _caller);
		if (cmp.rhs) scanExpression(*cmp.rhs, _caller);
	}
	else if (type == "BytesComparisonExpression")
	{
		auto const& cmp = static_cast<awst::BytesComparisonExpression const&>(_expr);
		if (cmp.lhs) scanExpression(*cmp.lhs, _caller);
		if (cmp.rhs) scanExpression(*cmp.rhs, _caller);
	}
	else if (type == "BooleanBinaryOperation")
	{
		auto const& op = static_cast<awst::BooleanBinaryOperation const&>(_expr);
		if (op.left) scanExpression(*op.left, _caller);
		if (op.right) scanExpression(*op.right, _caller);
	}
	else if (type == "Not")
	{
		auto const& n = static_cast<awst::Not const&>(_expr);
		if (n.expr) scanExpression(*n.expr, _caller);
	}
	else if (type == "AssertExpression")
	{
		auto const& a = static_cast<awst::AssertExpression const&>(_expr);
		if (a.condition) scanExpression(*a.condition, _caller);
	}
	else if (type == "AssignmentExpression")
	{
		auto const& a = static_cast<awst::AssignmentExpression const&>(_expr);
		if (a.target) scanExpression(*a.target, _caller);
		if (a.value) scanExpression(*a.value, _caller);
	}
	else if (type == "ConditionalExpression")
	{
		auto const& c = static_cast<awst::ConditionalExpression const&>(_expr);
		if (c.condition) scanExpression(*c.condition, _caller);
		if (c.trueExpr) scanExpression(*c.trueExpr, _caller);
		if (c.falseExpr) scanExpression(*c.falseExpr, _caller);
	}
	else if (type == "IntrinsicCall")
	{
		auto const& ic = static_cast<awst::IntrinsicCall const&>(_expr);
		for (auto const& arg: ic.stackArgs)
			if (arg) scanExpression(*arg, _caller);
	}
	else if (type == "PuyaLibCall")
	{
		auto const& plc = static_cast<awst::PuyaLibCall const&>(_expr);
		for (auto const& arg: plc.args)
			if (arg.value) scanExpression(*arg.value, _caller);
	}
	else if (type == "FieldExpression")
	{
		auto const& f = static_cast<awst::FieldExpression const&>(_expr);
		if (f.base) scanExpression(*f.base, _caller);
	}
	else if (type == "IndexExpression")
	{
		auto const& idx = static_cast<awst::IndexExpression const&>(_expr);
		if (idx.base) scanExpression(*idx.base, _caller);
		if (idx.index) scanExpression(*idx.index, _caller);
	}
	else if (type == "TupleExpression")
	{
		auto const& t = static_cast<awst::TupleExpression const&>(_expr);
		for (auto const& item: t.items)
			if (item) scanExpression(*item, _caller);
	}
	else if (type == "TupleItemExpression")
	{
		auto const& ti = static_cast<awst::TupleItemExpression const&>(_expr);
		if (ti.base) scanExpression(*ti.base, _caller);
	}
	else if (type == "ARC4Encode")
	{
		auto const& e = static_cast<awst::ARC4Encode const&>(_expr);
		if (e.value) scanExpression(*e.value, _caller);
	}
	else if (type == "ARC4Decode")
	{
		auto const& d = static_cast<awst::ARC4Decode const&>(_expr);
		if (d.value) scanExpression(*d.value, _caller);
	}
	else if (type == "ReinterpretCast")
	{
		auto const& rc = static_cast<awst::ReinterpretCast const&>(_expr);
		if (rc.expr) scanExpression(*rc.expr, _caller);
	}
	else if (type == "Copy")
	{
		auto const& c = static_cast<awst::Copy const&>(_expr);
		if (c.value) scanExpression(*c.value, _caller);
	}
	else if (type == "SingleEvaluation")
	{
		auto const& se = static_cast<awst::SingleEvaluation const&>(_expr);
		if (se.source) scanExpression(*se.source, _caller);
	}
	else if (type == "CheckedMaybe")
	{
		auto const& cm = static_cast<awst::CheckedMaybe const&>(_expr);
		if (cm.expr) scanExpression(*cm.expr, _caller);
	}
	else if (type == "NewArray")
	{
		auto const& na = static_cast<awst::NewArray const&>(_expr);
		for (auto const& v: na.values)
			if (v) scanExpression(*v, _caller);
	}
	else if (type == "ArrayLength")
	{
		auto const& al = static_cast<awst::ArrayLength const&>(_expr);
		if (al.array) scanExpression(*al.array, _caller);
	}
	else if (type == "ArrayPop")
	{
		auto const& ap = static_cast<awst::ArrayPop const&>(_expr);
		if (ap.base) scanExpression(*ap.base, _caller);
	}
	else if (type == "ArrayConcat")
	{
		auto const& ac = static_cast<awst::ArrayConcat const&>(_expr);
		if (ac.left) scanExpression(*ac.left, _caller);
		if (ac.right) scanExpression(*ac.right, _caller);
	}
	else if (type == "ArrayExtend")
	{
		auto const& ae = static_cast<awst::ArrayExtend const&>(_expr);
		if (ae.base) scanExpression(*ae.base, _caller);
		if (ae.other) scanExpression(*ae.other, _caller);
	}
	else if (type == "StateGet")
	{
		auto const& sg = static_cast<awst::StateGet const&>(_expr);
		if (sg.field) scanExpression(*sg.field, _caller);
		if (sg.defaultValue) scanExpression(*sg.defaultValue, _caller);
	}
	else if (type == "StateExists")
	{
		auto const& se = static_cast<awst::StateExists const&>(_expr);
		if (se.field) scanExpression(*se.field, _caller);
	}
	else if (type == "StateDelete")
	{
		auto const& sd = static_cast<awst::StateDelete const&>(_expr);
		if (sd.field) scanExpression(*sd.field, _caller);
	}
	else if (type == "StateGetEx")
	{
		auto const& sge = static_cast<awst::StateGetEx const&>(_expr);
		if (sge.field) scanExpression(*sge.field, _caller);
	}
	else if (type == "BoxPrefixedKeyExpression")
	{
		auto const& bpk = static_cast<awst::BoxPrefixedKeyExpression const&>(_expr);
		if (bpk.prefix) scanExpression(*bpk.prefix, _caller);
		if (bpk.key) scanExpression(*bpk.key, _caller);
	}
	else if (type == "BoxValueExpression")
	{
		auto const& bve = static_cast<awst::BoxValueExpression const&>(_expr);
		if (bve.key) scanExpression(*bve.key, _caller);
	}
	else if (type == "NewStruct")
	{
		auto const& ns = static_cast<awst::NewStruct const&>(_expr);
		for (auto const& [_, val]: ns.values)
			if (val) scanExpression(*val, _caller);
	}
	else if (type == "NamedTupleExpression")
	{
		auto const& nt = static_cast<awst::NamedTupleExpression const&>(_expr);
		for (auto const& [_, val]: nt.values)
			if (val) scanExpression(*val, _caller);
	}
	else if (type == "Emit")
	{
		auto const& e = static_cast<awst::Emit const&>(_expr);
		if (e.value) scanExpression(*e.value, _caller);
	}
	else if (type == "CreateInnerTransaction")
	{
		auto const& cit = static_cast<awst::CreateInnerTransaction const&>(_expr);
		for (auto const& [_, val]: cit.fields)
			if (val) scanExpression(*val, _caller);
	}
	else if (type == "SubmitInnerTransaction")
	{
		auto const& sit = static_cast<awst::SubmitInnerTransaction const&>(_expr);
		for (auto const& itxn: sit.itxns)
			if (itxn) scanExpression(*itxn, _caller);
	}
	else if (type == "InnerTransactionField")
	{
		auto const& itf = static_cast<awst::InnerTransactionField const&>(_expr);
		if (itf.itxn) scanExpression(*itf.itxn, _caller);
	}
	else if (type == "CommaExpression")
	{
		auto const& ce = static_cast<awst::CommaExpression const&>(_expr);
		for (auto const& e: ce.expressions)
			if (e) scanExpression(*e, _caller);
	}
}

void CallGraphAnalyzer::scanStatement(awst::Statement const& _stmt, std::string const& _caller)
{
	std::string type = _stmt.nodeType();

	if (type == "Block")
	{
		scanBlock(static_cast<awst::Block const&>(_stmt), _caller);
	}
	else if (type == "ExpressionStatement")
	{
		auto const& es = static_cast<awst::ExpressionStatement const&>(_stmt);
		if (es.expr)
			scanExpression(*es.expr, _caller);
	}
	else if (type == "ReturnStatement")
	{
		auto const& rs = static_cast<awst::ReturnStatement const&>(_stmt);
		if (rs.value)
			scanExpression(*rs.value, _caller);
	}
	else if (type == "IfElse")
	{
		auto const& ie = static_cast<awst::IfElse const&>(_stmt);
		if (ie.condition)
			scanExpression(*ie.condition, _caller);
		if (ie.ifBranch)
			scanBlock(*ie.ifBranch, _caller);
		if (ie.elseBranch)
			scanBlock(*ie.elseBranch, _caller);
	}
	else if (type == "WhileLoop")
	{
		auto const& wl = static_cast<awst::WhileLoop const&>(_stmt);
		if (wl.condition)
			scanExpression(*wl.condition, _caller);
		if (wl.loopBody)
			scanBlock(*wl.loopBody, _caller);
	}
	else if (type == "AssignmentStatement")
	{
		auto const& as = static_cast<awst::AssignmentStatement const&>(_stmt);
		if (as.target)
			scanExpression(*as.target, _caller);
		if (as.value)
			scanExpression(*as.value, _caller);
	}
	else if (type == "Switch")
	{
		auto const& sw = static_cast<awst::Switch const&>(_stmt);
		if (sw.value)
			scanExpression(*sw.value, _caller);
		for (auto const& [caseExpr, caseBlock]: sw.cases)
		{
			if (caseExpr)
				scanExpression(*caseExpr, _caller);
			if (caseBlock)
				scanBlock(*caseBlock, _caller);
		}
		if (sw.defaultCase)
			scanBlock(*sw.defaultCase, _caller);
	}
	else if (type == "ForInLoop")
	{
		auto const& fil = static_cast<awst::ForInLoop const&>(_stmt);
		if (fil.sequence)
			scanExpression(*fil.sequence, _caller);
		if (fil.items)
			scanExpression(*fil.items, _caller);
		if (fil.loopBody)
			scanBlock(*fil.loopBody, _caller);
	}
	else if (type == "UInt64AugmentedAssignment")
	{
		auto const& ua = static_cast<awst::UInt64AugmentedAssignment const&>(_stmt);
		if (ua.target)
			scanExpression(*ua.target, _caller);
		if (ua.value)
			scanExpression(*ua.value, _caller);
	}
	else if (type == "BigUIntAugmentedAssignment")
	{
		auto const& ba = static_cast<awst::BigUIntAugmentedAssignment const&>(_stmt);
		if (ba.target)
			scanExpression(*ba.target, _caller);
		if (ba.value)
			scanExpression(*ba.value, _caller);
	}
}

void CallGraphAnalyzer::scanBlock(awst::Block const& _block, std::string const& _caller)
{
	for (auto const& stmt: _block.body)
		if (stmt)
			scanStatement(*stmt, _caller);
}

std::set<std::string> CallGraphAnalyzer::transitiveDeps(std::string const& _func) const
{
	std::set<std::string> visited;
	std::queue<std::string> worklist;
	worklist.push(_func);

	while (!worklist.empty())
	{
		std::string current = worklist.front();
		worklist.pop();

		if (visited.count(current))
			continue;
		visited.insert(current);

		auto it = m_functions.find(current);
		if (it == m_functions.end())
			continue;

		for (auto const& callee: it->second.calls)
		{
			if (!visited.count(callee))
				worklist.push(callee);
		}
	}

	// Remove self from deps
	visited.erase(_func);
	return visited;
}

std::vector<std::vector<std::string>> CallGraphAnalyzer::binPack(
	SizeEstimator::Estimate const& _sizes,
	size_t _maxSize,
	std::set<std::string> const& _rewrittenFunctions,
	std::set<std::string> const& _mutableSharedFunctions
) const
{
	auto& logger = Logger::instance();

	// Separate functions into categories:
	// 1. Orchestrator functions: state-accessing methods + ARC4 methods + approval/clear
	//    + rewritten parent functions (they reference chunks that may be in different helpers)
	// 2. Candidates: pure subroutines that can be moved to helpers
	std::vector<std::string> orchestratorFuncs;
	std::vector<std::pair<std::string, size_t>> candidates; // (name, size including deps)

	for (auto const& [name, info]: m_functions)
	{
		// Rewritten parent functions go to orchestrator — they call chunks via
		// SubroutineCallExpression which must resolve within the same contract.
		if (_rewrittenFunctions.count(name))
		{
			orchestratorFuncs.push_back(name);
		}
		else if (info.isContractMethod || info.hasStateAccess || info.isARC4Method)
		{
			orchestratorFuncs.push_back(name);
		}
		else
		{
			// Pure subroutine — candidate for moving to helper
			size_t totalSize = info.estimatedSize;
			// Include transitive dependencies' sizes
			auto deps = transitiveDeps(name);
			for (auto const& dep: deps)
			{
				auto sIt = _sizes.methodSizes.find(dep);
				if (sIt != _sizes.methodSizes.end())
				{
					auto fIt = m_functions.find(dep);
					// Only add if the dep is also a candidate (not an orchestrator func)
					if (fIt != m_functions.end() && !fIt->second.isContractMethod &&
						!fIt->second.hasStateAccess && !_rewrittenFunctions.count(dep))
						totalSize += sIt->second;
				}
			}
			candidates.push_back({name, totalSize});
		}
	}

	// Sort candidates by size, largest first (greedy bin-packing)
	std::sort(candidates.begin(), candidates.end(),
		[](auto const& a, auto const& b) { return a.second > b.second; });

	// First partition is always the orchestrator
	std::vector<std::vector<std::string>> partitions;
	partitions.push_back(orchestratorFuncs);

	// Track which functions are already assigned to a partition
	std::set<std::string> assigned(orchestratorFuncs.begin(), orchestratorFuncs.end());

	// Identify function clusters: if A only calls B and B only called by A, keep together.
	// But limit cluster size: don't add to cluster if total would exceed max partition.
	std::map<std::string, std::string> clusterLeader;
	std::map<std::string, size_t> clusterTotalSize; // leader → total cluster size
	for (auto const& [name, size]: candidates)
	{
		if (assigned.count(name))
			continue;

		auto it = m_functions.find(name);
		if (it == m_functions.end())
			continue;

		// If this function is only called by one other function, they're a cluster
		if (it->second.calledBy.size() == 1)
		{
			std::string caller = *it->second.calledBy.begin();
			if (!assigned.count(caller))
			{
				auto callerIt = m_functions.find(caller);
				if (callerIt != m_functions.end() && !callerIt->second.isContractMethod &&
					!_rewrittenFunctions.count(caller))
				{
					// Check if adding this member would exceed max partition size
					auto memberSizeIt = _sizes.methodSizes.find(name);
					size_t memberSize = memberSizeIt != _sizes.methodSizes.end() ? memberSizeIt->second : 0;
					size_t currentClusterSize = clusterTotalSize[caller];
					if (currentClusterSize + memberSize <= _maxSize)
					{
						clusterLeader[name] = caller;
						clusterTotalSize[caller] += memberSize;
					}
				}
			}
		}
	}

	// ─── Pre-assign mutable-shared chunk groups ──────────────────────────────
	// Chunks of mutable-shared functions MUST stay in the same partition
	// because they share ReferenceArray parameter state via slots.
	std::vector<size_t> partitionSizes; // size of each helper partition
	// partitions[0] is orchestrator, helpers start at index 1

	if (!_mutableSharedFunctions.empty())
	{
		// Group chunks by their parent function
		std::map<std::string, std::vector<std::pair<std::string, size_t>>> mutableGroups;
		for (auto const& [name, size]: candidates)
		{
			if (assigned.count(name))
				continue;
			auto pos = name.find("__chunk_");
			if (pos != std::string::npos)
			{
				std::string parent = name.substr(0, pos);
				if (_mutableSharedFunctions.count(parent))
					mutableGroups[parent].push_back({name, size});
			}
		}

		// Place each group in its own partition
		for (auto const& [parent, chunks]: mutableGroups)
		{
			size_t groupSize = 0;
			std::vector<std::string> groupNames;
			for (auto const& [chunkName, chunkSize]: chunks)
			{
				groupNames.push_back(chunkName);
				auto sizeIt = _sizes.methodSizes.find(chunkName);
				groupSize += (sizeIt != _sizes.methodSizes.end()) ? sizeIt->second : chunkSize;
			}

			partitions.push_back(groupNames);
			partitionSizes.push_back(groupSize);
			for (auto const& n: groupNames)
				assigned.insert(n);

			logger.debug("  Grouped " + std::to_string(groupNames.size()) +
				" mutable-shared chunks of '" + parent + "' (~" +
				std::to_string(groupSize * 2) + " bytes)");
		}
	}

	// ─── Bin-pack remaining candidates (marginal-cost aware) ─────────────
	// Track deps already in each partition so shared deps aren't double-counted.
	std::vector<std::set<std::string>> partitionDeps; // per helper partition
	// Initialize dep sets for any pre-assigned mutable-shared partitions
	for (size_t pi = 1; pi < partitions.size(); ++pi)
	{
		std::set<std::string> deps;
		for (auto const& funcName: partitions[pi])
		{
			deps.insert(funcName);
			auto td = transitiveDeps(funcName);
			deps.insert(td.begin(), td.end());
		}
		if (pi - 1 < partitionDeps.size())
			partitionDeps[pi - 1] = std::move(deps);
		else
			partitionDeps.push_back(std::move(deps));
	}

	for (auto const& [name, size]: candidates)
	{
		if (assigned.count(name))
			continue;

		// Compute this candidate's own size
		auto ownSizeIt = _sizes.methodSizes.find(name);
		size_t ownSize = (ownSizeIt != _sizes.methodSizes.end()) ? ownSizeIt->second : size;

		// Compute candidate's transitive deps (non-orchestrator only)
		auto candidateDeps = transitiveDeps(name);
		std::set<std::string> candidateNonOrchDeps;
		for (auto const& dep: candidateDeps)
		{
			auto fIt = m_functions.find(dep);
			if (fIt != m_functions.end() && !fIt->second.isContractMethod &&
				!fIt->second.hasStateAccess && !_rewrittenFunctions.count(dep))
				candidateNonOrchDeps.insert(dep);
		}

		// Try to fit into an existing partition using marginal cost
		bool placed = false;
		for (size_t i = 0; i < partitionSizes.size(); ++i)
		{
			// Marginal cost = candidate's own size + only NEW deps for this partition
			size_t marginalCost = ownSize;
			for (auto const& dep: candidateNonOrchDeps)
			{
				if (!partitionDeps[i].count(dep))
				{
					auto sIt = _sizes.methodSizes.find(dep);
					if (sIt != _sizes.methodSizes.end())
						marginalCost += sIt->second;
				}
			}

			if (partitionSizes[i] + marginalCost <= _maxSize)
			{
				partitions[i + 1].push_back(name); // +1 because [0] is orchestrator
				partitionSizes[i] += marginalCost;
				// Add candidate and its deps to this partition's dep set
				partitionDeps[i].insert(name);
				partitionDeps[i].insert(candidateNonOrchDeps.begin(), candidateNonOrchDeps.end());
				placed = true;
				break;
			}
		}

		if (!placed)
		{
			// Create new partition
			partitions.push_back({name});
			std::set<std::string> newDeps;
			newDeps.insert(name);
			newDeps.insert(candidateNonOrchDeps.begin(), candidateNonOrchDeps.end());
			size_t newSize = ownSize;
			for (auto const& dep: candidateNonOrchDeps)
			{
				auto sIt = _sizes.methodSizes.find(dep);
				if (sIt != _sizes.methodSizes.end())
					newSize += sIt->second;
			}
			partitionSizes.push_back(newSize);
			partitionDeps.push_back(std::move(newDeps));
		}

		assigned.insert(name);

		// Also place cluster members (only if they fit within the partition threshold)
		for (auto const& [member, leader]: clusterLeader)
		{
			if (leader == name && !assigned.count(member))
			{
				auto memberSizeIt = _sizes.methodSizes.find(member);
				size_t memberSize = memberSizeIt != _sizes.methodSizes.end() ? memberSizeIt->second : 0;

				// Place in same partition as leader
				size_t partIdx = partitions.size() - 1;
				for (size_t i = 1; i < partitions.size(); ++i)
				{
					for (auto const& f: partitions[i])
					{
						if (f == name)
						{
							partIdx = i;
							break;
						}
					}
				}

				if (partIdx > 0 && partIdx - 1 < partitionSizes.size())
				{
					// Check if adding the member would exceed the partition threshold.
					// If so, skip — the member will be placed independently later.
					if (partitionSizes[partIdx - 1] + memberSize > _maxSize)
						continue;

					partitions[partIdx].push_back(member);
					partitionSizes[partIdx - 1] += memberSize;
					// Add member and its deps to partition's dep set
					partitionDeps[partIdx - 1].insert(member);
					auto memberDeps = transitiveDeps(member);
					for (auto const& dep: memberDeps)
					{
						auto fIt = m_functions.find(dep);
						if (fIt != m_functions.end() && !fIt->second.isContractMethod &&
							!fIt->second.hasStateAccess && !_rewrittenFunctions.count(dep))
							partitionDeps[partIdx - 1].insert(dep);
					}
				}
				assigned.insert(member);
			}
		}
	}

	// Log partition sizes
	for (size_t i = 0; i < partitions.size(); ++i)
	{
		size_t totalSize = 0;
		for (auto const& funcName: partitions[i])
		{
			auto it = _sizes.methodSizes.find(funcName);
			if (it != _sizes.methodSizes.end())
				totalSize += it->second;
		}
		std::string partLabel = (i == 0) ? "Orchestrator" : "Helper " + std::to_string(i);
		logger.debug(partLabel + ": " + std::to_string(partitions[i].size()) +
			" functions, ~" + std::to_string(totalSize * 2) + " estimated bytes");
	}

	return partitions;
}

} // namespace puyasol::splitter
