/// @file FunctionSplitter.cpp
/// See FunctionSplitter.h for design.

#include "splitter/FunctionSplitter.h"
#include "Logger.h"

#include <algorithm>
#include <sstream>

namespace puyasol::splitter
{

// ─── Live-vars analysis (defs ∩ uses across split point) ─────────────

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

void FunctionSplitter::collectVarType(awst::Expression const& _expr)
{
	// Walk and capture name → wtype as a side effect of collectExprUses.
	// (collectExprUses already populates m_varTypes for VarExpression.)
	std::set<std::string> dummy;
	collectExprUses(_expr, dummy);
}

// ─── computeLiveVars ─────────────────────────────────────────────────

std::vector<FunctionSplitter::VarInfo> FunctionSplitter::computeLiveVars(
	std::vector<std::shared_ptr<awst::Statement>> const& _stmts,
	size_t _splitPoint,
	std::set<std::string> const& _paramNames)
{
	// defs in [0, splitPoint)
	std::set<std::string> definedBefore;
	for (size_t i = 0; i < _splitPoint; ++i)
		if (_stmts[i])
			collectStmtDefs(*_stmts[i], definedBefore);

	// uses in [splitPoint, end)
	std::set<std::string> usedAfter;
	for (size_t i = _splitPoint; i < _stmts.size(); ++i)
		if (_stmts[i])
			collectStmtUses(*_stmts[i], usedAfter);

	// Live = defined-before ∩ used-after. Includes redefined params:
	// if a param is reassigned in the prefix, the suffix sees the new
	// value, which must travel through scratch like any other live var.
	std::vector<VarInfo> live;
	for (auto const& var : definedBefore)
	{
		if (!usedAfter.count(var))
			continue;
		VarInfo vi;
		vi.name = var;
		auto it = m_varTypes.find(var);
		vi.wtype = (it != m_varTypes.end())
			? it->second
			: awst::WType::biguintType();
		live.push_back(vi);
	}

	std::sort(live.begin(), live.end(),
		[](VarInfo const& a, VarInfo const& b) { return a.name < b.name; });

	return live;
}

// ─── Piece emission ──────────────────────────────────────────────────
//
// Each piece is a free-standing Subroutine that:
//   * Takes the same arg list as the original (so callers / orch can
//     pass through verbatim). Piece 0 actually consumes them; subsequent
//     pieces ignore most and read live-vars from scratch slot 100.
//   * Body:
//       - prologue: gload(prev_call_txn_index, kLiveVarsScratchSlot)
//                   then ARC4-decode the tuple into the live vars
//                   (skipped on piece 0 — it gets values from args)
//       - sliced statements from the original body
//       - epilogue: ARC4-encode current live-vars into a tuple, store
//                   to scratch slot 100
//                   (skipped on the last piece — it returns the original
//                    return value as normal)
//   * Returns: original sub's return type for the LAST piece; void
//     for intermediate pieces.

namespace {

std::string pieceName(std::string const& _orig, size_t _index, int _groupId)
{
	return _orig + "__piece_" + std::to_string(_index)
		+ "_g" + std::to_string(_groupId);
}

// Convention: every live var is normalised to a fixed-width 32-byte
// chunk, all chunks concatenated, stored at scratch slot 100.
// Decoding is the inverse: extract each chunk, reinterpret to wtype.
//
// Why not ARC4: ARC4 tuple encoding requires plumbing ARC4Tuple wtypes
// (mapping each WTuple element to its ARC4 form) and that plumbing
// lives in puya-sol's TypeMapper, not in the splitter. Fixed-width
// bytes round-trip cleanly through ReinterpretCast for biguint /
// account / bool / uint64 — covers everything FunctionSplitter sees
// in practice.
constexpr int kLiveVarBytes = 32;

std::shared_ptr<awst::Expression> coerceToFixedBytes(
	std::shared_ptr<awst::Expression> _value,
	awst::WType const* _wtype,
	awst::SourceLocation const& _loc)
{
	if (_wtype == awst::WType::biguintType())
	{
		// biguint → bytes (variable length) → left-pad to 32 by
		// concat-with-bzero(32) + extract last 32 bytes.
		auto bytes = awst::makeReinterpretCast(
			std::move(_value), awst::WType::bytesType(), _loc);
		auto bz = awst::makeBzero(32, _loc);
		auto cat = awst::makeConcat(std::move(bz), std::move(bytes), _loc);
		auto len = awst::makeLen(cat, _loc);
		auto offset = awst::makeUInt64BinOp(
			std::move(len), awst::UInt64BinaryOperator::Sub,
			awst::makeIntegerConstant("32", _loc), _loc);
		auto extract = awst::makeIntrinsicCall(
			"extract3", awst::WType::bytesType(), _loc);
		extract->stackArgs.push_back(cat);
		extract->stackArgs.push_back(std::move(offset));
		extract->stackArgs.push_back(awst::makeIntegerConstant("32", _loc));
		return extract;
	}
	if (_wtype == awst::WType::accountType())
		return awst::makeReinterpretCast(
			std::move(_value), awst::WType::bytesType(), _loc);
	if (_wtype == awst::WType::uint64Type())
	{
		auto itob = awst::makeItob(std::move(_value), _loc);
		return awst::makeLeftPad(std::move(itob), 24, _loc);
	}
	if (_wtype == awst::WType::boolType())
	{
		auto cond = awst::makeConditional(
			std::move(_value),
			awst::makeIntegerConstant("1", _loc),
			awst::makeIntegerConstant("0", _loc),
			awst::WType::uint64Type(), _loc);
		auto itob = awst::makeItob(std::move(cond), _loc);
		return awst::makeLeftPad(std::move(itob), 24, _loc);
	}
	if (_wtype && _wtype->kind() == awst::WTypeKind::Bytes)
		return _value;
	return awst::makeReinterpretCast(
		std::move(_value), awst::WType::bytesType(), _loc);
}

std::shared_ptr<awst::Expression> coerceFromFixedBytes(
	std::shared_ptr<awst::Expression> _bytes,
	awst::WType const* _wtype,
	awst::SourceLocation const& _loc)
{
	if (_wtype == awst::WType::biguintType())
		return awst::makeReinterpretCast(
			std::move(_bytes), awst::WType::biguintType(), _loc);
	if (_wtype == awst::WType::accountType())
		return awst::makeReinterpretCast(
			std::move(_bytes), awst::WType::accountType(), _loc);
	if (_wtype == awst::WType::uint64Type())
	{
		auto extract = awst::makeIntrinsicCall(
			"extract", awst::WType::bytesType(), _loc);
		extract->immediates = {24, 8};
		extract->stackArgs.push_back(std::move(_bytes));
		return awst::makeBtoi(std::move(extract), _loc);
	}
	if (_wtype == awst::WType::boolType())
	{
		auto extract = awst::makeIntrinsicCall(
			"extract", awst::WType::bytesType(), _loc);
		extract->immediates = {31, 1};
		extract->stackArgs.push_back(std::move(_bytes));
		auto u64 = awst::makeBtoi(std::move(extract), _loc);
		return awst::makeNumericCompare(
			std::move(u64), awst::NumericComparison::Ne,
			awst::makeIntegerConstant("0", _loc), _loc);
	}
	if (_wtype && _wtype->kind() == awst::WTypeKind::Bytes)
		return _bytes;
	return awst::makeReinterpretCast(std::move(_bytes), _wtype, _loc);
}

std::shared_ptr<awst::Statement> makeScratchStoreStmt(
	std::vector<FunctionSplitter::VarInfo> const& _liveOut,
	awst::SourceLocation const& _loc)
{
	std::shared_ptr<awst::Expression> payload;
	if (_liveOut.empty())
	{
		payload = awst::makeBytesConstant({}, _loc);
	}
	else
	{
		std::shared_ptr<awst::Expression> acc;
		for (auto const& lv : _liveOut)
		{
			auto var = awst::makeVarExpression(lv.name, lv.wtype, _loc);
			auto fixed = coerceToFixedBytes(var, lv.wtype, _loc);
			if (!acc)
				acc = fixed;
			else
				acc = awst::makeConcat(acc, fixed, _loc);
		}
		payload = acc;
	}

	auto store = awst::makeIntrinsicCall(
		"store", awst::WType::voidType(), _loc);
	store->immediates = {FunctionSplitter::kLiveVarsScratchSlot};
	store->stackArgs.push_back(std::move(payload));
	return awst::makeExpressionStatement(std::move(store), _loc);
}

std::vector<std::shared_ptr<awst::Statement>> makeScratchLoadStmts(
	std::vector<FunctionSplitter::VarInfo> const& _liveIn,
	int _prevCallTxnIndex,
	bool _crossChunk,
	awst::SourceLocation const& _loc)
{
	std::vector<std::shared_ptr<awst::Statement>> out;
	if (_liveIn.empty())
		return out;

	// Two read modes:
	//   - In-program (crossChunk=false): pieces share the same txn's
	//     scratch frame, so `load <slot>` reads what the previous piece
	//     stored.
	//   - Cross-chunk (crossChunk=true): pieces run as siblings inside
	//     orch.dispatch_chain's staged inner-txn group. Each piece
	//     reads the previous piece's scratch via `gload <prev_idx>
	//     <slot>` where `prev_idx` is the previous piece's call-txn
	//     position in the orch group (= 2N-1 for piece N, since orch
	//     interleaves install at 2N and call at 2N+1).
	std::string tmpName = "__uros_live_in";
	std::shared_ptr<awst::Expression> loadExpr;
	if (_crossChunk)
	{
		auto gload = awst::makeIntrinsicCall(
			"gload", awst::WType::bytesType(), _loc);
		gload->immediates = {_prevCallTxnIndex,
			FunctionSplitter::kLiveVarsScratchSlot};
		loadExpr = std::move(gload);
	}
	else
	{
		auto load = awst::makeIntrinsicCall(
			"load", awst::WType::bytesType(), _loc);
		load->immediates = {FunctionSplitter::kLiveVarsScratchSlot};
		loadExpr = std::move(load);
	}
	auto tmpTarget = awst::makeVarExpression(
		tmpName, awst::WType::bytesType(), _loc);
	out.push_back(awst::makeAssignmentStatement(
		tmpTarget, std::move(loadExpr), _loc));

	for (size_t i = 0; i < _liveIn.size(); ++i)
	{
		auto blob = awst::makeVarExpression(
			tmpName, awst::WType::bytesType(), _loc);
		auto extract = awst::makeIntrinsicCall(
			"extract3", awst::WType::bytesType(), _loc);
		extract->stackArgs.push_back(blob);
		extract->stackArgs.push_back(awst::makeIntegerConstant(
			std::to_string(i * kLiveVarBytes), _loc));
		extract->stackArgs.push_back(awst::makeIntegerConstant(
			std::to_string(kLiveVarBytes), _loc));
		auto value = coerceFromFixedBytes(extract, _liveIn[i].wtype, _loc);
		auto target = awst::makeVarExpression(
			_liveIn[i].name, _liveIn[i].wtype, _loc);
		out.push_back(awst::makeAssignmentStatement(
			std::move(target), std::move(value), _loc));
	}

	return out;
}

} // anonymous namespace

// ─── Public API: splitAt ─────────────────────────────────────────────

// A SplitTarget abstracts over Subroutine / ContractMethod so the
// slicing code below can treat them uniformly. We mutate body / args /
// returnType / name / id through pointer-to-member-like accessors so
// rewriting the original works for both shapes.
//
// `parentContract` is non-null only when method != nullptr; it lets the
// piece emitter add ContractMethod pieces back to the contract's
// `methods` list so puya can resolve them.
struct SplitTarget
{
	// Body, args, return type, name, id are all referenced via accessors
	// on the underlying node (Subroutine* or ContractMethod*). One of
	// the two pointers is set; the other is null.
	std::shared_ptr<awst::Subroutine> sub;
	awst::ContractMethod* method = nullptr;
	awst::Contract* parentContract = nullptr;
	awst::SourceLocation loc;

