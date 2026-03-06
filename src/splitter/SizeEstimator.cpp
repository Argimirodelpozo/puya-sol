#include "splitter/SizeEstimator.h"
#include "Logger.h"

namespace puyasol::splitter
{

SizeEstimator::Estimate SizeEstimator::estimate(
	awst::Contract const& _contract,
	std::vector<std::shared_ptr<awst::RootNode>> const& _subroutines
)
{
	Estimate result;

	// Estimate approval program
	size_t approvalSize = estimateMethod(_contract.approvalProgram);
	result.methodSizes["__approval__"] = approvalSize;
	result.totalInstructions += approvalSize;

	// Estimate clear program
	size_t clearSize = estimateMethod(_contract.clearProgram);
	result.methodSizes["__clear__"] = clearSize;
	result.totalInstructions += clearSize;

	// Estimate each contract method
	for (auto const& method: _contract.methods)
	{
		size_t methodSize = estimateMethod(method);
		result.methodSizes[method.memberName] = methodSize;
		result.totalInstructions += methodSize;
	}

	// Estimate subroutines (free functions, library functions)
	for (auto const& root: _subroutines)
	{
		if (auto const* sub = dynamic_cast<awst::Subroutine const*>(root.get()))
		{
			size_t subSize = 0;
			if (sub->body)
				subSize = estimateBlock(*sub->body);
			// Add overhead for callsub/retsub frame
			subSize += 3;
			// Add ABI codec overhead for chunk subroutines (they become ABI methods
			// when placed in helpers, so puya generates arg decode + return encode code)
			if (sub->name.find("__chunk_") != std::string::npos)
				subSize += estimateABICodecCost(*sub);
			result.methodSizes[sub->name] = subSize;
			result.totalInstructions += subSize;
		}
	}

	// Approximate bytes: ~3 bytes per instruction on average.
	// Simple opcodes are 1 byte, but pushbytes/pushint for constants,
	// ARC4 routing code, and ABI codec code inflate the real ratio.
	result.estimatedBytes = result.totalInstructions * 3;

	return result;
}

size_t SizeEstimator::estimateExpression(awst::Expression const& _expr)
{
	std::string type = _expr.nodeType();

	// Constants: cost depends on encoded size
	if (type == "IntegerConstant")
	{
		auto const& ic = static_cast<awst::IntegerConstant const&>(_expr);
		// biguint constants are pushbytes of 32-64 bytes
		if (ic.wtype && ic.wtype->name() == "biguint")
			return 18; // pushbytes 32 bytes = ~34 bytecode bytes ≈ 17 units
		return 2;
	}
	if (type == "BoolConstant")
		return 1;
	if (type == "BytesConstant")
	{
		auto const& bc = static_cast<awst::BytesConstant const&>(_expr);
		// pushbytes bytecode = 1 (opcode) + varint(len) + len bytes.
		// At ~2 bytes per instruction unit, cost ≈ 2 + byteSize/2.
		size_t byteSize = bc.value.size();
		if (byteSize <= 64)
			return 2;
		return 2 + byteSize / 2;
	}
	if (type == "StringConstant")
		return 2;
	if (type == "VoidConstant")
		return 0;
	if (type == "AddressConstant")
		return 2;
	if (type == "MethodConstant")
		return 2;

	// Variables: 1 instruction (frame_dig / load)
	if (type == "VarExpression")
		return 1;

	// Binary operations
	if (type == "UInt64BinaryOperation")
	{
		auto const& op = static_cast<awst::UInt64BinaryOperation const&>(_expr);
		return estimateExpression(*op.left) + estimateExpression(*op.right) + 1;
	}
	if (type == "BigUIntBinaryOperation")
	{
		auto const& op = static_cast<awst::BigUIntBinaryOperation const&>(_expr);
		// biguint ops: 8-15 TEAL instructions per operation
		// (b*/b+/b- + b% modulus + pushbytes modulus constant + concat/extract
		//  padding + dig/uncover stack shuffling)
		return estimateExpression(*op.left) + estimateExpression(*op.right) + 10;
	}
	if (type == "BytesBinaryOperation")
	{
		auto const& op = static_cast<awst::BytesBinaryOperation const&>(_expr);
		return estimateExpression(*op.left) + estimateExpression(*op.right) + 1;
	}
	if (type == "BytesUnaryOperation")
	{
		auto const& op = static_cast<awst::BytesUnaryOperation const&>(_expr);
		return estimateExpression(*op.expr) + 1;
	}

	// Comparisons
	if (type == "NumericComparisonExpression")
	{
		auto const& cmp = static_cast<awst::NumericComparisonExpression const&>(_expr);
		return estimateExpression(*cmp.lhs) + estimateExpression(*cmp.rhs) + 1;
	}
	if (type == "BytesComparisonExpression")
	{
		auto const& cmp = static_cast<awst::BytesComparisonExpression const&>(_expr);
		return estimateExpression(*cmp.lhs) + estimateExpression(*cmp.rhs) + 1;
	}

	// Boolean operations
	if (type == "BooleanBinaryOperation")
	{
		auto const& op = static_cast<awst::BooleanBinaryOperation const&>(_expr);
		// Short-circuit: condition + bnz/bz + other branch
		return estimateExpression(*op.left) + estimateExpression(*op.right) + 3;
	}
	if (type == "Not")
	{
		auto const& n = static_cast<awst::Not const&>(_expr);
		return estimateExpression(*n.expr) + 1;
	}

	// Assertions
	if (type == "AssertExpression")
	{
		auto const& a = static_cast<awst::AssertExpression const&>(_expr);
		return estimateExpression(*a.condition) + 2; // assert opcode
	}

	// Assignments
	if (type == "AssignmentExpression")
	{
		auto const& a = static_cast<awst::AssignmentExpression const&>(_expr);
		return estimateExpression(*a.target) + estimateExpression(*a.value) + 1;
	}

	// Conditional (ternary)
	if (type == "ConditionalExpression")
	{
		auto const& c = static_cast<awst::ConditionalExpression const&>(_expr);
		return estimateExpression(*c.condition)
			+ estimateExpression(*c.trueExpr)
			+ estimateExpression(*c.falseExpr) + 4; // condition + bnz + b
	}

	// Subroutine call
	if (type == "SubroutineCallExpression")
	{
		auto const& call = static_cast<awst::SubroutineCallExpression const&>(_expr);
		size_t cost = 2; // callsub + overhead
		for (auto const& arg: call.args)
			if (arg.value)
				cost += estimateExpression(*arg.value);
		return cost;
	}

	// Intrinsic call: 1-3 instructions
	if (type == "IntrinsicCall")
	{
		auto const& ic = static_cast<awst::IntrinsicCall const&>(_expr);
		size_t cost = 1; // the opcode itself
		for (auto const& arg: ic.stackArgs)
			if (arg)
				cost += estimateExpression(*arg);
		return cost;
	}

	// PuyaLibCall: acts like a subroutine call
	if (type == "PuyaLibCall")
	{
		auto const& plc = static_cast<awst::PuyaLibCall const&>(_expr);
		size_t cost = 5; // ensure_budget and other puya_lib calls have overhead
		for (auto const& arg: plc.args)
			if (arg.value)
				cost += estimateExpression(*arg.value);
		return cost;
	}

	// Field / Index access
	if (type == "FieldExpression")
	{
		auto const& f = static_cast<awst::FieldExpression const&>(_expr);
		return estimateExpression(*f.base) + 2; // extract field
	}
	if (type == "IndexExpression")
	{
		auto const& idx = static_cast<awst::IndexExpression const&>(_expr);
		return estimateExpression(*idx.base) + estimateExpression(*idx.index) + 3;
	}

	// Tuples
	if (type == "TupleExpression")
	{
		auto const& t = static_cast<awst::TupleExpression const&>(_expr);
		size_t cost = 0;
		for (auto const& item: t.items)
			if (item)
				cost += estimateExpression(*item);
		return cost + 1;
	}
	if (type == "TupleItemExpression")
	{
		auto const& ti = static_cast<awst::TupleItemExpression const&>(_expr);
		return estimateExpression(*ti.base) + 1;
	}

	// ARC4 encode/decode
	if (type == "ARC4Encode")
	{
		auto const& e = static_cast<awst::ARC4Encode const&>(_expr);
		return estimateExpression(*e.value) + 3;
	}
	if (type == "ARC4Decode")
	{
		auto const& d = static_cast<awst::ARC4Decode const&>(_expr);
		return estimateExpression(*d.value) + 3;
	}
	if (type == "ARC4Router")
		return 20; // routing logic

	// Casts and copies
	if (type == "ReinterpretCast")
	{
		auto const& rc = static_cast<awst::ReinterpretCast const&>(_expr);
		return estimateExpression(*rc.expr); // no-op at TEAL level usually
	}
	if (type == "Copy")
	{
		auto const& c = static_cast<awst::Copy const&>(_expr);
		return estimateExpression(*c.value) + 1;
	}
	if (type == "SingleEvaluation")
	{
		auto const& se = static_cast<awst::SingleEvaluation const&>(_expr);
		return estimateExpression(*se.source) + 2; // dup + store temp
	}
	if (type == "CheckedMaybe")
	{
		auto const& cm = static_cast<awst::CheckedMaybe const&>(_expr);
		return estimateExpression(*cm.expr) + 2; // assert exists
	}

	// Array operations
	if (type == "NewArray")
	{
		auto const& na = static_cast<awst::NewArray const&>(_expr);
		size_t cost = 2;
		for (auto const& v: na.values)
			if (v)
				cost += estimateExpression(*v) + 1;
		return cost;
	}
	if (type == "ArrayLength")
	{
		auto const& al = static_cast<awst::ArrayLength const&>(_expr);
		return estimateExpression(*al.array) + 2;
	}
	if (type == "ArrayPop")
	{
		auto const& ap = static_cast<awst::ArrayPop const&>(_expr);
		return estimateExpression(*ap.base) + 4;
	}
	if (type == "ArrayConcat")
	{
		auto const& ac = static_cast<awst::ArrayConcat const&>(_expr);
		return estimateExpression(*ac.left) + estimateExpression(*ac.right) + 2;
	}
	if (type == "ArrayExtend")
	{
		auto const& ae = static_cast<awst::ArrayExtend const&>(_expr);
		return estimateExpression(*ae.base) + estimateExpression(*ae.other) + 2;
	}

	// Storage operations
	if (type == "StateGet")
	{
		auto const& sg = static_cast<awst::StateGet const&>(_expr);
		size_t cost = 4; // box_get/app_global_get + default handling
		if (sg.field)
			cost += estimateExpression(*sg.field);
		if (sg.defaultValue)
			cost += estimateExpression(*sg.defaultValue);
		return cost;
	}
	if (type == "StateExists")
	{
		auto const& se = static_cast<awst::StateExists const&>(_expr);
		return (se.field ? estimateExpression(*se.field) : 0) + 3;
	}
	if (type == "StateDelete")
	{
		auto const& sd = static_cast<awst::StateDelete const&>(_expr);
		return (sd.field ? estimateExpression(*sd.field) : 0) + 2;
	}
	if (type == "StateGetEx")
	{
		auto const& sge = static_cast<awst::StateGetEx const&>(_expr);
		return (sge.field ? estimateExpression(*sge.field) : 0) + 4;
	}
	if (type == "AppStateExpression" || type == "AppAccountStateExpression")
		return 3;
	if (type == "BoxValueExpression")
		return 4;
	if (type == "BoxPrefixedKeyExpression")
	{
		auto const& bpk = static_cast<awst::BoxPrefixedKeyExpression const&>(_expr);
		size_t cost = 2;
		if (bpk.prefix)
			cost += estimateExpression(*bpk.prefix);
		if (bpk.key)
			cost += estimateExpression(*bpk.key);
		return cost;
	}

	// Structs
	if (type == "NewStruct")
	{
		auto const& ns = static_cast<awst::NewStruct const&>(_expr);
		size_t cost = 1;
		for (auto const& [_, val]: ns.values)
			if (val)
				cost += estimateExpression(*val) + 1;
		return cost;
	}
	if (type == "NamedTupleExpression")
	{
		auto const& nt = static_cast<awst::NamedTupleExpression const&>(_expr);
		size_t cost = 1;
		for (auto const& [_, val]: nt.values)
			if (val)
				cost += estimateExpression(*val) + 1;
		return cost;
	}

	// Emit
	if (type == "Emit")
	{
		auto const& e = static_cast<awst::Emit const&>(_expr);
		return (e.value ? estimateExpression(*e.value) : 0) + 5;
	}

	// Inner transactions
	if (type == "CreateInnerTransaction")
	{
		auto const& cit = static_cast<awst::CreateInnerTransaction const&>(_expr);
		size_t cost = 3;
		for (auto const& [_, val]: cit.fields)
			if (val)
				cost += estimateExpression(*val) + 1;
		return cost;
	}
	if (type == "SubmitInnerTransaction")
	{
		auto const& sit = static_cast<awst::SubmitInnerTransaction const&>(_expr);
		size_t cost = 2;
		for (auto const& itxn: sit.itxns)
			if (itxn)
				cost += estimateExpression(*itxn);
		return cost;
	}
	if (type == "InnerTransactionField")
	{
		auto const& itf = static_cast<awst::InnerTransactionField const&>(_expr);
		return (itf.itxn ? estimateExpression(*itf.itxn) : 0) + 2;
	}

	// Comma expression
	if (type == "CommaExpression")
	{
		auto const& ce = static_cast<awst::CommaExpression const&>(_expr);
		size_t cost = 0;
		for (auto const& e: ce.expressions)
			if (e)
				cost += estimateExpression(*e);
		return cost;
	}

	// Unknown node type — conservative estimate
	return 3;
}

size_t SizeEstimator::estimateStatement(awst::Statement const& _stmt)
{
	std::string type = _stmt.nodeType();

	if (type == "Block")
		return estimateBlock(static_cast<awst::Block const&>(_stmt));

	if (type == "ExpressionStatement")
	{
		auto const& es = static_cast<awst::ExpressionStatement const&>(_stmt);
		return es.expr ? estimateExpression(*es.expr) : 0;
	}

	if (type == "ReturnStatement")
	{
		auto const& rs = static_cast<awst::ReturnStatement const&>(_stmt);
		return (rs.value ? estimateExpression(*rs.value) : 0) + 1; // retsub
	}

	if (type == "IfElse")
	{
		auto const& ie = static_cast<awst::IfElse const&>(_stmt);
		size_t cost = estimateExpression(*ie.condition) + 3; // bnz/bz + jumps
		if (ie.ifBranch)
			cost += estimateBlock(*ie.ifBranch);
		if (ie.elseBranch)
			cost += estimateBlock(*ie.elseBranch);
		return cost;
	}

	if (type == "WhileLoop")
	{
		auto const& wl = static_cast<awst::WhileLoop const&>(_stmt);
		size_t cost = estimateExpression(*wl.condition) + 4; // condition + bnz + b
		if (wl.loopBody)
			cost += estimateBlock(*wl.loopBody);
		return cost;
	}

	if (type == "LoopExit")
		return 1; // b (jump out)

	if (type == "LoopContinue")
		return 1; // b (jump back)

	if (type == "AssignmentStatement")
	{
		auto const& as = static_cast<awst::AssignmentStatement const&>(_stmt);
		size_t cost = 1; // store/frame_bury
		if (as.target)
			cost += estimateExpression(*as.target);
		if (as.value)
			cost += estimateExpression(*as.value);
		return cost;
	}

	if (type == "Goto")
		return 1;

	if (type == "Switch")
	{
		auto const& sw = static_cast<awst::Switch const&>(_stmt);
		size_t cost = estimateExpression(*sw.value) + 2;
		for (auto const& [caseExpr, caseBlock]: sw.cases)
		{
			if (caseExpr)
				cost += estimateExpression(*caseExpr) + 2; // comparison + bnz
			if (caseBlock)
				cost += estimateBlock(*caseBlock);
		}
		if (sw.defaultCase)
			cost += estimateBlock(*sw.defaultCase);
		return cost;
	}

	if (type == "ForInLoop")
	{
		auto const& fil = static_cast<awst::ForInLoop const&>(_stmt);
		size_t cost = 6; // loop setup + iteration overhead
		if (fil.sequence)
			cost += estimateExpression(*fil.sequence);
		if (fil.items)
			cost += estimateExpression(*fil.items);
		if (fil.loopBody)
			cost += estimateBlock(*fil.loopBody);
		return cost;
	}

	if (type == "UInt64AugmentedAssignment")
	{
		auto const& ua = static_cast<awst::UInt64AugmentedAssignment const&>(_stmt);
		size_t cost = 2; // load + op + store
		if (ua.target)
			cost += estimateExpression(*ua.target);
		if (ua.value)
			cost += estimateExpression(*ua.value);
		return cost;
	}

	if (type == "BigUIntAugmentedAssignment")
	{
		auto const& ba = static_cast<awst::BigUIntAugmentedAssignment const&>(_stmt);
		size_t cost = 4; // load + biguint op + store
		if (ba.target)
			cost += estimateExpression(*ba.target);
		if (ba.value)
			cost += estimateExpression(*ba.value);
		return cost;
	}

	// Unknown statement — conservative
	return 2;
}

size_t SizeEstimator::estimateBlock(awst::Block const& _block)
{
	size_t cost = 0;
	for (auto const& stmt: _block.body)
		if (stmt)
			cost += estimateStatement(*stmt);
	return cost;
}

size_t SizeEstimator::estimateMethod(awst::ContractMethod const& _method)
{
	size_t cost = 3; // proto/retsub overhead + ARC4 routing dispatch
	if (_method.arc4MethodConfig)
		cost += 5; // ARC4 method selector matching

	// Arguments setup
	cost += _method.args.size();

	if (_method.body)
		cost += estimateBlock(*_method.body);

	return cost;
}

/// Helper: estimate the ABI-encoded byte size of a WType.
/// This approximates how many bytes of ARC4-encoded data the type needs
/// (not bytecode — raw data bytes).
size_t SizeEstimator::estimateABIEncodedSize(awst::WType const* _wtype)
{
	if (!_wtype)
		return 64; // default biguint

	switch (_wtype->kind())
	{
	case awst::WTypeKind::Basic:
		if (_wtype->name() == "biguint")
			return 32; // uint256 in ARC4
		if (_wtype->name() == "uint64" || _wtype->name() == "bool")
			return 8;
		return 64;
	case awst::WTypeKind::ARC4UIntN:
	{
		auto const* uintN = dynamic_cast<awst::ARC4UIntN const*>(_wtype);
		if (uintN)
			return uintN->n() / 8; // uint256 → 32, uint128 → 16, etc.
		return 32;
	}
	case awst::WTypeKind::Bytes:
	{
		auto const* bt = dynamic_cast<awst::BytesWType const*>(_wtype);
		if (bt && bt->length())
			return *bt->length();
		return 64;
	}
	case awst::WTypeKind::ReferenceArray:
	{
		auto const* ra = dynamic_cast<awst::ReferenceArray const*>(_wtype);
		if (ra && ra->arraySize())
			return *ra->arraySize() * estimateABIEncodedSize(ra->elementType());
		return 2816; // default: 44 * 64
	}
	case awst::WTypeKind::ARC4StaticArray:
	{
		auto const* sa = dynamic_cast<awst::ARC4StaticArray const*>(_wtype);
		if (sa)
			return sa->arraySize() * estimateABIEncodedSize(sa->elementType());
		return 2816;
	}
	case awst::WTypeKind::WTuple:
	{
		auto const* tup = dynamic_cast<awst::WTuple const*>(_wtype);
		if (tup)
		{
			size_t total = 0;
			for (auto const* t: tup->types())
				total += estimateABIEncodedSize(t);
			return total;
		}
		return 128;
	}
	case awst::WTypeKind::ARC4Tuple:
	{
		auto const* tup = dynamic_cast<awst::ARC4Tuple const*>(_wtype);
		if (tup)
		{
			size_t total = 0;
			for (auto const* t: tup->types())
				total += estimateABIEncodedSize(t);
			return total;
		}
		return 128;
	}
	case awst::WTypeKind::ARC4Struct:
	{
		auto const* st = dynamic_cast<awst::ARC4Struct const*>(_wtype);
		if (st)
		{
			size_t total = 0;
			for (auto const& [_, t]: st->fields())
				total += estimateABIEncodedSize(t);
			return total;
		}
		return 128;
	}
	default:
		return 64;
	}
}

size_t SizeEstimator::estimateABICodecCost(awst::Subroutine const& _sub)
{
	size_t cost = 0;

	// ARC4 routing overhead: method selector match, txn ApplicationArgs, etc.
	cost += 15;

	// Per-argument decode cost: txna ApplicationArgs + length validation + extraction
	for (auto const& arg: _sub.args)
	{
		size_t encodedSize = estimateABIEncodedSize(arg.wtype);

		// Base decode: txna + len + == + assert = 4 instructions
		cost += 4;

		// Extract cost: each 64-byte element needs ~3 instructions (pushint offset + extract3)
		// Plus overhead for stack management (dig/uncover)
		size_t numElements = (encodedSize + 63) / 64;
		cost += numElements * 4;
	}

	// Return value encode cost: similar to decode
	if (_sub.returnType && _sub.returnType != awst::WType::voidType())
	{
		size_t returnSize = estimateABIEncodedSize(_sub.returnType);
		size_t numElements = (returnSize + 63) / 64;
		// concat per element + log prefix + return
		cost += numElements * 4 + 5;
	}

	return cost;
}

} // namespace puyasol::splitter
