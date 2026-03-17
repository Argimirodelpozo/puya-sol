#include "builder/statements/StatementBuilder.h"
#include "builder/assembly/AssemblyBuilder.h"
#include "builder/storage/StorageMapper.h"
#include "Logger.h"

#include <libsolidity/ast/ASTAnnotations.h>
#include <libsolutil/Numeric.h>
#include <libyul/AST.h>
#include <libyul/YulName.h>

#include <algorithm>
#include <sstream>

namespace puyasol::builder
{

StatementBuilder::StatementBuilder(
	ExpressionBuilder& _exprBuilder,
	TypeMapper& _typeMapper,
	std::string const& _sourceFile
)
	: m_exprBuilder(_exprBuilder), m_typeMapper(_typeMapper), m_sourceFile(_sourceFile)
{
}

std::shared_ptr<awst::Statement> StatementBuilder::build(
	solidity::frontend::Statement const& _stmt
)
{
	m_stack.clear();
	_stmt.accept(*this);
	if (m_stack.empty())
		return nullptr;
	return pop();
}

std::shared_ptr<awst::Block> StatementBuilder::buildBlock(
	solidity::frontend::Block const& _block
)
{
	auto awstBlock = std::make_shared<awst::Block>();
	awstBlock->sourceLocation = makeLoc(_block.location());

	// Track unchecked blocks for wrapping arithmetic
	bool const wasUnchecked = m_exprBuilder.inUncheckedBlock();
	if (_block.unchecked())
		m_exprBuilder.setInUncheckedBlock(true);

	for (auto const& stmt: _block.statements())
	{
		// Flatten unchecked blocks (and any nested blocks) into the parent
		if (auto const* innerBlock = dynamic_cast<solidity::frontend::Block const*>(stmt.get()))
		{
			auto translatedBlock = buildBlock(*innerBlock);
			for (auto& innerStmt: translatedBlock->body)
				awstBlock->body.push_back(std::move(innerStmt));
		}
		else
		{
			// Use the stack directly instead of build() to capture ALL
			// statements (pending inner txn submits + the primary statement).
			m_stack.clear();
			stmt->accept(*this);
			for (auto& translated: m_stack)
				if (translated)
					awstBlock->body.push_back(std::move(translated));
			m_stack.clear();
		}
	}

	m_exprBuilder.setInUncheckedBlock(wasUnchecked);
	return awstBlock;
}

void StatementBuilder::push(std::shared_ptr<awst::Statement> _stmt)
{
	m_stack.push_back(std::move(_stmt));
}

std::shared_ptr<awst::Statement> StatementBuilder::pop()
{
	if (m_stack.empty())
		return nullptr;
	auto stmt = m_stack.back();
	m_stack.pop_back();
	return stmt;
}

awst::SourceLocation StatementBuilder::makeLoc(
	solidity::langutil::SourceLocation const& _solLoc
)
{
	awst::SourceLocation loc;
	loc.file = m_sourceFile;
	loc.line = _solLoc.start >= 0 ? _solLoc.start : 0;
	loc.endLine = _solLoc.end >= 0 ? _solLoc.end : 0;
	return loc;
}


} // namespace puyasol::builder
