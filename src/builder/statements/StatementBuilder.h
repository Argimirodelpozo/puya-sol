#pragma once

#include "awst/Node.h"
#include "builder/expressions/ExpressionBuilder.h"
#include "builder/sol-types/TypeMapper.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/ASTVisitor.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace puyasol::builder
{

/// Builds AWST Statement nodes from Solidity AST statements.
///
/// Uses the visitor pattern (ASTConstVisitor) with a result stack.
/// Delegates expression translation to ExpressionBuilder.
///
/// Implementation is split across multiple files:
///   - StatementBuilder.cpp           — Core: constructor, build(), buildBlock()
///   - ExpressionStatementBuilder.cpp — Expression statements and block visitors
///   - ReturnBuilder.cpp              — Return statements with type coercion
///   - ControlFlowBuilder.cpp         — if/else, while, for, continue, break
///   - VariableDeclarationBuilder.cpp — Variable declarations with initializers
///   - EmitBuilder.cpp                — Event emission statements
///   - RevertBuilder.cpp              — Revert statements with custom errors
///   - InlineAssemblyBuilder.cpp      — Inline assembly block delegation to AssemblyBuilder
class StatementBuilder: public solidity::frontend::ASTConstVisitor
{
public:
	StatementBuilder(
		ExpressionBuilder& _exprBuilder,
		TypeMapper& _typeMapper,
		std::string const& _sourceFile
	);

	/// Build an AWST statement from a Solidity statement.
	std::shared_ptr<awst::Statement> build(solidity::frontend::Statement const& _stmt);

	/// Build an AWST block from a block of statements.
	std::shared_ptr<awst::Block> buildBlock(solidity::frontend::Block const& _block);

	/// Set function context for inline assembly translation.
	/// Must be called before buildBlock if the function body may contain assembly.
	void setFunctionContext(
		std::vector<std::pair<std::string, awst::WType const*>> const& _params,
		awst::WType const* _returnType
	);

	// ASTConstVisitor overrides
	bool visit(solidity::frontend::Block const& _node) override;
	bool visit(solidity::frontend::ExpressionStatement const& _node) override;
	bool visit(solidity::frontend::Return const& _node) override;
	bool visit(solidity::frontend::IfStatement const& _node) override;
	bool visit(solidity::frontend::WhileStatement const& _node) override;
	bool visit(solidity::frontend::ForStatement const& _node) override;
	bool visit(solidity::frontend::Continue const& _node) override;
	bool visit(solidity::frontend::Break const& _node) override;
	bool visit(solidity::frontend::VariableDeclarationStatement const& _node) override;
	bool visit(solidity::frontend::EmitStatement const& _node) override;
	bool visit(solidity::frontend::RevertStatement const& _node) override;
	bool visit(solidity::frontend::InlineAssembly const& _node) override;

private:
	ExpressionBuilder& m_exprBuilder;
	TypeMapper& m_typeMapper;
	std::string m_sourceFile;

	/// Function context for inline assembly translation.
	std::vector<std::pair<std::string, awst::WType const*>> m_functionParams;
	awst::WType const* m_returnType = nullptr;

	/// Statement result stack.
	std::vector<std::shared_ptr<awst::Statement>> m_stack;

	/// Pending ArrayExtend statements from large array chunking.
	std::vector<std::shared_ptr<awst::Statement>> m_pendingExtends;

	/// For-loop post expression (increment). When set, `continue` statements
	/// are preceded by this expression so the loop variable advances.
	std::shared_ptr<awst::Statement> m_forLoopPost;

	void push(std::shared_ptr<awst::Statement> _stmt);
	std::shared_ptr<awst::Statement> pop();

	awst::SourceLocation makeLoc(solidity::langutil::SourceLocation const& _solLoc);
};

} // namespace puyasol::builder
