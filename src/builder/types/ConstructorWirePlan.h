#pragma once

#include "builder/types/CallBoundaryPlan.h"

namespace solidity::frontend { class FunctionDefinition; }

namespace puyasol::builder
{

/// The create reader and deferred ARC4 router are distinct entry conventions.
/// Their parameter types, names, encoders and decoders share this plan.
class ConstructorWirePlan
{
public:
	using Expr = std::shared_ptr<awst::Expression>;
	using Statements = std::vector<std::shared_ptr<awst::Statement>>;
	ConstructorWirePlan(TypeMapper& types,
		solidity::frontend::FunctionDefinition const* constructor, bool deferred);
	std::vector<CallParameterPlan> parameters;
	std::string postInitSignature() const;
	Expr encode(size_t index, Expr value, awst::SourceLocation const& loc) const;
	Expr decodeCreate(size_t index, Expr bytes, awst::SourceLocation const& loc, Statements& out) const;
	Expr decodeParameter(size_t index, Expr wire, awst::SourceLocation const& loc, Statements& out) const;

private:
	TypeMapper& m_types;
	bool m_deferred;
	bool m_validate = true;
	Expr decodeScalar(size_t index, Expr bytes, awst::SourceLocation const& loc, Statements& out) const;
};

} // namespace puyasol::builder
