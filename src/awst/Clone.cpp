/// @file Clone.cpp
/// Deep clone of AWST expression/statement trees. See Clone.h.
///
/// Strategy: copy-construct the concrete node (which copies every scalar field AND the wtype pointer
/// — wtypes are interned/immutable so sharing is correct), then recursively replace each child
/// shared_ptr with a fresh clone. Scalar fields can never be forgotten; only child links are
/// enumerated. An unhandled node type fails loud (Logger::error) rather than silently sharing.
///
/// Sharing is preserved WITHIN a clone via a per-call memo keyed by the original node pointer: an
/// AWST tree can be a DAG (the same node reached by two paths, e.g. a shared SingleEvaluation for
/// "evaluate once, use twice"), and naive cloning would split it into independent nodes — evaluating
/// a side-effecting source twice. The memo maps each original node to a single clone. SingleEvaluation
/// ids are re-minted once per distinct original (nextSingleEvalId) so puya's (source,id) eval cache
/// does not merge the copy with the original. Each top-level clone() call uses a FRESH memo, so two
/// splices of the same body (a modifier with two `_;`) are mutually independent.
#include "awst/Clone.h"
#include "Logger.h"

#include <unordered_map>

namespace puyasol::awst
{

namespace
{

struct CloneCtx
{
	std::unordered_map<Expression const*, std::shared_ptr<Expression>> exprs;
	std::unordered_map<Statement const*, std::shared_ptr<Statement>> stmts;
};

std::shared_ptr<Expression> cloneE(std::shared_ptr<Expression> const& e, CloneCtx& ctx);
std::shared_ptr<Statement> cloneS(std::shared_ptr<Statement> const& s, CloneCtx& ctx);
std::shared_ptr<Block> cloneB(std::shared_ptr<Block> const& b, CloneCtx& ctx);

std::vector<std::shared_ptr<Expression>> cloneEVec(
	std::vector<std::shared_ptr<Expression>> const& src, CloneCtx& ctx)
{
	std::vector<std::shared_ptr<Expression>> out;
	out.reserve(src.size());
	for (auto const& e: src)
		out.push_back(cloneE(e, ctx));
	return out;
}

std::vector<CallArg> cloneArgs(std::vector<CallArg> const& src, CloneCtx& ctx)
{
	std::vector<CallArg> out;
	out.reserve(src.size());
	for (auto const& a: src)
	{
		CallArg c;
		c.name = a.name;
		c.value = cloneE(a.value, ctx);
		out.push_back(std::move(c));
	}
	return out;
}

std::map<std::string, std::shared_ptr<Expression>> cloneEMap(
	std::map<std::string, std::shared_ptr<Expression>> const& src, CloneCtx& ctx)
{
	std::map<std::string, std::shared_ptr<Expression>> out;
	for (auto const& [k, v]: src)
		out[k] = cloneE(v, ctx);
	return out;
}

std::shared_ptr<Expression> cloneE(std::shared_ptr<Expression> const& e, CloneCtx& ctx)
{
	if (!e)
		return nullptr;
	if (auto it = ctx.exprs.find(e.get()); it != ctx.exprs.end())
		return it->second; // preserve DAG sharing within this clone

	Expression* p = e.get();
	std::shared_ptr<Expression> r;

	if (false) {} // anchor so every clause below chains as `else if` into one final `else`
#define LEAF(T) else if (auto* n = dynamic_cast<T*>(p)) { (void) n; r = std::make_shared<T>(*n); }
#define CHILD1(T, f) else if (auto* n = dynamic_cast<T*>(p)) { auto c = std::make_shared<T>(*n); c->f = cloneE(n->f, ctx); r = c; }
#define CHILD2(T, a, b) else if (auto* n = dynamic_cast<T*>(p)) { auto c = std::make_shared<T>(*n); c->a = cloneE(n->a, ctx); c->b = cloneE(n->b, ctx); r = c; }
#define CHILD3(T, a, b, d) else if (auto* n = dynamic_cast<T*>(p)) { auto c = std::make_shared<T>(*n); c->a = cloneE(n->a, ctx); c->b = cloneE(n->b, ctx); c->d = cloneE(n->d, ctx); r = c; }
#define VEC(T, f) else if (auto* n = dynamic_cast<T*>(p)) { auto c = std::make_shared<T>(*n); c->f = cloneEVec(n->f, ctx); r = c; }
#define EMAP(T, f) else if (auto* n = dynamic_cast<T*>(p)) { auto c = std::make_shared<T>(*n); c->f = cloneEMap(n->f, ctx); r = c; }
#define ARGS(T) else if (auto* n = dynamic_cast<T*>(p)) { auto c = std::make_shared<T>(*n); c->args = cloneArgs(n->args, ctx); r = c; }

	LEAF(IntegerConstant)
	LEAF(BoolConstant)
	LEAF(BytesConstant)
	LEAF(StringConstant)
	LEAF(VoidConstant)
	LEAF(VarExpression)
	LEAF(TemplateVar)
	LEAF(MethodConstant)
	LEAF(AddressConstant)
	LEAF(ARC4Router)
	CHILD2(UInt64BinaryOperation, left, right)
	CHILD2(BigUIntBinaryOperation, left, right)
	CHILD2(BytesBinaryOperation, left, right)
	CHILD1(BytesUnaryOperation, expr)
	CHILD2(NumericComparisonExpression, lhs, rhs)
	CHILD2(BytesComparisonExpression, lhs, rhs)
	CHILD2(BooleanBinaryOperation, left, right)
	CHILD1(Not, expr)
	CHILD1(AssertExpression, condition)
	CHILD2(AssignmentExpression, target, value)
	CHILD3(ConditionalExpression, condition, trueExpr, falseExpr)
	CHILD1(FieldExpression, base)
	CHILD2(IndexExpression, base, index)
	CHILD1(TupleItemExpression, base)
	CHILD1(ReinterpretCast, expr)
	CHILD1(Copy, value)
	CHILD1(CheckedMaybe, expr)
	CHILD1(Emit, value)
	CHILD1(ArrayLength, array)
	CHILD1(ArrayPop, base)
	CHILD2(ArrayConcat, left, right)
	CHILD2(ArrayExtend, base, other)
	CHILD1(ConvertArray, expr)
	CHILD2(StateGet, field, defaultValue)
	CHILD1(StateExists, field)
	CHILD1(StateDelete, field)
	CHILD1(StateGetEx, field)
	CHILD1(AppStateExpression, key)
	CHILD2(AppAccountStateExpression, key, account)
	CHILD2(BoxPrefixedKeyExpression, prefix, key)
	CHILD1(BoxValueExpression, key)
	CHILD2(InnerTransactionField, itxn, arrayIndex)
	CHILD1(ARC4Encode, value)
	CHILD1(ARC4Decode, value)
	VEC(IntrinsicCall, stackArgs)
	VEC(TupleExpression, items)
	VEC(NewArray, values)
	VEC(SubmitInnerTransaction, itxns)
	VEC(CommaExpression, expressions)
	EMAP(NewStruct, values)
	EMAP(NamedTupleExpression, values)
	EMAP(CreateInnerTransaction, fields)
	ARGS(SubroutineCallExpression)
	ARGS(PuyaLibCall)
	else if (auto* n = dynamic_cast<SingleEvaluation*>(p))
	{
		auto c = std::make_shared<SingleEvaluation>(*n);
		c->source = cloneE(n->source, ctx);
		c->id = nextSingleEvalId(); // distinct from the original's (source,id) eval-cache key
		r = c;
	}
	else
	{
		Logger::instance().error(
			"cloneExpr: unhandled expression node type '" + e->nodeType()
				+ "' (a modifier with multiple `_;` contains a construct the cloner does not yet handle)",
			e->sourceLocation);
		r = e; // logged hard-error; share as a last resort
	}

#undef LEAF
#undef CHILD1
#undef CHILD2
#undef CHILD3
#undef VEC
#undef EMAP
#undef ARGS

	ctx.exprs.emplace(e.get(), r);
	return r;
}

std::shared_ptr<Block> cloneB(std::shared_ptr<Block> const& b, CloneCtx& ctx)
{
	if (!b)
		return nullptr;
	if (auto it = ctx.stmts.find(b.get()); it != ctx.stmts.end())
		return std::static_pointer_cast<Block>(it->second);
	auto c = std::make_shared<Block>(*b);
	c->body.clear();
	c->body.reserve(b->body.size());
	for (auto const& s: b->body)
		c->body.push_back(cloneS(s, ctx));
	ctx.stmts.emplace(b.get(), c);
	return c;
}

std::shared_ptr<Statement> cloneS(std::shared_ptr<Statement> const& s, CloneCtx& ctx)
{
	if (!s)
		return nullptr;
	if (auto it = ctx.stmts.find(s.get()); it != ctx.stmts.end())
		return it->second;

	Statement* p = s.get();
	std::shared_ptr<Statement> r;

	if (dynamic_cast<Block*>(p))
		r = cloneB(std::static_pointer_cast<Block>(s), ctx);
	else if (dynamic_cast<LoopExit*>(p))
		r = std::make_shared<LoopExit>(*static_cast<LoopExit*>(p));
	else if (dynamic_cast<LoopContinue*>(p))
		r = std::make_shared<LoopContinue>(*static_cast<LoopContinue*>(p));
	else if (dynamic_cast<Goto*>(p))
		r = std::make_shared<Goto>(*static_cast<Goto*>(p));
	else if (auto* n = dynamic_cast<ExpressionStatement*>(p))
	{ auto c = std::make_shared<ExpressionStatement>(*n); c->expr = cloneE(n->expr, ctx); r = c; }
	else if (auto* n = dynamic_cast<ReturnStatement*>(p))
	{ auto c = std::make_shared<ReturnStatement>(*n); c->value = cloneE(n->value, ctx); r = c; }
	else if (auto* n = dynamic_cast<IfElse*>(p))
	{
		auto c = std::make_shared<IfElse>(*n);
		c->condition = cloneE(n->condition, ctx);
		c->ifBranch = cloneB(n->ifBranch, ctx);
		c->elseBranch = cloneB(n->elseBranch, ctx);
		r = c;
	}
	else if (auto* n = dynamic_cast<WhileLoop*>(p))
	{
		auto c = std::make_shared<WhileLoop>(*n);
		c->condition = cloneE(n->condition, ctx);
		c->loopBody = cloneB(n->loopBody, ctx);
		r = c;
	}
	else if (auto* n = dynamic_cast<AssignmentStatement*>(p))
	{ auto c = std::make_shared<AssignmentStatement>(*n); c->target = cloneE(n->target, ctx); c->value = cloneE(n->value, ctx); r = c; }
	else if (auto* n = dynamic_cast<UInt64AugmentedAssignment*>(p))
	{ auto c = std::make_shared<UInt64AugmentedAssignment>(*n); c->target = cloneE(n->target, ctx); c->value = cloneE(n->value, ctx); r = c; }
	else if (auto* n = dynamic_cast<BigUIntAugmentedAssignment*>(p))
	{ auto c = std::make_shared<BigUIntAugmentedAssignment>(*n); c->target = cloneE(n->target, ctx); c->value = cloneE(n->value, ctx); r = c; }
	else if (auto* n = dynamic_cast<ForInLoop*>(p))
	{
		auto c = std::make_shared<ForInLoop>(*n);
		c->sequence = cloneE(n->sequence, ctx);
		c->items = cloneE(n->items, ctx);
		c->loopBody = cloneB(n->loopBody, ctx);
		r = c;
	}
	else if (auto* n = dynamic_cast<Switch*>(p))
	{
		auto c = std::make_shared<Switch>(*n);
		c->value = cloneE(n->value, ctx);
		c->cases.clear();
		for (auto const& [caseVal, caseBlk]: n->cases)
			c->cases.emplace_back(cloneE(caseVal, ctx), cloneB(caseBlk, ctx));
		c->defaultCase = cloneB(n->defaultCase, ctx);
		r = c;
	}
	else
	{
		Logger::instance().error(
			"cloneStmt: unhandled statement node type '" + s->nodeType()
				+ "' (a modifier with multiple `_;` contains a construct the cloner does not yet handle)",
			s->sourceLocation);
		r = s; // logged hard-error; share as a last resort
	}

	ctx.stmts.emplace(s.get(), r);
	return r;
}

} // namespace

std::shared_ptr<Statement> cloneStmt(std::shared_ptr<Statement> const& s)
{
	CloneCtx ctx;
	return cloneS(s, ctx);
}

std::shared_ptr<Block> cloneBlock(std::shared_ptr<Block> const& b)
{
	CloneCtx ctx;
	return cloneB(b, ctx);
}

} // namespace puyasol::awst
