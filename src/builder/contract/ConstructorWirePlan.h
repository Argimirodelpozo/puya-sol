#pragma once

#include "builder/CallBoundaryPlan.h"

namespace solidity::frontend { class FunctionDefinition; }

namespace puyasol::builder
{

/// The create reader and deferred ARC4 router are distinct entry conventions.
/// Their parameter types, names, encoders and decoders share this plan.
class ConstructorWirePlan
{
public:
	using Expr = std::shared_ptr<awst::Expression>;
	ConstructorWirePlan(TypeMapper& types,
		solidity::frontend::FunctionDefinition const* constructor, bool deferred);
	std::vector<CallParameterPlan> parameters;
	std::string postInitSignature() const;
	Expr encode(size_t index, Expr value, awst::SourceLocation const& loc) const;
	Expr decodeCreate(size_t index, Expr bytes, awst::SourceLocation const& loc) const;
	Expr decodeParameter(size_t index, Expr wire, awst::SourceLocation const& loc) const;

private:
	TypeMapper& m_types;
	bool m_deferred;
};

} // namespace puyasol::builder
