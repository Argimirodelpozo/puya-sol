/// @file StorageBackend.cpp

#include "builder/storage/StorageBackend.h"
#include "builder/storage/StorageMapper.h"
#include "builder/storage/TransientStorage.h"

namespace puyasol::builder
{

bool StorageBackend::isTransient(
	solidity::frontend::VariableDeclaration const& _var) const
{
	return m_transient && m_transient->isTransient(_var);
}

std::shared_ptr<awst::Expression> StorageBackend::emitReadForVar(
	solidity::frontend::VariableDeclaration const& _var,
	awst::SourceLocation const& _loc) const
{
	if (isTransient(_var))
		return m_transient->buildRead(_var, _loc);

	auto binding = m_mapper.physicalBindingFor(_var);
	return m_mapper.createStateRead(binding, _loc);
}

std::shared_ptr<awst::Statement> StorageBackend::emitWriteForVar(
	solidity::frontend::VariableDeclaration const& _var,
	std::shared_ptr<awst::Expression> _value,
	awst::SourceLocation const& _loc) const
{
	if (isTransient(_var))
		return m_transient->buildWrite(_var, std::move(_value), _loc);

	auto binding = m_mapper.physicalBindingFor(_var);
	// createStateWrite returns an Expression; wrap as Statement.
	auto writeExpr = m_mapper.createStateWrite(binding, std::move(_value), _loc);
	return awst::makeExpressionStatement(std::move(writeExpr), _loc);
}

} // namespace puyasol::builder
