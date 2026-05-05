/// @file AwstWalker.cpp
/// See AwstWalker.h. Recursion is hand-written per Expression /
/// Statement subclass — there's no virtual visit() on awst::Node, so
/// dispatching by dynamic_cast is the only option. Tedious but local.

#include "splitter/AwstWalker.h"

#include "Logger.h"

namespace puyasol::splitter
{

namespace
{

void recurseExpr(
	std::shared_ptr<awst::Expression>& _expr, ExprRewriteFn const& _fn);

// Helper — accept a slot, apply the callback, recurse into children if
// the callback didn't replace the node.
void visitSlot(
	std::shared_ptr<awst::Expression>& _slot, ExprRewriteFn const& _fn)
{
	if (!_slot)
		return;
	if (auto rep = _fn(*_slot))
	{
		_slot = std::move(rep);
		return; // callback owns recursion into the replacement subtree
	}
	recurseExpr(_slot, _fn);
}

void recurseExpr(
	std::shared_ptr<awst::Expression>& _expr, ExprRewriteFn const& _fn)
{
	using namespace awst;
	auto* e = _expr.get();
	if (!e) return;

	// Leaf nodes — no children to recurse into.
	if (dynamic_cast<IntegerConstant*>(e)) return;
	if (dynamic_cast<BoolConstant*>(e)) return;
	if (dynamic_cast<BytesConstant*>(e)) return;
	if (dynamic_cast<StringConstant*>(e)) return;
	if (dynamic_cast<VoidConstant*>(e)) return;
	if (dynamic_cast<VarExpression*>(e)) return;
	if (dynamic_cast<TemplateVar*>(e)) return;
	if (dynamic_cast<MethodConstant*>(e)) return;
	if (dynamic_cast<AddressConstant*>(e)) return;
	if (dynamic_cast<ARC4Router*>(e)) return;

	// Binary ops: left, right
	if (auto* x = dynamic_cast<UInt64BinaryOperation*>(e))
	{ visitSlot(x->left, _fn); visitSlot(x->right, _fn); return; }
	if (auto* x = dynamic_cast<BigUIntBinaryOperation*>(e))
	{ visitSlot(x->left, _fn); visitSlot(x->right, _fn); return; }
	if (auto* x = dynamic_cast<BytesBinaryOperation*>(e))
	{ visitSlot(x->left, _fn); visitSlot(x->right, _fn); return; }
	if (auto* x = dynamic_cast<BooleanBinaryOperation*>(e))
	{ visitSlot(x->left, _fn); visitSlot(x->right, _fn); return; }
	if (auto* x = dynamic_cast<NumericComparisonExpression*>(e))
	{ visitSlot(x->lhs, _fn); visitSlot(x->rhs, _fn); return; }
	if (auto* x = dynamic_cast<BytesComparisonExpression*>(e))
	{ visitSlot(x->lhs, _fn); visitSlot(x->rhs, _fn); return; }

	// Unary ops
	if (auto* x = dynamic_cast<BytesUnaryOperation*>(e))
	{ visitSlot(x->expr, _fn); return; }
	if (auto* x = dynamic_cast<Not*>(e))
	{ visitSlot(x->expr, _fn); return; }
	if (auto* x = dynamic_cast<ReinterpretCast*>(e))
	{ visitSlot(x->expr, _fn); return; }
	if (auto* x = dynamic_cast<ConvertArray*>(e))
	{ visitSlot(x->expr, _fn); return; }
	if (auto* x = dynamic_cast<CheckedMaybe*>(e))
	{ visitSlot(x->expr, _fn); return; }
	if (auto* x = dynamic_cast<Copy*>(e))
	{ visitSlot(x->value, _fn); return; }
	if (auto* x = dynamic_cast<SingleEvaluation*>(e))
	{ visitSlot(x->source, _fn); return; }
	if (auto* x = dynamic_cast<ARC4Encode*>(e))
	{ visitSlot(x->value, _fn); return; }
	if (auto* x = dynamic_cast<ARC4Decode*>(e))
	{ visitSlot(x->value, _fn); return; }
	if (auto* x = dynamic_cast<ARC4FromBytes*>(e))
	{ visitSlot(x->value, _fn); return; }
	if (auto* x = dynamic_cast<Emit*>(e))
	{ visitSlot(x->value, _fn); return; }
	if (auto* x = dynamic_cast<ArrayLength*>(e))
	{ visitSlot(x->array, _fn); return; }
	if (auto* x = dynamic_cast<ArrayPop*>(e))
	{ visitSlot(x->base, _fn); return; }
	if (auto* x = dynamic_cast<TupleItemExpression*>(e))
	{ visitSlot(x->base, _fn); return; }
	if (auto* x = dynamic_cast<FieldExpression*>(e))
	{ visitSlot(x->base, _fn); return; }

	// Two-children
	if (auto* x = dynamic_cast<IndexExpression*>(e))
	{ visitSlot(x->base, _fn); visitSlot(x->index, _fn); return; }
	if (auto* x = dynamic_cast<ArrayConcat*>(e))
	{ visitSlot(x->left, _fn); visitSlot(x->right, _fn); return; }
	if (auto* x = dynamic_cast<ArrayExtend*>(e))
	{ visitSlot(x->base, _fn); visitSlot(x->other, _fn); return; }
	if (auto* x = dynamic_cast<AssignmentExpression*>(e))
	{ visitSlot(x->target, _fn); visitSlot(x->value, _fn); return; }

	// Conditional
	if (auto* x = dynamic_cast<ConditionalExpression*>(e))
	{
		visitSlot(x->condition, _fn);
		visitSlot(x->trueExpr, _fn);
		visitSlot(x->falseExpr, _fn);
		return;
	}
	if (auto* x = dynamic_cast<AssertExpression*>(e))
	{ visitSlot(x->condition, _fn); return; }

	// Vector-of-expr
	if (auto* x = dynamic_cast<TupleExpression*>(e))
	{ for (auto& it : x->items) visitSlot(it, _fn); return; }
	if (auto* x = dynamic_cast<NewArray*>(e))
	{ for (auto& v : x->values) visitSlot(v, _fn); return; }
	if (auto* x = dynamic_cast<IntrinsicCall*>(e))
	{ for (auto& a : x->stackArgs) visitSlot(a, _fn); return; }
	if (auto* x = dynamic_cast<CommaExpression*>(e))
	{ for (auto& it : x->expressions) visitSlot(it, _fn); return; }
	if (auto* x = dynamic_cast<SubmitInnerTransaction*>(e))
	{ for (auto& it : x->itxns) visitSlot(it, _fn); return; }
	if (auto* x = dynamic_cast<SubroutineCallExpression*>(e))
	{ for (auto& a : x->args) visitSlot(a.value, _fn); return; }
	if (auto* x = dynamic_cast<PuyaLibCall*>(e))
	{ for (auto& a : x->args) visitSlot(a.value, _fn); return; }

	// Map-of-expr
	if (auto* x = dynamic_cast<NewStruct*>(e))
	{ for (auto& [k, v] : x->values) visitSlot(v, _fn); return; }
	if (auto* x = dynamic_cast<NamedTupleExpression*>(e))
	{ for (auto& [k, v] : x->values) visitSlot(v, _fn); return; }
	if (auto* x = dynamic_cast<CreateInnerTransaction*>(e))
	{ for (auto& [k, v] : x->fields) visitSlot(v, _fn); return; }

	// Storage expressions
	if (auto* x = dynamic_cast<AppStateExpression*>(e))
	{ visitSlot(x->key, _fn); return; }
	if (auto* x = dynamic_cast<AppAccountStateExpression*>(e))
	{ visitSlot(x->key, _fn); visitSlot(x->account, _fn); return; }
	if (auto* x = dynamic_cast<BoxValueExpression*>(e))
	{ visitSlot(x->key, _fn); return; }
	if (auto* x = dynamic_cast<BoxPrefixedKeyExpression*>(e))
	{ visitSlot(x->prefix, _fn); visitSlot(x->key, _fn); return; }
	if (auto* x = dynamic_cast<StateGet*>(e))
	{ visitSlot(x->field, _fn); visitSlot(x->defaultValue, _fn); return; }
	if (auto* x = dynamic_cast<StateGetEx*>(e))
	{ visitSlot(x->field, _fn); return; }
	if (auto* x = dynamic_cast<StateExists*>(e))
	{ visitSlot(x->field, _fn); return; }
	if (auto* x = dynamic_cast<StateDelete*>(e))
	{ visitSlot(x->field, _fn); return; }

	// Inner-txn field access: itxn → arrayIndex (both Expression slots).
	if (auto* x = dynamic_cast<InnerTransactionField*>(e))
	{ visitSlot(x->itxn, _fn); visitSlot(x->arrayIndex, _fn); return; }

	// Any unhandled subclass: log once-ish but keep going. The patching
	// pass that wanted this node will simply skip the unrecursed subtree;
	// usually fine, but it's worth surfacing if a new node type appears.
	// Cast-through-typeid to print the dynamic type name.
	Logger::instance().warning(
		std::string("AwstWalker: unhandled Expression subclass: ")
		+ typeid(*e).name()
		+ " — children not recursed; add handling in AwstWalker.cpp",
		e->sourceLocation);
}

void recurseStmt(
	std::shared_ptr<awst::Statement>& _stmt, ExprRewriteFn const& _fn);

void visitStmtSlot(
	std::shared_ptr<awst::Statement>& _slot, ExprRewriteFn const& _fn)
{
	if (_slot)
		recurseStmt(_slot, _fn);
}

void recurseStmt(
	std::shared_ptr<awst::Statement>& _stmt, ExprRewriteFn const& _fn)
{
	using namespace awst;
	auto* s = _stmt.get();
	if (!s) return;

	if (auto* x = dynamic_cast<Block*>(s))
	{ for (auto& bs : x->body) visitStmtSlot(bs, _fn); return; }

	if (auto* x = dynamic_cast<ExpressionStatement*>(s))
	{ visitSlot(x->expr, _fn); return; }

	if (auto* x = dynamic_cast<ReturnStatement*>(s))
	{ visitSlot(x->value, _fn); return; }

	if (auto* x = dynamic_cast<AssignmentStatement*>(s))
	{ visitSlot(x->target, _fn); visitSlot(x->value, _fn); return; }

	if (auto* x = dynamic_cast<IfElse*>(s))
	{
		visitSlot(x->condition, _fn);
		if (x->ifBranch)
		{
			std::shared_ptr<Statement> body = x->ifBranch;
			recurseStmt(body, _fn);
		}
		if (x->elseBranch)
		{
			std::shared_ptr<Statement> body = x->elseBranch;
			recurseStmt(body, _fn);
		}
		return;
	}

	if (auto* x = dynamic_cast<WhileLoop*>(s))
	{
		visitSlot(x->condition, _fn);
		if (x->loopBody)
		{
			std::shared_ptr<Statement> body = x->loopBody;
			recurseStmt(body, _fn);
		}
		return;
	}

	if (auto* x = dynamic_cast<ForInLoop*>(s))
	{
		visitSlot(x->sequence, _fn);
		visitSlot(x->items, _fn);
		if (x->loopBody)
		{
			std::shared_ptr<Statement> body = x->loopBody;
			recurseStmt(body, _fn);
		}
		return;
	}

	if (auto* x = dynamic_cast<Switch*>(s))
	{
		visitSlot(x->value, _fn);
		for (auto& [caseExpr, caseBlock] : x->cases)
		{
			visitSlot(caseExpr, _fn);
			if (caseBlock)
			{
				std::shared_ptr<Statement> body = caseBlock;
				recurseStmt(body, _fn);
			}
		}
		if (x->defaultCase)
		{
			std::shared_ptr<Statement> body = x->defaultCase;
			recurseStmt(body, _fn);
		}
		return;
	}

	if (auto* x = dynamic_cast<UInt64AugmentedAssignment*>(s))
	{ visitSlot(x->target, _fn); visitSlot(x->value, _fn); return; }

	if (auto* x = dynamic_cast<BigUIntAugmentedAssignment*>(s))
	{ visitSlot(x->target, _fn); visitSlot(x->value, _fn); return; }

	if (dynamic_cast<Goto*>(s)) return;
	if (dynamic_cast<LoopExit*>(s)) return;
	if (dynamic_cast<LoopContinue*>(s)) return;

	Logger::instance().warning(
		std::string("AwstWalker: unhandled Statement subclass: ")
		+ typeid(*s).name()
		+ " — children not recursed; add handling in AwstWalker.cpp",
		s->sourceLocation);
}

} // namespace

void walkExpression(
	std::shared_ptr<awst::Expression>& _expr, ExprRewriteFn const& _fn)
{
	visitSlot(_expr, _fn);
}

void walkStatement(awst::Statement& _stmt, ExprRewriteFn const& _fn)
{
	std::shared_ptr<awst::Statement> tmp(&_stmt, [](awst::Statement*){});
	recurseStmt(tmp, _fn);
}

void walkBlock(awst::Block& _block, ExprRewriteFn const& _fn)
{
	for (auto& s : _block.body)
		visitStmtSlot(s, _fn);
}

} // namespace puyasol::splitter
