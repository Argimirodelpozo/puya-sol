#pragma once

#include "awst/Node.h"
#include "builder/ExpressionTranslator.h"
#include "builder/TypeMapper.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/ASTVisitor.h>

#include <memory>
#include <string>
#include <vector>

namespace puyasol::builder
{

/// Translates Solidity statements to AWST Statement nodes.
class StatementTranslator: public solidity::frontend::ASTConstVisitor
{
public:
	StatementTranslator(
		ExpressionTranslator& _exprTranslator,
		TypeMapper& _typeMapper,
		std::string const& _sourceFile
	);

	/// Translate a Solidity statement to AWST.
	std::shared_ptr<awst::Statement> translate(solidity::frontend::Statement const& _stmt);

	/// Translate a block of statements.
	std::shared_ptr<awst::Block> translateBlock(solidity::frontend::Block const& _block);

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

private:
	ExpressionTranslator& m_exprTranslator;
	TypeMapper& m_typeMapper;
	std::string m_sourceFile;

	/// Statement result stack.
	std::vector<std::shared_ptr<awst::Statement>> m_stack;

	void push(std::shared_ptr<awst::Statement> _stmt);
	std::shared_ptr<awst::Statement> pop();

	awst::SourceLocation makeLoc(solidity::langutil::SourceLocation const& _solLoc);
};

} // namespace puyasol::builder
