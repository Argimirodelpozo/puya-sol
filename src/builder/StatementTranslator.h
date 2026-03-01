#pragma once

#include "awst/Node.h"
#include "builder/ExpressionTranslator.h"
#include "builder/TypeMapper.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/ASTVisitor.h>

#include <memory>
#include <string>
#include <utility>
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

	/// Set function context for inline assembly translation.
	/// Must be called before translateBlock if the function body may contain assembly.
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
	ExpressionTranslator& m_exprTranslator;
	TypeMapper& m_typeMapper;
	std::string m_sourceFile;

	/// Function context for inline assembly translation.
	std::vector<std::pair<std::string, awst::WType const*>> m_functionParams;
	awst::WType const* m_returnType = nullptr;

	/// Statement result stack.
	std::vector<std::shared_ptr<awst::Statement>> m_stack;

	/// Pending ArrayExtend statements from large array chunking.
	std::vector<std::shared_ptr<awst::Statement>> m_pendingExtends;

	void push(std::shared_ptr<awst::Statement> _stmt);
	std::shared_ptr<awst::Statement> pop();

	awst::SourceLocation makeLoc(solidity::langutil::SourceLocation const& _solLoc);
};

} // namespace puyasol::builder
