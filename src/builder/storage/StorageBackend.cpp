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
	std::string const& _name,
	awst::WType const* _type,
	awst::SourceLocation const& _loc) const
{
	if (isTransient(_var))
		return m_transient->buildRead(_name, _type, _loc);

	auto kind = m_mapper.shouldUseBoxStorage(_var)
		? awst::AppStorageKind::Box
		: awst::AppStorageKind::AppGlobal;
	return m_mapper.createStateRead(
		m_mapper.storageNameFor(_var), _type, kind, _loc);
}

std::shared_ptr<awst::Statement> StorageBackend::emitWriteForVar(
	solidity::frontend::VariableDeclaration const& _var,
	std::string const& _name,
	std::shared_ptr<awst::Expression> _value,
	awst::SourceLocation const& _loc) const
{
	if (isTransient(_var))
		return m_transient->buildWrite(_name, std::move(_value), _loc);

	auto kind = m_mapper.shouldUseBoxStorage(_var)
		? awst::AppStorageKind::Box
		: awst::AppStorageKind::AppGlobal;
	auto const* type = _value ? _value->wtype : nullptr;
	// createStateWrite returns an Expression; wrap as Statement.
	auto writeExpr = m_mapper.createStateWrite(
		m_mapper.storageNameFor(_var), std::move(_value), type, kind, _loc);
	return awst::makeExpressionStatement(std::move(writeExpr), _loc);
}

} // namespace puyasol::builder
