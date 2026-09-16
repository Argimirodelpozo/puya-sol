#include "awst/Visit.h"

#include <stdexcept>
#include <typeinfo>
#include <unordered_set>

namespace puyasol::awst
{
namespace
{

struct Traversal
{
	ExpressionVisitor const& visitor;
	mutable std::unordered_set<void const*> seen;
	void operator()(Expression const& node) const { visitor(node); }
};

void visitExpr(Expression const* _node, Traversal const& _visitor);
void visitStmt(Statement const* _node, Traversal const& _visitor);

// Per-node child traversal (one overload per PUYASOL_AWST_*_NODES entry).
// Leaves.
void visitChildren(IntegerConstant const&, Traversal const&) {}
void visitChildren(BoolConstant const&, Traversal const&) {}
void visitChildren(BytesConstant const&, Traversal const&) {}
void visitChildren(StringConstant const&, Traversal const&) {}
void visitChildren(VoidConstant const&, Traversal const&) {}
void visitChildren(VarExpression const&, Traversal const&) {}
void visitChildren(TemplateVar const&, Traversal const&) {}
void visitChildren(MethodConstant const&, Traversal const&) {}
void visitChildren(AddressConstant const&, Traversal const&) {}
void visitChildren(ARC4Router const&, Traversal const&) {}

void visitChildren(UInt64BinaryOperation const& _n, Traversal const& _v)
{
	visitExpr(_n.left.get(), _v);
	visitExpr(_n.right.get(), _v);
}
void visitChildren(BigUIntBinaryOperation const& _n, Traversal const& _v)
{
	visitExpr(_n.left.get(), _v);
	visitExpr(_n.right.get(), _v);
}
void visitChildren(BytesBinaryOperation const& _n, Traversal const& _v)
{
	visitExpr(_n.left.get(), _v);
	visitExpr(_n.right.get(), _v);
}
void visitChildren(BooleanBinaryOperation const& _n, Traversal const& _v)
{
	visitExpr(_n.left.get(), _v);
	visitExpr(_n.right.get(), _v);
}
void visitChildren(NumericComparisonExpression const& _n, Traversal const& _v)
{
	visitExpr(_n.lhs.get(), _v);
	visitExpr(_n.rhs.get(), _v);
}
void visitChildren(BytesComparisonExpression const& _n, Traversal const& _v)
{
	visitExpr(_n.lhs.get(), _v);
	visitExpr(_n.rhs.get(), _v);
}

void visitChildren(BytesUnaryOperation const& _n, Traversal const& _v)
{
	visitExpr(_n.expr.get(), _v);
}
void visitChildren(Not const& _n, Traversal const& _v) { visitExpr(_n.expr.get(), _v); }
void visitChildren(ReinterpretCast const& _n, Traversal const& _v) { visitExpr(_n.expr.get(), _v); }
void visitChildren(ConvertArray const& _n, Traversal const& _v) { visitExpr(_n.expr.get(), _v); }
void visitChildren(CheckedMaybe const& _n, Traversal const& _v) { visitExpr(_n.expr.get(), _v); }
void visitChildren(Copy const& _n, Traversal const& _v) { visitExpr(_n.value.get(), _v); }
void visitChildren(SingleEvaluation const& _n, Traversal const& _v)
{
	visitExpr(_n.source.get(), _v);
}
void visitChildren(ARC4Encode const& _n, Traversal const& _v) { visitExpr(_n.value.get(), _v); }
void visitChildren(ARC4Decode const& _n, Traversal const& _v) { visitExpr(_n.value.get(), _v); }
void visitChildren(ARC4FromBytes const& _n, Traversal const& _v) { visitExpr(_n.value.get(), _v); }
void visitChildren(Emit const& _n, Traversal const& _v) { visitExpr(_n.value.get(), _v); }
void visitChildren(ArrayLength const& _n, Traversal const& _v) { visitExpr(_n.array.get(), _v); }
void visitChildren(ArrayPop const& _n, Traversal const& _v) { visitExpr(_n.base.get(), _v); }
void visitChildren(TupleItemExpression const& _n, Traversal const& _v)
{
	visitExpr(_n.base.get(), _v);
}
void visitChildren(FieldExpression const& _n, Traversal const& _v) { visitExpr(_n.base.get(), _v); }
void visitChildren(AssertExpression const& _n, Traversal const& _v)
{
	visitExpr(_n.condition.get(), _v);
}

void visitChildren(IndexExpression const& _n, Traversal const& _v)
{
	visitExpr(_n.base.get(), _v);
	visitExpr(_n.index.get(), _v);
}
void visitChildren(ArrayConcat const& _n, Traversal const& _v)
{
	visitExpr(_n.left.get(), _v);
	visitExpr(_n.right.get(), _v);
}
void visitChildren(ArrayExtend const& _n, Traversal const& _v)
{
	visitExpr(_n.base.get(), _v);
	visitExpr(_n.other.get(), _v);
}
void visitChildren(AssignmentExpression const& _n, Traversal const& _v)
{
	visitExpr(_n.target.get(), _v);
	visitExpr(_n.value.get(), _v);
}

void visitChildren(ConditionalExpression const& _n, Traversal const& _v)
{
	visitExpr(_n.condition.get(), _v);
	visitExpr(_n.trueExpr.get(), _v);
	visitExpr(_n.falseExpr.get(), _v);
}

void visitChildren(TupleExpression const& _n, Traversal const& _v)
{
	for (auto const& item: _n.items) visitExpr(item.get(), _v);
}
void visitChildren(NewArray const& _n, Traversal const& _v)
{
	for (auto const& value: _n.values) visitExpr(value.get(), _v);
}
void visitChildren(IntrinsicCall const& _n, Traversal const& _v)
{
	for (auto const& arg: _n.stackArgs) visitExpr(arg.get(), _v);
}
void visitChildren(CommaExpression const& _n, Traversal const& _v)
{
	for (auto const& item: _n.expressions) visitExpr(item.get(), _v);
}
void visitChildren(SubmitInnerTransaction const& _n, Traversal const& _v)
{
	for (auto const& txn: _n.itxns) visitExpr(txn.get(), _v);
}
void visitChildren(SubroutineCallExpression const& _n, Traversal const& _v)
{
	for (auto const& arg: _n.args) visitExpr(arg.value.get(), _v);
}
void visitChildren(PuyaLibCall const& _n, Traversal const& _v)
{
	for (auto const& arg: _n.args) visitExpr(arg.value.get(), _v);
}

void visitChildren(NewStruct const& _n, Traversal const& _v)
{
	for (auto const& [_, value]: _n.values) visitExpr(value.get(), _v);
}
void visitChildren(NamedTupleExpression const& _n, Traversal const& _v)
{
	for (auto const& [_, value]: _n.values) visitExpr(value.get(), _v);
}
void visitChildren(CreateInnerTransaction const& _n, Traversal const& _v)
{
	for (auto const& [_, value]: _n.fields) visitExpr(value.get(), _v);
}

void visitChildren(AppStateExpression const& _n, Traversal const& _v) { visitExpr(_n.key.get(), _v); }
void visitChildren(AppAccountStateExpression const& _n, Traversal const& _v)
{
	visitExpr(_n.key.get(), _v);
	visitExpr(_n.account.get(), _v);
}
void visitChildren(BoxValueExpression const& _n, Traversal const& _v) { visitExpr(_n.key.get(), _v); }
void visitChildren(StateGet const& _n, Traversal const& _v)
{
	visitExpr(_n.field.get(), _v);
	visitExpr(_n.defaultValue.get(), _v);
}
void visitChildren(StateGetEx const& _n, Traversal const& _v) { visitExpr(_n.field.get(), _v); }
void visitChildren(StateExists const& _n, Traversal const& _v) { visitExpr(_n.field.get(), _v); }
void visitChildren(StateDelete const& _n, Traversal const& _v) { visitExpr(_n.field.get(), _v); }
void visitChildren(InnerTransactionField const& _n, Traversal const& _v)
{
	visitExpr(_n.itxn.get(), _v);
	visitExpr(_n.arrayIndex.get(), _v);
}

void visitChildren(Block const& _n, Traversal const& _v)
{
	for (auto const& statement: _n.body) visitStmt(statement.get(), _v);
}
void visitChildren(ExpressionStatement const& _n, Traversal const& _v)
{
	visitExpr(_n.expr.get(), _v);
}
void visitChildren(ReturnStatement const& _n, Traversal const& _v) { visitExpr(_n.value.get(), _v); }
void visitChildren(AssignmentStatement const& _n, Traversal const& _v)
{
	visitExpr(_n.target.get(), _v);
	visitExpr(_n.value.get(), _v);
}
void visitChildren(IfElse const& _n, Traversal const& _v)
{
	visitExpr(_n.condition.get(), _v);
	visitStmt(_n.ifBranch.get(), _v);
	visitStmt(_n.elseBranch.get(), _v);
}
void visitChildren(WhileLoop const& _n, Traversal const& _v)
{
	visitExpr(_n.condition.get(), _v);
	visitStmt(_n.loopBody.get(), _v);
}
void visitChildren(ForInLoop const& _n, Traversal const& _v)
{
	visitExpr(_n.sequence.get(), _v);
	visitExpr(_n.items.get(), _v);
	visitStmt(_n.loopBody.get(), _v);
}
void visitChildren(Switch const& _n, Traversal const& _v)
{
	visitExpr(_n.value.get(), _v);
	for (auto const& [value, block]: _n.cases)
	{
		visitExpr(value.get(), _v);
		visitStmt(block.get(), _v);
	}
	visitStmt(_n.defaultCase.get(), _v);
}
void visitChildren(UInt64AugmentedAssignment const& _n, Traversal const& _v)
{
	visitExpr(_n.target.get(), _v);
	visitExpr(_n.value.get(), _v);
}
void visitChildren(BigUIntAugmentedAssignment const& _n, Traversal const& _v)
{
	visitExpr(_n.target.get(), _v);
	visitExpr(_n.value.get(), _v);
}
void visitChildren(Goto const&, Traversal const&) {}
void visitChildren(LoopExit const&, Traversal const&) {}
void visitChildren(LoopContinue const&, Traversal const&) {}

#define PUYASOL_AWST_VISIT_ARM(Node) \
	if (auto const* node = dynamic_cast<Node const*>(_node)) \
	{ \
		visitChildren(*node, _visitor); \
		return; \
	}

void visitExpr(Expression const* _node, Traversal const& _visitor)
{
	if (!_node || !_visitor.seen.insert(_node).second)
		return;
	_visitor(*_node);

	PUYASOL_AWST_EXPRESSION_NODES(PUYASOL_AWST_VISIT_ARM)

	throw std::logic_error(
		std::string("unhandled AWST expression in const visitor: ")
		+ typeid(*_node).name());
}

void visitStmt(Statement const* _node, Traversal const& _visitor)
{
	if (!_node || !_visitor.seen.insert(_node).second)
		return;

	PUYASOL_AWST_STATEMENT_NODES(PUYASOL_AWST_VISIT_ARM)

	throw std::logic_error(
		std::string("unhandled AWST statement in const visitor: ")
		+ typeid(*_node).name());
}

#undef PUYASOL_AWST_VISIT_ARM

} // namespace

void visitExpressions(Statement const& _statement, ExpressionVisitor const& _visitor)
{
	visitStmt(&_statement, Traversal{_visitor, {}});
}

void visitExpressions(ContractMethod const& _method, ExpressionVisitor const& _visitor)
{
	visitStmt(_method.body.get(), Traversal{_visitor, {}});
}

void visitExpressions(Statement& _statement, MutableExpressionVisitor const& _visitor)
{
	visitExpressions(static_cast<Statement const&>(_statement),
		[&_visitor](Expression const& expression) {
			_visitor(const_cast<Expression&>(expression));
		});
}

void visitExpressions(ContractMethod& _method, MutableExpressionVisitor const& _visitor)
{
	if (_method.body)
		visitExpressions(*_method.body, _visitor);
}

} // namespace puyasol::awst
