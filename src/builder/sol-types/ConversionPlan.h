#pragma once

#include "awst/Node.h"

#include <libsolidity/ast/Types.h>

#include <memory>

namespace puyasol::builder
{

/// Semantic implicit conversion selected from solc types, kept separate from
/// the AWST representation operations used to emit it.
class ConversionPlan
{
public:
	enum class Context { Assignment, Initialization, Argument, Return, AbiArgument };

	ConversionPlan(
		solidity::frontend::Type const* _source,
		solidity::frontend::Type const* _target,
		awst::WType const* _targetRepresentation,
		Context _context)
		: m_source(_source),
		  m_target(_target),
		  m_targetRepresentation(_targetRepresentation),
		  m_context(_context)
	{}

	std::shared_ptr<awst::Expression> emit(
		std::shared_ptr<awst::Expression> _value,
		awst::SourceLocation const& _loc) const;

	solidity::frontend::Type const* sourceType() const { return m_source; }
	solidity::frontend::Type const* targetType() const { return m_target; }

private:
	solidity::frontend::Type const* m_source;
	solidity::frontend::Type const* m_target;
	awst::WType const* m_targetRepresentation;
	Context m_context;
};

} // namespace puyasol::builder