	std::shared_ptr<awst::Block>& body() {
		return sub ? sub->body : method->body;
	}
	std::vector<awst::SubroutineArgument> const& args() const {
		return sub ? sub->args : method->args;
	}
	awst::WType const* returnType() const {
		return sub ? sub->returnType : method->returnType;
	}
	std::string const& name() const {
		return sub ? sub->name : method->memberName;
	}
	std::string id() const {
		// Subroutine has explicit id; ContractMethod uses cref + ".method"
		// for callsub identification (puya's resolveContractMethod walks
		// MRO + cref to find).
		return sub ? sub->id : (method->cref + "." + method->memberName);
	}
	bool pure() const {
		return sub ? sub->pure : method->pure;
	}
	bool isContractMethod() const { return method != nullptr; }
};

FunctionSplitter::SplitResult FunctionSplitter::splitAt(
	std::vector<std::shared_ptr<awst::RootNode>>& _roots,
	std::vector<PieceSpec> const& _specs)
{
	auto& logger = Logger::instance();
	SplitResult result;

	// Build name → SplitTarget map. Both Subroutines (free / library
	// functions) and ContractMethods (members of a Contract root) are
	// targetable.
	std::map<std::string, SplitTarget> byName;
	for (auto const& root : _roots)
	{
		if (auto sub = std::dynamic_pointer_cast<awst::Subroutine>(root))
		{
			SplitTarget st;
			st.sub = sub;
			st.loc = sub->sourceLocation;
			byName[sub->name] = st;
		}
		else if (auto contract =
			std::dynamic_pointer_cast<awst::Contract>(root))
		{
			for (auto& m : contract->methods)
			{
				SplitTarget st;
				st.method = &m;
				st.parentContract = contract.get();
				st.loc = m.sourceLocation;
				// Name lookup uses the bare member name.
				byName[m.memberName] = st;
			}
		}
	}

	for (auto const& spec : _specs)
	{
		auto it = byName.find(spec.subroutineName);
		if (it == byName.end())
		{
			logger.warning(
				"--fn-split: subroutine '" + spec.subroutineName +
				"' not found in AWST roots; skipping");
			continue;
		}
		SplitTarget& tgt = it->second;
		if (!tgt.body())
		{
			logger.warning(
				"--fn-split: '" + spec.subroutineName +
				"' has no body; skipping");
			continue;
		}

		auto const& stmts = tgt.body()->body;
		if (spec.splitPoints.empty())
		{
			logger.warning(
				"--fn-split: '" + spec.subroutineName +
				"' has no split points; skipping");
			continue;
		}

		// Validate split points are in range and ascending.
		for (size_t i = 0; i < spec.splitPoints.size(); ++i)
		{
			if (spec.splitPoints[i] >= stmts.size())
			{
				logger.error(
					"--fn-split: '" + spec.subroutineName +
					"' split point " + std::to_string(spec.splitPoints[i]) +
					" is past end of body (" + std::to_string(stmts.size()) +
					" statements)");
				return result;
			}
			if (i > 0 && spec.splitPoints[i] <= spec.splitPoints[i - 1])
			{
				logger.error(
					"--fn-split: '" + spec.subroutineName +
					"' split points must be strictly ascending");
				return result;
			}
		}

		logger.info(
			"--fn-split: slicing '" + spec.subroutineName + "' (" +
			std::to_string(stmts.size()) + " stmts) into " +
			std::to_string(spec.splitPoints.size() + 1) +
			" pieces of group g" + std::to_string(spec.groupId));

		// Collect param names and seed m_varTypes from the args.
		std::set<std::string> paramNames;
		m_varTypes.clear();
		for (auto const& arg : tgt.args())
		{
			paramNames.insert(arg.name);
			m_varTypes[arg.name] = arg.wtype;
		}

		// Pre-walk the body once to populate m_varTypes for every
		// VarExpression — we need wtypes for live-vars before we know
		// which split point a var crosses.
		{
			std::set<std::string> dummy;
			for (auto const& s : stmts)
				if (s) collectStmtUses(*s, dummy);
		}

		// Compute live vars at each split point. liveAt[i] = vars live
		// across split point spec.splitPoints[i].
		std::vector<std::vector<VarInfo>> liveAt;
		for (size_t sp : spec.splitPoints)
			liveAt.push_back(computeLiveVars(stmts, sp, paramNames));

		// Build chunk ranges: [0, sp[0]), [sp[0], sp[1]), ..., [sp[N-1], end).
		std::vector<std::pair<size_t, size_t>> ranges;
		size_t prev = 0;
		for (size_t sp : spec.splitPoints)
		{
			ranges.push_back({prev, sp});
			prev = sp;
		}
		ranges.push_back({prev, stmts.size()});

		// Emit one piece per range. Piece SHAPE depends on target:
		//   - Target is a Subroutine: pieces are Subroutines, called via
		//     SubroutineID (callsub).
		//   - Target is a ContractMethod: pieces are ContractMethods on
		//     the SAME contract, called via InstanceMethodTarget. This
		//     matters because piece bodies may contain instance-method
		//     invocations (e.g. `this.helper()` → InstanceMethodTarget),
		//     which puya only accepts inside a ContractMethod context.
		std::string baseId = tgt.id();
		size_t numPieces = ranges.size();

		// Cache: needed for rewriting the original body without holding a
		// (possibly invalidated) tgt.method pointer once we push pieces.
		auto origLoc = tgt.loc;
		auto origArgs = tgt.args();
		auto* origReturnType = tgt.returnType();
		bool isContractMethod = tgt.isContractMethod();
		auto* parentContract = tgt.parentContract;
		std::string cref = isContractMethod ? tgt.method->cref : "";

		// Build all piece bodies first (heap allocations; no vector
		// invalidation concerns yet).
		struct BuiltPiece
		{
			std::shared_ptr<awst::Block> body;
			awst::WType const* returnType = nullptr;
			std::string id;
			std::string name;
		};
		std::vector<BuiltPiece> built;
		built.reserve(numPieces);
		for (size_t pi = 0; pi < numPieces; ++pi)
		{
			bool isFirst = (pi == 0);
			bool isLast = (pi == numPieces - 1);

			auto pieceBody = awst::makeBlock(origLoc);

			// Prologue: read live-vars from previous piece's scratch
			// (skip on piece 0). spec.crossChunk picks `gload`
			// (orch.dispatch_chain inner-txn group siblings) vs `load`
			// (same txn frame).
			if (!isFirst)
			{
				int prevCallTxnIdx = static_cast<int>(2 * pi - 1);
				for (auto& s : makeScratchLoadStmts(
					liveAt[pi - 1], prevCallTxnIdx,
					spec.crossChunk, origLoc))
				{
					pieceBody->body.push_back(std::move(s));
				}
			}

			// Body: original statements in this piece's range.
			for (size_t i = ranges[pi].first; i < ranges[pi].second; ++i)
				pieceBody->body.push_back(stmts[i]);

			// Epilogue: store live-vars to scratch slot 100 (skip on
			// last — ends with the original return statement instead).
			if (!isLast)
			{
				pieceBody->body.push_back(makeScratchStoreStmt(
					liveAt[pi], origLoc));
				pieceBody->body.push_back(awst::makeReturnStatement(
					nullptr, origLoc));
			}

			BuiltPiece bp;
			bp.body = pieceBody;
			bp.returnType = isLast
				? origReturnType
				: awst::WType::voidType();
			bp.id = baseId + "__piece_" + std::to_string(pi) +
				"_g" + std::to_string(spec.groupId);
			bp.name = pieceName(tgt.name(), pi, spec.groupId);
			built.push_back(std::move(bp));
		}

		// Rewrite the ORIGINAL function's body to dispatch to the
		// pieces sequentially. Pieces share the txn's scratch frame
		// (slot 100) so live vars cross piece boundaries transparently.
		// Without this rewrite the original still carries its full body,
		// which puya tries to compile and trips the 'h' format / 32 KB
		// branch limit that motivated splitting in the first place.
		auto newBody = awst::makeBlock(origLoc);
		for (size_t pi = 0; pi < numPieces; ++pi)
		{
			BuiltPiece const& bp = built[pi];

			auto call = std::make_shared<awst::SubroutineCallExpression>();
			call->sourceLocation = origLoc;
			call->wtype = bp.returnType;
			if (isContractMethod)
			{
				// Use InstanceMethodTarget — piece is a ContractMethod
				// on the same contract, callable on `this`.
				awst::InstanceMethodTarget imt;
				imt.memberName = bp.name;
				call->target = imt;
			}
			else
			{
				call->target = awst::SubroutineID{bp.id};
			}
			// Pass through the original args verbatim.
			for (auto const& arg : origArgs)
			{
				awst::CallArg ca;
				ca.name = arg.name;
				ca.value = awst::makeVarExpression(
					arg.name, arg.wtype, origLoc);
				call->args.push_back(std::move(ca));
			}

			bool isLast = (pi == numPieces - 1);
			if (isLast && bp.returnType != awst::WType::voidType())
			{
				newBody->body.push_back(awst::makeReturnStatement(
					std::move(call), origLoc));
			}
			else
			{
				newBody->body.push_back(awst::makeExpressionStatement(
					std::move(call), origLoc));
			}
		}
		// If the last piece returns void, append a bare return.
		if (origReturnType == awst::WType::voidType()
			&& !newBody->body.empty()
			&& newBody->body.back()->nodeType() != "ReturnStatement")
		{
			newBody->body.push_back(awst::makeReturnStatement(
				nullptr, origLoc));
		}
		tgt.body() = newBody; // safe: tgt.method pointer still valid

		// Now register the pieces — for ContractMethod target, push them
		// onto the parent contract's `methods` (this MAY invalidate
		// `tgt.method`, but we've already done all mutation we need on
		// it). For Subroutine target, return them via SplitResult so the
		// caller appends to roots.
		bool tgtPure = tgt.pure();
		for (auto const& bp : built)
		{
			if (isContractMethod)
			{
				awst::ContractMethod m;
				m.sourceLocation = origLoc;
				m.args = origArgs;
				m.returnType = bp.returnType;
				m.body = bp.body;
				m.cref = cref;
				m.memberName = bp.name;
				m.pure = tgtPure;
				if (spec.crossChunk)
				{
					// Cross-chunk pieces need to be reachable BY
					// SELECTOR from orch.dispatch_chain — orch's
					// staged inner-txn calls the chunk with
					// `app_args=(piece_sel, ...)` and expects the
					// chunk's ABI router to dispatch. Without an
					// arc4MethodConfig the piece is internal-only and
					// has no router slot.
					awst::ARC4ABIMethodConfig abi;
					abi.sourceLocation = origLoc;
					abi.allowedCompletionTypes = {0}; // NoOp only
					abi.create = 3;                   // Disallow
					abi.name = bp.name;
					m.arc4MethodConfig = abi;
				}
				// else: in-program callsub mode — pieces are
				// internal-only, invoked via InstanceMethodTarget
				// from the rewritten original method.
				parentContract->methods.push_back(std::move(m));
				++result.newContractMethodPieces;
			}
			else
			{
				auto piece = std::make_shared<awst::Subroutine>();
				piece->sourceLocation = origLoc;
				piece->id = bp.id;
				piece->name = bp.name;
				piece->args = origArgs;
				piece->returnType = bp.returnType;
				piece->pure = tgtPure;
				piece->inlineOpt = false;
				piece->body = bp.body;
				result.newSubroutines.push_back(piece);
			}
		}

		result.splitFunctions.insert(tgt.name());
		result.didSplit = true;
	}

	// Append all new pieces to roots.
	for (auto const& p : result.newSubroutines)
		_roots.push_back(p);

	return result;
}

} // namespace puyasol::splitter
