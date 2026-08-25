#include "awst/Visit.h"

#include <stdexcept>
#include <typeinfo>

namespace puyasol::awst
{
namespace
{

void visitExpr(Expression const* _expression, ExpressionVisitor const& _visitor);
void visitStmt(Statement const* _statement, ExpressionVisitor const& _visitor);

void visitExpr(Expression const* _expression, ExpressionVisitor const& _visitor)
{
	if (!_expression)
		return;
	_visitor(*_expression);

	// Leaves.
	if (dynamic_cast<IntegerConstant const*>(_expression)) return;
	if (dynamic_cast<BoolConstant const*>(_expression)) return;
	if (dynamic_cast<BytesConstant const*>(_expression)) return;
	if (dynamic_cast<StringConstant const*>(_expression)) return;
	if (dynamic_cast<VoidConstant const*>(_expression)) return;
	if (dynamic_cast<VarExpression const*>(_expression)) return;
	if (dynamic_cast<TemplateVar const*>(_expression)) return;
	if (dynamic_cast<MethodConstant const*>(_expression)) return;
	if (dynamic_cast<AddressConstant const*>(_expression)) return;
	if (dynamic_cast<ARC4Router const*>(_expression)) return;

	auto one = [&](auto const* _node, auto const& _child) {
		if (!_node) return false;
		visitExpr((_node->*_child).get(), _visitor);
		return true;
	};
	auto two = [&](auto const* _node, auto const& _left, auto const& _right) {
		if (!_node) return false;
		visitExpr((_node->*_left).get(), _visitor);
		visitExpr((_node->*_right).get(), _visitor);
		return true;
	};

	if (two(dynamic_cast<UInt64BinaryOperation const*>(_expression),
		&UInt64BinaryOperation::left, &UInt64BinaryOperation::right)) return;
	if (two(dynamic_cast<BigUIntBinaryOperation const*>(_expression),
		&BigUIntBinaryOperation::left, &BigUIntBinaryOperation::right)) return;
	if (two(dynamic_cast<BytesBinaryOperation const*>(_expression),
		&BytesBinaryOperation::left, &BytesBinaryOperation::right)) return;
	if (two(dynamic_cast<BooleanBinaryOperation const*>(_expression),
		&BooleanBinaryOperation::left, &BooleanBinaryOperation::right)) return;
	if (two(dynamic_cast<NumericComparisonExpression const*>(_expression),
		&NumericComparisonExpression::lhs, &NumericComparisonExpression::rhs)) return;
	if (two(dynamic_cast<BytesComparisonExpression const*>(_expression),
		&BytesComparisonExpression::lhs, &BytesComparisonExpression::rhs)) return;

	if (one(dynamic_cast<BytesUnaryOperation const*>(_expression), &BytesUnaryOperation::expr)) return;
	if (one(dynamic_cast<Not const*>(_expression), &Not::expr)) return;
	if (one(dynamic_cast<ReinterpretCast const*>(_expression), &ReinterpretCast::expr)) return;
	if (one(dynamic_cast<ConvertArray const*>(_expression), &ConvertArray::expr)) return;
	if (one(dynamic_cast<CheckedMaybe const*>(_expression), &CheckedMaybe::expr)) return;
	if (one(dynamic_cast<Copy const*>(_expression), &Copy::value)) return;
	if (one(dynamic_cast<SingleEvaluation const*>(_expression), &SingleEvaluation::source)) return;
	if (one(dynamic_cast<ARC4Encode const*>(_expression), &ARC4Encode::value)) return;
	if (one(dynamic_cast<ARC4Decode const*>(_expression), &ARC4Decode::value)) return;
	if (one(dynamic_cast<ARC4FromBytes const*>(_expression), &ARC4FromBytes::value)) return;
	if (one(dynamic_cast<Emit const*>(_expression), &Emit::value)) return;
	if (one(dynamic_cast<ArrayLength const*>(_expression), &ArrayLength::array)) return;
	if (one(dynamic_cast<ArrayPop const*>(_expression), &ArrayPop::base)) return;
	if (one(dynamic_cast<TupleItemExpression const*>(_expression), &TupleItemExpression::base)) return;
	if (one(dynamic_cast<FieldExpression const*>(_expression), &FieldExpression::base)) return;
	if (one(dynamic_cast<AssertExpression const*>(_expression), &AssertExpression::condition)) return;

	if (two(dynamic_cast<IndexExpression const*>(_expression),
		&IndexExpression::base, &IndexExpression::index)) return;
	if (two(dynamic_cast<ArrayConcat const*>(_expression),
		&ArrayConcat::left, &ArrayConcat::right)) return;
	if (two(dynamic_cast<ArrayExtend const*>(_expression),
		&ArrayExtend::base, &ArrayExtend::other)) return;
	if (two(dynamic_cast<AssignmentExpression const*>(_expression),
		&AssignmentExpression::target, &AssignmentExpression::value)) return;

	if (auto const* node = dynamic_cast<ConditionalExpression const*>(_expression))
	{
		visitExpr(node->condition.get(), _visitor);
		visitExpr(node->trueExpr.get(), _visitor);
		visitExpr(node->falseExpr.get(), _visitor);
		return;
	}

	if (auto const* node = dynamic_cast<TupleExpression const*>(_expression))
	{
		for (auto const& item: node->items) visitExpr(item.get(), _visitor);
		return;
	}
	if (auto const* node = dynamic_cast<NewArray const*>(_expression))
	{
		for (auto const& value: node->values) visitExpr(value.get(), _visitor);
		return;
	}
	if (auto const* node = dynamic_cast<IntrinsicCall const*>(_expression))
	{
		for (auto const& arg: node->stackArgs) visitExpr(arg.get(), _visitor);
		return;
	}
	if (auto const* node = dynamic_cast<CommaExpression const*>(_expression))
	{
		for (auto const& item: node->expressions) visitExpr(item.get(), _visitor);
		return;
	}
	if (auto const* node = dynamic_cast<SubmitInnerTransaction const*>(_expression))
	{
		for (auto const& txn: node->itxns) visitExpr(txn.get(), _visitor);
		return;
	}
	if (auto const* node = dynamic_cast<SubroutineCallExpression const*>(_expression))
	{
		for (auto const& arg: node->args) visitExpr(arg.value.get(), _visitor);
		return;
	}
	if (auto const* node = dynamic_cast<PuyaLibCall const*>(_expression))
	{
		for (auto const& arg: node->args) visitExpr(arg.value.get(), _visitor);
		return;
	}

	if (auto const* node = dynamic_cast<NewStruct const*>(_expression))
	{
		for (auto const& [_, value]: node->values) visitExpr(value.get(), _visitor);
		return;
	}
	if (auto const* node = dynamic_cast<NamedTupleExpression const*>(_expression))
	{
		for (auto const& [_, value]: node->values) visitExpr(value.get(), _visitor);
		return;
	}
	if (auto const* node = dynamic_cast<CreateInnerTransaction const*>(_expression))
	{
		for (auto const& [_, value]: node->fields) visitExpr(value.get(), _visitor);
		return;
	}

	if (one(dynamic_cast<AppStateExpression const*>(_expression), &AppStateExpression::key)) return;
	if (two(dynamic_cast<AppAccountStateExpression const*>(_expression),
		&AppAccountStateExpression::key, &AppAccountStateExpression::account)) return;
	if (one(dynamic_cast<BoxValueExpression const*>(_expression), &BoxValueExpression::key)) return;
	if (two(dynamic_cast<BoxPrefixedKeyExpression const*>(_expression),
		&BoxPrefixedKeyExpression::prefix, &BoxPrefixedKeyExpression::key)) return;
	if (two(dynamic_cast<StateGet const*>(_expression),
		&StateGet::field, &StateGet::defaultValue)) return;
	if (one(dynamic_cast<StateGetEx const*>(_expression), &StateGetEx::field)) return;
	if (one(dynamic_cast<StateExists const*>(_expression), &StateExists::field)) return;
	if (one(dynamic_cast<StateDelete const*>(_expression), &StateDelete::field)) return;
	if (two(dynamic_cast<InnerTransactionField const*>(_expression),
		&InnerTransactionField::itxn, &InnerTransactionField::arrayIndex)) return;

	throw std::logic_error(
		std::string("unhandled AWST expression in const visitor: ")
		+ typeid(*_expression).name());
}

void visitStmt(Statement const* _statement, ExpressionVisitor const& _visitor)
{
	if (!_statement)
		return;
	if (auto const* node = dynamic_cast<Block const*>(_statement))
	{
		for (auto const& statement: node->body) visitStmt(statement.get(), _visitor);
		return;
	}
	if (auto const* node = dynamic_cast<ExpressionStatement const*>(_statement))
	{
		visitExpr(node->expr.get(), _visitor);
		return;
	}
	if (auto const* node = dynamic_cast<ReturnStatement const*>(_statement))
	{
		visitExpr(node->value.get(), _visitor);
		return;
	}
	if (auto const* node = dynamic_cast<AssignmentStatement const*>(_statement))
	{
		visitExpr(node->target.get(), _visitor);
		visitExpr(node->value.get(), _visitor);
		return;
	}
	if (auto const* node = dynamic_cast<IfElse const*>(_statement))
	{
		visitExpr(node->condition.get(), _visitor);
		visitStmt(node->ifBranch.get(), _visitor);
		visitStmt(node->elseBranch.get(), _visitor);
		return;
	}
	if (auto const* node = dynamic_cast<WhileLoop const*>(_statement))
	{
		visitExpr(node->condition.get(), _visitor);
		visitStmt(node->loopBody.get(), _visitor);
		return;
	}
	if (auto const* node = dynamic_cast<ForInLoop const*>(_statement))
	{
		visitExpr(node->sequence.get(), _visitor);
		visitExpr(node->items.get(), _visitor);
		visitStmt(node->loopBody.get(), _visitor);
		return;
	}
	if (auto const* node = dynamic_cast<Switch const*>(_statement))
	{
		visitExpr(node->value.get(), _visitor);
		for (auto const& [value, block]: node->cases)
		{
			visitExpr(value.get(), _visitor);
			visitStmt(block.get(), _visitor);
		}
		visitStmt(node->defaultCase.get(), _visitor);
		return;
	}
	if (auto const* node = dynamic_cast<UInt64AugmentedAssignment const*>(_statement))
	{
		visitExpr(node->target.get(), _visitor);
		visitExpr(node->value.get(), _visitor);
		return;
	}
	if (auto const* node = dynamic_cast<BigUIntAugmentedAssignment const*>(_statement))
	{
		visitExpr(node->target.get(), _visitor);
		visitExpr(node->value.get(), _visitor);
		return;
	}
	if (dynamic_cast<Goto const*>(_statement)) return;
	if (dynamic_cast<LoopExit const*>(_statement)) return;
	if (dynamic_cast<LoopContinue const*>(_statement)) return;

	throw std::logic_error(
		std::string("unhandled AWST statement in const visitor: ")
		+ typeid(*_statement).name());
}

} // namespace

void visitExpressions(Statement const& _statement, ExpressionVisitor const& _visitor)
{
	visitStmt(&_statement, _visitor);
}

void visitExpressions(ContractMethod const& _method, ExpressionVisitor const& _visitor)
{
	visitStmt(_method.body.get(), _visitor);
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
