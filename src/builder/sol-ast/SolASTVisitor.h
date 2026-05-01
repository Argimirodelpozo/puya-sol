#pragma once

/// @file SolASTVisitor.h
/// Generic value-returning visitor over Solidity's AST.
///
/// Inspired by Solidity's own ASTConstVisitor and IRGenerator pattern, but with
/// two adjustments suited to puya-sol:
///
///   1. The result type `R` is a template parameter — we run several different
///      passes over the same AST (expression translation returns
///      shared_ptr<awst::Expression>, statement translation returns a vector,
///      analyses return void / bool / int).
///
///   2. Dispatch is by dynamic_cast over Solidity's AST hierarchy, not by
///      double-dispatch through the AST. We don't own the AST, can't add
///      `accept(Visitor&)` methods to it, and don't want to fight the Solidity
///      hierarchy. Wrapping `dynamic_cast` in a base class hands every visitor
///      the same uniform dispatch table for free.
///
/// Subclasses pick which `visit*` methods to override. Anything they don't
/// override falls through to `visitDefault(ASTNode const&)`, which is the only
/// pure-virtual hook — every concrete visitor has to decide what "I don't
/// handle this kind" means (warn? abort? return zero?).
///
/// Both Expression and Statement live in the same class because both inherit
/// from `ASTNode`, and analysis passes over either may want to share a
/// uniform `visitDefault`. Two entry points (`visit(Expression const&)` and
/// `visit(Statement const&)`) keep the API call-sites clean.

#include <libsolidity/ast/AST.h>

namespace puyasol::builder::sol_ast
{

template <typename R>
class SolASTVisitor
{
public:
	virtual ~SolASTVisitor() = default;

	// ── Dispatch entry points ─────────────────────────────────────────────

	R visit(solidity::frontend::Expression const& _expr)
	{
		using namespace solidity::frontend;
		if (auto const* n = dynamic_cast<Literal const*>(&_expr))                     return visitLiteral(*n);
		if (auto const* n = dynamic_cast<Identifier const*>(&_expr))                  return visitIdentifier(*n);
		if (auto const* n = dynamic_cast<BinaryOperation const*>(&_expr))             return visitBinaryOp(*n);
		if (auto const* n = dynamic_cast<UnaryOperation const*>(&_expr))              return visitUnaryOp(*n);
		if (auto const* n = dynamic_cast<Conditional const*>(&_expr))                 return visitConditional(*n);
		if (auto const* n = dynamic_cast<Assignment const*>(&_expr))                  return visitAssignment(*n);
		if (auto const* n = dynamic_cast<IndexAccess const*>(&_expr))                 return visitIndexAccess(*n);
		if (auto const* n = dynamic_cast<IndexRangeAccess const*>(&_expr))            return visitIndexRange(*n);
		if (auto const* n = dynamic_cast<TupleExpression const*>(&_expr))             return visitTuple(*n);
		if (auto const* n = dynamic_cast<FunctionCall const*>(&_expr))                return visitFunctionCall(*n);
		if (auto const* n = dynamic_cast<MemberAccess const*>(&_expr))                return visitMemberAccess(*n);
		if (auto const* n = dynamic_cast<FunctionCallOptions const*>(&_expr))         return visitCallOptions(*n);
		if (auto const* n = dynamic_cast<ElementaryTypeNameExpression const*>(&_expr)) return visitTypeName(*n);
		return visitDefault(_expr);
	}

	R visit(solidity::frontend::Statement const& _stmt)
	{
		using namespace solidity::frontend;
		if (auto const* n = dynamic_cast<Block const*>(&_stmt))                          return visitBlock(*n);
		if (auto const* n = dynamic_cast<IfStatement const*>(&_stmt))                    return visitIfStatement(*n);
		if (auto const* n = dynamic_cast<WhileStatement const*>(&_stmt))                 return visitWhile(*n);
		if (auto const* n = dynamic_cast<ForStatement const*>(&_stmt))                   return visitFor(*n);
		if (auto const* n = dynamic_cast<Return const*>(&_stmt))                         return visitReturn(*n);
		if (auto const* n = dynamic_cast<RevertStatement const*>(&_stmt))                return visitRevert(*n);
		if (auto const* n = dynamic_cast<EmitStatement const*>(&_stmt))                  return visitEmit(*n);
		if (auto const* n = dynamic_cast<VariableDeclarationStatement const*>(&_stmt))   return visitVarDecl(*n);
		if (auto const* n = dynamic_cast<InlineAssembly const*>(&_stmt))                 return visitInlineAssembly(*n);
		if (auto const* n = dynamic_cast<ExpressionStatement const*>(&_stmt))            return visitExprStatement(*n);
		if (auto const* n = dynamic_cast<Continue const*>(&_stmt))                       return visitContinue(*n);
		if (auto const* n = dynamic_cast<Break const*>(&_stmt))                          return visitBreak(*n);
		if (auto const* n = dynamic_cast<PlaceholderStatement const*>(&_stmt))           return visitPlaceholder(*n);
		if (auto const* n = dynamic_cast<TryStatement const*>(&_stmt))                   return visitTryCatch(*n);
		return visitDefault(_stmt);
	}

