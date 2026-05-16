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

	auto kind = StorageMapper::shouldUseBoxStorage(_var)
		? awst::AppStorageKind::Box
		: awst::AppStorageKind::AppGlobal;
	return m_mapper.createStateRead(_name, _type, kind, _loc);
}

std::shared_ptr<awst::Statement> StorageBackend::emitWriteForVar(
	solidity::frontend::VariableDeclaration const& _var,
	std::string const& _name,
	std::shared_ptr<awst::Expression> _value,
	awst::SourceLocation const& _loc) const
{
	if (isTransient(_var))
		return m_transient->buildWrite(_name, std::move(_value), _loc);

	auto kind = StorageMapper::shouldUseBoxStorage(_var)
		? awst::AppStorageKind::Box
		: awst::AppStorageKind::AppGlobal;
	auto const* type = _value ? _value->wtype : nullptr;
	// StorageMapper::createStateWrite returns an Expression (an
	// AssignmentExpression) — wrap as Statement for caller uniformity.
	auto writeExpr = m_mapper.createStateWrite(
		_name, std::move(_value), type, kind, _loc);
	return awst::makeExpressionStatement(std::move(writeExpr), _loc);
}

} // namespace puyasol::builder
