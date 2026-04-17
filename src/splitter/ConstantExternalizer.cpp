#include "splitter/ConstantExternalizer.h"
#include "Logger.h"

#include <algorithm>
#include <functional>
#include <sstream>

namespace puyasol::splitter
{

// ─── Utility ─────────────────────────────────────────────────────────────────

std::string ConstantExternalizer::hashBytes(std::vector<uint8_t> const& _bytes)
{
	// Hash full content for deduplication using FNV-1a
	uint64_t hash = 14695981039346656037ULL; // FNV offset basis
	for (uint8_t b: _bytes)
	{
		hash ^= b;
		hash *= 1099511628211ULL; // FNV prime
	}
	std::ostringstream oss;
	oss << _bytes.size() << ":" << std::hex << hash;
	return oss.str();
}

// ─── Phase 1: Find large constants ──────────────────────────────────────────

void ConstantExternalizer::findConstants(awst::Expression const& _expr, size_t _threshold)
{
	std::string type = _expr.nodeType();

	if (type == "BytesConstant")
	{
		auto const& bc = static_cast<awst::BytesConstant const&>(_expr);
		if (bc.value.size() >= _threshold)
		{
			auto hash = hashBytes(bc.value);
			if (m_constants.find(hash) == m_constants.end())
			{
				ConstantEntry entry;
				entry.hash = hash;
				entry.value = bc.value;
				entry.scratchSlot = m_nextSlot++;
				entry.boxOffset = m_nextOffset;
				m_nextOffset += bc.value.size();
				m_constants[hash] = std::move(entry);
			}
		}
		return;
	}

	// Recursively scan child expressions
	if (type == "SubroutineCallExpression")
	{
		auto const& call = static_cast<awst::SubroutineCallExpression const&>(_expr);
		for (auto const& arg: call.args)
			if (arg.value) findConstants(*arg.value, _threshold);
	}
	else if (type == "UInt64BinaryOperation")
	{
		auto const& op = static_cast<awst::UInt64BinaryOperation const&>(_expr);
		if (op.left) findConstants(*op.left, _threshold);
		if (op.right) findConstants(*op.right, _threshold);
	}
	else if (type == "BigUIntBinaryOperation")
	{
		auto const& op = static_cast<awst::BigUIntBinaryOperation const&>(_expr);
		if (op.left) findConstants(*op.left, _threshold);
		if (op.right) findConstants(*op.right, _threshold);
	}
	else if (type == "BytesBinaryOperation")
	{
		auto const& op = static_cast<awst::BytesBinaryOperation const&>(_expr);
		if (op.left) findConstants(*op.left, _threshold);
		if (op.right) findConstants(*op.right, _threshold);
	}
	else if (type == "BytesUnaryOperation")
	{
		auto const& op = static_cast<awst::BytesUnaryOperation const&>(_expr);
		if (op.expr) findConstants(*op.expr, _threshold);
	}
	else if (type == "NumericComparisonExpression")
	{
		auto const& cmp = static_cast<awst::NumericComparisonExpression const&>(_expr);
		if (cmp.lhs) findConstants(*cmp.lhs, _threshold);
		if (cmp.rhs) findConstants(*cmp.rhs, _threshold);
	}
	else if (type == "BytesComparisonExpression")
	{
		auto const& cmp = static_cast<awst::BytesComparisonExpression const&>(_expr);
		if (cmp.lhs) findConstants(*cmp.lhs, _threshold);
		if (cmp.rhs) findConstants(*cmp.rhs, _threshold);
	}
	else if (type == "BooleanBinaryOperation")
	{
		auto const& op = static_cast<awst::BooleanBinaryOperation const&>(_expr);
		if (op.left) findConstants(*op.left, _threshold);
		if (op.right) findConstants(*op.right, _threshold);
	}
	else if (type == "Not")
	{
		auto const& n = static_cast<awst::Not const&>(_expr);
		if (n.expr) findConstants(*n.expr, _threshold);
	}
	else if (type == "AssertExpression")
	{
		auto const& a = static_cast<awst::AssertExpression const&>(_expr);
		if (a.condition) findConstants(*a.condition, _threshold);
	}
	else if (type == "AssignmentExpression")
	{
		auto const& a = static_cast<awst::AssignmentExpression const&>(_expr);
		if (a.target) findConstants(*a.target, _threshold);
		if (a.value) findConstants(*a.value, _threshold);
	}
	else if (type == "ConditionalExpression")
	{
		auto const& c = static_cast<awst::ConditionalExpression const&>(_expr);
		if (c.condition) findConstants(*c.condition, _threshold);
		if (c.trueExpr) findConstants(*c.trueExpr, _threshold);
		if (c.falseExpr) findConstants(*c.falseExpr, _threshold);
	}
	else if (type == "IntrinsicCall")
	{
		auto const& ic = static_cast<awst::IntrinsicCall const&>(_expr);
		for (auto const& arg: ic.stackArgs)
			if (arg) findConstants(*arg, _threshold);
	}
	else if (type == "PuyaLibCall")
	{
		auto const& plc = static_cast<awst::PuyaLibCall const&>(_expr);
		for (auto const& arg: plc.args)
			if (arg.value) findConstants(*arg.value, _threshold);
	}
	else if (type == "FieldExpression")
	{
		auto const& f = static_cast<awst::FieldExpression const&>(_expr);
		if (f.base) findConstants(*f.base, _threshold);
	}
	else if (type == "IndexExpression")
	{
		auto const& idx = static_cast<awst::IndexExpression const&>(_expr);
		if (idx.base) findConstants(*idx.base, _threshold);
		if (idx.index) findConstants(*idx.index, _threshold);
	}
	else if (type == "TupleExpression")
	{
		auto const& t = static_cast<awst::TupleExpression const&>(_expr);
		for (auto const& item: t.items)
			if (item) findConstants(*item, _threshold);
	}
	else if (type == "TupleItemExpression")
	{
		auto const& ti = static_cast<awst::TupleItemExpression const&>(_expr);
		if (ti.base) findConstants(*ti.base, _threshold);
	}
	else if (type == "ARC4Encode")
	{
		auto const& e = static_cast<awst::ARC4Encode const&>(_expr);
		if (e.value) findConstants(*e.value, _threshold);
	}
	else if (type == "ARC4Decode")
	{
		auto const& d = static_cast<awst::ARC4Decode const&>(_expr);
		if (d.value) findConstants(*d.value, _threshold);
	}
	else if (type == "ReinterpretCast")
	{
		auto const& rc = static_cast<awst::ReinterpretCast const&>(_expr);
		if (rc.expr) findConstants(*rc.expr, _threshold);
	}
	else if (type == "Copy")
	{
		auto const& c = static_cast<awst::Copy const&>(_expr);
		if (c.value) findConstants(*c.value, _threshold);
	}
	else if (type == "SingleEvaluation")
	{
		auto const& se = static_cast<awst::SingleEvaluation const&>(_expr);
		if (se.source) findConstants(*se.source, _threshold);
	}
	else if (type == "CheckedMaybe")
	{
		auto const& cm = static_cast<awst::CheckedMaybe const&>(_expr);
		if (cm.expr) findConstants(*cm.expr, _threshold);
	}
	else if (type == "NewArray")
	{
		auto const& na = static_cast<awst::NewArray const&>(_expr);
		for (auto const& v: na.values)
			if (v) findConstants(*v, _threshold);
	}
	else if (type == "NewStruct")
	{
		auto const& ns = static_cast<awst::NewStruct const&>(_expr);
		for (auto const& [_, val]: ns.values)
			if (val) findConstants(*val, _threshold);
	}
	else if (type == "NamedTupleExpression")
	{
		auto const& nt = static_cast<awst::NamedTupleExpression const&>(_expr);
		for (auto const& [_, val]: nt.values)
			if (val) findConstants(*val, _threshold);
	}
}

void ConstantExternalizer::findConstantsInStatement(awst::Statement const& _stmt, size_t _threshold)
{
	std::string type = _stmt.nodeType();

	if (type == "Block")
		findConstantsInBlock(static_cast<awst::Block const&>(_stmt), _threshold);
	else if (type == "ExpressionStatement")
	{
		auto const& es = static_cast<awst::ExpressionStatement const&>(_stmt);
		if (es.expr) findConstants(*es.expr, _threshold);
	}
	else if (type == "ReturnStatement")
	{
		auto const& rs = static_cast<awst::ReturnStatement const&>(_stmt);
		if (rs.value) findConstants(*rs.value, _threshold);
	}
	else if (type == "IfElse")
	{
		auto const& ie = static_cast<awst::IfElse const&>(_stmt);
		if (ie.condition) findConstants(*ie.condition, _threshold);
		if (ie.ifBranch) findConstantsInBlock(*ie.ifBranch, _threshold);
		if (ie.elseBranch) findConstantsInBlock(*ie.elseBranch, _threshold);
	}
	else if (type == "WhileLoop")
	{
		auto const& wl = static_cast<awst::WhileLoop const&>(_stmt);
		if (wl.condition) findConstants(*wl.condition, _threshold);
		if (wl.loopBody) findConstantsInBlock(*wl.loopBody, _threshold);
	}
	else if (type == "AssignmentStatement")
	{
		auto const& as = static_cast<awst::AssignmentStatement const&>(_stmt);
		if (as.target) findConstants(*as.target, _threshold);
		if (as.value) findConstants(*as.value, _threshold);
	}
	else if (type == "Switch")
	{
		auto const& sw = static_cast<awst::Switch const&>(_stmt);
		if (sw.value) findConstants(*sw.value, _threshold);
		for (auto const& [caseExpr, caseBlock]: sw.cases)
		{
			if (caseExpr) findConstants(*caseExpr, _threshold);
			if (caseBlock) findConstantsInBlock(*caseBlock, _threshold);
		}
		if (sw.defaultCase) findConstantsInBlock(*sw.defaultCase, _threshold);
	}
	else if (type == "ForInLoop")
	{
		auto const& fil = static_cast<awst::ForInLoop const&>(_stmt);
		if (fil.sequence) findConstants(*fil.sequence, _threshold);
		if (fil.items) findConstants(*fil.items, _threshold);
		if (fil.loopBody) findConstantsInBlock(*fil.loopBody, _threshold);
	}
	else if (type == "UInt64AugmentedAssignment")
	{
		auto const& ua = static_cast<awst::UInt64AugmentedAssignment const&>(_stmt);
		if (ua.target) findConstants(*ua.target, _threshold);
		if (ua.value) findConstants(*ua.value, _threshold);
	}
	else if (type == "BigUIntAugmentedAssignment")
	{
		auto const& ba = static_cast<awst::BigUIntAugmentedAssignment const&>(_stmt);
		if (ba.target) findConstants(*ba.target, _threshold);
		if (ba.value) findConstants(*ba.value, _threshold);
	}
}

void ConstantExternalizer::findConstantsInBlock(awst::Block const& _block, size_t _threshold)
{
	for (auto const& stmt: _block.body)
		if (stmt) findConstantsInStatement(*stmt, _threshold);
}

// ─── Phase 2: Replace constants with gloads reads ────────────────────────────

std::shared_ptr<awst::Expression> ConstantExternalizer::buildGloadsRead(
	size_t _scratchSlot,
	awst::SourceLocation const& _loc
)
{
	// Build: gloads _scratchSlot, with txn GroupIndex - 1 on stack
	//   txn GroupIndex
	//   pushint 1
	//   -
	//   gloads _scratchSlot

	auto groupIdx = std::make_shared<awst::IntrinsicCall>();
	groupIdx->opCode = "txn";
	groupIdx->immediates = {std::string("GroupIndex")};
	groupIdx->wtype = awst::WType::uint64Type();
	groupIdx->sourceLocation = _loc;

	auto one = std::make_shared<awst::IntegerConstant>();
	one->value = "1";
	one->wtype = awst::WType::uint64Type();
	one->sourceLocation = _loc;

	auto prevIdx = std::make_shared<awst::UInt64BinaryOperation>();
	prevIdx->left = groupIdx;
	prevIdx->op = awst::UInt64BinaryOperator::Sub;
	prevIdx->right = one;
	prevIdx->wtype = awst::WType::uint64Type();
	prevIdx->sourceLocation = _loc;

	auto gloads = std::make_shared<awst::IntrinsicCall>();
	gloads->opCode = "gloads";
	gloads->immediates = {static_cast<int>(_scratchSlot)};
	gloads->stackArgs = {prevIdx};
	gloads->wtype = awst::WType::bytesType();
	gloads->sourceLocation = _loc;

	return gloads;
}

std::shared_ptr<awst::Expression> ConstantExternalizer::replaceInExpression(
	std::shared_ptr<awst::Expression> _expr
)
{
	if (!_expr)
		return _expr;

	std::string type = _expr->nodeType();

	// Check if this is a large BytesConstant that was externalized
	if (type == "BytesConstant")
	{
		auto const& bc = static_cast<awst::BytesConstant const&>(*_expr);
		auto hash = hashBytes(bc.value);
		auto it = m_constants.find(hash);
		if (it != m_constants.end())
			return buildGloadsRead(it->second.scratchSlot, _expr->sourceLocation);
		return _expr;
	}

	// Recursively replace in child expressions
	if (type == "SubroutineCallExpression")
	{
		auto& call = static_cast<awst::SubroutineCallExpression&>(*_expr);
		for (auto& arg: call.args)
			if (arg.value) arg.value = replaceInExpression(arg.value);
	}
	else if (type == "UInt64BinaryOperation")
	{
		auto& op = static_cast<awst::UInt64BinaryOperation&>(*_expr);
		if (op.left) op.left = replaceInExpression(op.left);
		if (op.right) op.right = replaceInExpression(op.right);
	}
	else if (type == "BigUIntBinaryOperation")
	{
		auto& op = static_cast<awst::BigUIntBinaryOperation&>(*_expr);
		if (op.left) op.left = replaceInExpression(op.left);
		if (op.right) op.right = replaceInExpression(op.right);
	}
	else if (type == "BytesBinaryOperation")
	{
		auto& op = static_cast<awst::BytesBinaryOperation&>(*_expr);
		if (op.left) op.left = replaceInExpression(op.left);
		if (op.right) op.right = replaceInExpression(op.right);
	}
	else if (type == "BytesUnaryOperation")
	{
		auto& op = static_cast<awst::BytesUnaryOperation&>(*_expr);
		if (op.expr) op.expr = replaceInExpression(op.expr);
	}
	else if (type == "NumericComparisonExpression")
	{
		auto& cmp = static_cast<awst::NumericComparisonExpression&>(*_expr);
		if (cmp.lhs) cmp.lhs = replaceInExpression(cmp.lhs);
		if (cmp.rhs) cmp.rhs = replaceInExpression(cmp.rhs);
	}
	else if (type == "BytesComparisonExpression")
	{
		auto& cmp = static_cast<awst::BytesComparisonExpression&>(*_expr);
		if (cmp.lhs) cmp.lhs = replaceInExpression(cmp.lhs);
		if (cmp.rhs) cmp.rhs = replaceInExpression(cmp.rhs);
	}
	else if (type == "BooleanBinaryOperation")
	{
		auto& op = static_cast<awst::BooleanBinaryOperation&>(*_expr);
		if (op.left) op.left = replaceInExpression(op.left);
		if (op.right) op.right = replaceInExpression(op.right);
	}
	else if (type == "Not")
	{
		auto& n = static_cast<awst::Not&>(*_expr);
		if (n.expr) n.expr = replaceInExpression(n.expr);
	}
	else if (type == "AssertExpression")
	{
		auto& a = static_cast<awst::AssertExpression&>(*_expr);
		if (a.condition) a.condition = replaceInExpression(a.condition);
	}
	else if (type == "AssignmentExpression")
	{
		auto& a = static_cast<awst::AssignmentExpression&>(*_expr);
		if (a.target) a.target = replaceInExpression(a.target);
		if (a.value) a.value = replaceInExpression(a.value);
	}
	else if (type == "ConditionalExpression")
	{
		auto& c = static_cast<awst::ConditionalExpression&>(*_expr);
		if (c.condition) c.condition = replaceInExpression(c.condition);
		if (c.trueExpr) c.trueExpr = replaceInExpression(c.trueExpr);
		if (c.falseExpr) c.falseExpr = replaceInExpression(c.falseExpr);
	}
	else if (type == "IntrinsicCall")
	{
		auto& ic = static_cast<awst::IntrinsicCall&>(*_expr);
		for (auto& arg: ic.stackArgs)
			if (arg) arg = replaceInExpression(arg);
	}
	else if (type == "PuyaLibCall")
	{
		auto& plc = static_cast<awst::PuyaLibCall&>(*_expr);
		for (auto& arg: plc.args)
			if (arg.value) arg.value = replaceInExpression(arg.value);
	}
	else if (type == "FieldExpression")
	{
		auto& f = static_cast<awst::FieldExpression&>(*_expr);
		if (f.base) f.base = replaceInExpression(f.base);
	}
	else if (type == "IndexExpression")
	{
		auto& idx = static_cast<awst::IndexExpression&>(*_expr);
		if (idx.base) idx.base = replaceInExpression(idx.base);
		if (idx.index) idx.index = replaceInExpression(idx.index);
	}
	else if (type == "TupleExpression")
	{
		auto& t = static_cast<awst::TupleExpression&>(*_expr);
		for (auto& item: t.items)
			if (item) item = replaceInExpression(item);
	}
	else if (type == "TupleItemExpression")
	{
		auto& ti = static_cast<awst::TupleItemExpression&>(*_expr);
		if (ti.base) ti.base = replaceInExpression(ti.base);
	}
	else if (type == "ARC4Encode")
	{
		auto& e = static_cast<awst::ARC4Encode&>(*_expr);
		if (e.value) e.value = replaceInExpression(e.value);
	}
	else if (type == "ARC4Decode")
	{
		auto& d = static_cast<awst::ARC4Decode&>(*_expr);
		if (d.value) d.value = replaceInExpression(d.value);
	}
	else if (type == "ReinterpretCast")
	{
		auto& rc = static_cast<awst::ReinterpretCast&>(*_expr);
		if (rc.expr) rc.expr = replaceInExpression(rc.expr);
	}
	else if (type == "Copy")
	{
		auto& c = static_cast<awst::Copy&>(*_expr);
		if (c.value) c.value = replaceInExpression(c.value);
	}
	else if (type == "SingleEvaluation")
	{
		auto& se = static_cast<awst::SingleEvaluation&>(*_expr);
		if (se.source) se.source = replaceInExpression(se.source);
	}
	else if (type == "CheckedMaybe")
	{
		auto& cm = static_cast<awst::CheckedMaybe&>(*_expr);
		if (cm.expr) cm.expr = replaceInExpression(cm.expr);
	}
	else if (type == "NewArray")
	{
		auto& na = static_cast<awst::NewArray&>(*_expr);
		for (auto& v: na.values)
			if (v) v = replaceInExpression(v);
	}
	else if (type == "NewStruct")
	{
		auto& ns = static_cast<awst::NewStruct&>(*_expr);
		for (auto& [_, val]: ns.values)
			if (val) val = replaceInExpression(val);
	}
	else if (type == "NamedTupleExpression")
	{
		auto& nt = static_cast<awst::NamedTupleExpression&>(*_expr);
		for (auto& [_, val]: nt.values)
			if (val) val = replaceInExpression(val);
	}

	return _expr;
}

void ConstantExternalizer::replaceInStatement(std::shared_ptr<awst::Statement>& _stmt)
{
	if (!_stmt) return;
	std::string type = _stmt->nodeType();

	if (type == "Block")
	{
		auto block = std::dynamic_pointer_cast<awst::Block>(_stmt);
		if (block) replaceInBlock(block);
	}
	else if (type == "ExpressionStatement")
	{
		auto& es = static_cast<awst::ExpressionStatement&>(*_stmt);
		if (es.expr) es.expr = replaceInExpression(es.expr);
	}
	else if (type == "ReturnStatement")
	{
		auto& rs = static_cast<awst::ReturnStatement&>(*_stmt);
		if (rs.value) rs.value = replaceInExpression(rs.value);
	}
	else if (type == "IfElse")
	{
		auto& ie = static_cast<awst::IfElse&>(*_stmt);
		if (ie.condition) ie.condition = replaceInExpression(ie.condition);
		if (ie.ifBranch) replaceInBlock(ie.ifBranch);
		if (ie.elseBranch) replaceInBlock(ie.elseBranch);
	}
	else if (type == "WhileLoop")
	{
		auto& wl = static_cast<awst::WhileLoop&>(*_stmt);
		if (wl.condition) wl.condition = replaceInExpression(wl.condition);
		if (wl.loopBody) replaceInBlock(wl.loopBody);
	}
	else if (type == "AssignmentStatement")
	{
		auto& as = static_cast<awst::AssignmentStatement&>(*_stmt);
		if (as.target) as.target = replaceInExpression(as.target);
		if (as.value) as.value = replaceInExpression(as.value);
	}
	else if (type == "Switch")
	{
		auto& sw = static_cast<awst::Switch&>(*_stmt);
		if (sw.value) sw.value = replaceInExpression(sw.value);
		for (auto& [caseExpr, caseBlock]: sw.cases)
		{
			if (caseExpr) caseExpr = replaceInExpression(caseExpr);
			if (caseBlock) replaceInBlock(caseBlock);
		}
		if (sw.defaultCase) replaceInBlock(sw.defaultCase);
	}
	else if (type == "ForInLoop")
	{
		auto& fil = static_cast<awst::ForInLoop&>(*_stmt);
		if (fil.sequence) fil.sequence = replaceInExpression(fil.sequence);
		if (fil.items) fil.items = replaceInExpression(fil.items);
		if (fil.loopBody) replaceInBlock(fil.loopBody);
	}
	else if (type == "UInt64AugmentedAssignment")
	{
		auto& ua = static_cast<awst::UInt64AugmentedAssignment&>(*_stmt);
		if (ua.target) ua.target = replaceInExpression(ua.target);
		if (ua.value) ua.value = replaceInExpression(ua.value);
	}
	else if (type == "BigUIntAugmentedAssignment")
	{
		auto& ba = static_cast<awst::BigUIntAugmentedAssignment&>(*_stmt);
		if (ba.target) ba.target = replaceInExpression(ba.target);
		if (ba.value) ba.value = replaceInExpression(ba.value);
	}
}

void ConstantExternalizer::replaceInBlock(std::shared_ptr<awst::Block>& _block)
{
	if (!_block) return;
	for (auto& stmt: _block->body)
		replaceInStatement(stmt);
}

// ─── __load_constants method builder ─────────────────────────────────────────

awst::ContractMethod ConstantExternalizer::buildLoadConstantsMethod(
	awst::SourceLocation const& _loc,
	std::string const& _contractId,
	std::string const& _boxName
)
{
	awst::ContractMethod method;
	method.sourceLocation = _loc;
	method.returnType = awst::WType::voidType();
	method.cref = _contractId;
	method.memberName = "__load_constants";
	method.pure = false;

	auto body = std::make_shared<awst::Block>();
	body->sourceLocation = _loc;

	// Build box key as BytesConstant
	auto makeBoxKey = [&]() -> std::shared_ptr<awst::BytesConstant>
	{
		return awst::makeUtf8BytesConstant(_boxName, _loc);
	};

	// For each externalized constant:
	//   store slot_i, box_extract("__constants", offset, length)
	for (auto const& [hash, entry]: m_constants)
	{
		// box_extract(key, offset, length)
		auto boxExtract = std::make_shared<awst::IntrinsicCall>();
		boxExtract->sourceLocation = _loc;
		boxExtract->opCode = "box_extract";
		boxExtract->wtype = awst::WType::bytesType();

		auto offset = std::make_shared<awst::IntegerConstant>();
		offset->value = std::to_string(entry.boxOffset);
		offset->wtype = awst::WType::uint64Type();
		offset->sourceLocation = _loc;

		auto length = std::make_shared<awst::IntegerConstant>();
		length->value = std::to_string(entry.value.size());
		length->wtype = awst::WType::uint64Type();
		length->sourceLocation = _loc;

		boxExtract->stackArgs = {makeBoxKey(), offset, length};

		// store slot_i, box_extract(...)
		auto storeCall = std::make_shared<awst::IntrinsicCall>();
		storeCall->sourceLocation = _loc;
		storeCall->opCode = "store";
		storeCall->immediates = {static_cast<int>(entry.scratchSlot)};
		storeCall->wtype = awst::WType::voidType();
		storeCall->stackArgs = {boxExtract};

		auto stmt = std::make_shared<awst::ExpressionStatement>();
		stmt->sourceLocation = _loc;
		stmt->expr = storeCall;
		body->body.push_back(stmt);
	}

	// return (void)
	auto ret = std::make_shared<awst::ReturnStatement>();
	ret->sourceLocation = _loc;
	body->body.push_back(ret);

	method.body = body;

	// ABI config: allow NoOp calls (not on create)
	awst::ARC4ABIMethodConfig abiConfig;
	abiConfig.sourceLocation = _loc;
	abiConfig.allowedCompletionTypes = {0}; // NoOp
	abiConfig.create = 3; // Disallow
	abiConfig.name = "__load_constants";
	abiConfig.readonly = false;
	method.arc4MethodConfig = abiConfig;

	return method;
}

// ─── Main entry point ────────────────────────────────────────────────────────

ConstantExternalizer::Result ConstantExternalizer::externalize(
	awst::Contract& _contract,
	std::vector<std::shared_ptr<awst::RootNode>>& _subroutines,
	size_t _sizeThreshold
)
{
	auto& logger = Logger::instance();
	Result result;
	result.boxName = "__constants";

	m_constants.clear();
	m_nextSlot = ScratchSlotBase;
	m_nextOffset = 0;

	// Phase 1: Find large constants in subroutines
	for (auto const& root: _subroutines)
	{
		if (auto const* sub = dynamic_cast<awst::Subroutine const*>(root.get()))
			if (sub->body) findConstantsInBlock(*sub->body, _sizeThreshold);
	}

	// Also scan contract method bodies
	for (auto const& method: _contract.methods)
		if (method.body) findConstantsInBlock(*method.body, _sizeThreshold);

	if (m_constants.empty())
	{
		logger.debug("ConstantExternalizer: no large constants found (threshold=" +
			std::to_string(_sizeThreshold) + " bytes)");
		return result;
	}

	logger.info("ConstantExternalizer: found " + std::to_string(m_constants.size()) +
		" large constant(s) totaling " + std::to_string(m_nextOffset) + " bytes");

	// Phase 2: Replace constants with gloads reads
	for (auto& root: _subroutines)
	{
		if (auto sub = std::dynamic_pointer_cast<awst::Subroutine>(root))
			if (sub->body) replaceInBlock(sub->body);
	}

	for (auto& method: _contract.methods)
		if (method.body) replaceInBlock(method.body);

	// Phase 3: Add __load_constants method to the contract
	auto loadMethod = buildLoadConstantsMethod(
		_contract.sourceLocation,
		_contract.id,
		result.boxName
	);
	_contract.methods.push_back(std::move(loadMethod));

	logger.info("ConstantExternalizer: added __load_constants method with " +
		std::to_string(m_constants.size()) + " scratch slots (" +
		std::to_string(ScratchSlotBase) + "-" +
		std::to_string(m_nextSlot - 1) + ")");

	// Build result
	result.didExternalize = true;
	result.totalBoxSize = m_nextOffset;
	for (auto const& [hash, entry]: m_constants)
	{
		result.constants.push_back({
			entry.scratchSlot,
			entry.boxOffset,
			entry.value.size(),
			entry.value
		});
	}

	return result;
}

} // namespace puyasol::splitter