	// ── Expression hooks (default → visitDefault) ─────────────────────────

	virtual R visitLiteral(solidity::frontend::Literal const& _n)                              { return visitDefault(_n); }
	virtual R visitIdentifier(solidity::frontend::Identifier const& _n)                        { return visitDefault(_n); }
	virtual R visitBinaryOp(solidity::frontend::BinaryOperation const& _n)                     { return visitDefault(_n); }
	virtual R visitUnaryOp(solidity::frontend::UnaryOperation const& _n)                       { return visitDefault(_n); }
	virtual R visitConditional(solidity::frontend::Conditional const& _n)                      { return visitDefault(_n); }
	virtual R visitAssignment(solidity::frontend::Assignment const& _n)                        { return visitDefault(_n); }
	virtual R visitIndexAccess(solidity::frontend::IndexAccess const& _n)                      { return visitDefault(_n); }
	virtual R visitIndexRange(solidity::frontend::IndexRangeAccess const& _n)                  { return visitDefault(_n); }
	virtual R visitTuple(solidity::frontend::TupleExpression const& _n)                        { return visitDefault(_n); }
	virtual R visitFunctionCall(solidity::frontend::FunctionCall const& _n)                    { return visitDefault(_n); }
	virtual R visitMemberAccess(solidity::frontend::MemberAccess const& _n)                    { return visitDefault(_n); }
	virtual R visitCallOptions(solidity::frontend::FunctionCallOptions const& _n)              { return visitDefault(_n); }
	virtual R visitTypeName(solidity::frontend::ElementaryTypeNameExpression const& _n)        { return visitDefault(_n); }

	// ── Statement hooks (default → visitDefault) ──────────────────────────

	virtual R visitBlock(solidity::frontend::Block const& _n)                                  { return visitDefault(_n); }
	virtual R visitIfStatement(solidity::frontend::IfStatement const& _n)                      { return visitDefault(_n); }
	virtual R visitWhile(solidity::frontend::WhileStatement const& _n)                         { return visitDefault(_n); }
	virtual R visitFor(solidity::frontend::ForStatement const& _n)                             { return visitDefault(_n); }
	virtual R visitReturn(solidity::frontend::Return const& _n)                                { return visitDefault(_n); }
	virtual R visitRevert(solidity::frontend::RevertStatement const& _n)                       { return visitDefault(_n); }
	virtual R visitEmit(solidity::frontend::EmitStatement const& _n)                           { return visitDefault(_n); }
	virtual R visitVarDecl(solidity::frontend::VariableDeclarationStatement const& _n)         { return visitDefault(_n); }
	virtual R visitInlineAssembly(solidity::frontend::InlineAssembly const& _n)                { return visitDefault(_n); }
	virtual R visitExprStatement(solidity::frontend::ExpressionStatement const& _n)            { return visitDefault(_n); }
	virtual R visitContinue(solidity::frontend::Continue const& _n)                            { return visitDefault(_n); }
	virtual R visitBreak(solidity::frontend::Break const& _n)                                  { return visitDefault(_n); }
	virtual R visitPlaceholder(solidity::frontend::PlaceholderStatement const& _n)             { return visitDefault(_n); }
	virtual R visitTryCatch(solidity::frontend::TryStatement const& _n)                        { return visitDefault(_n); }

	// ── Catchall — every concrete visitor must implement this. ────────────

	virtual R visitDefault(solidity::frontend::ASTNode const& _node) = 0;
};

} // namespace puyasol::builder::sol_ast
