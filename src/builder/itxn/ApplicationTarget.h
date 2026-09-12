#pragma once

#include "awst/Node.h"

namespace puyasol::builder
{
struct TargetProfile;

/// The AVM contract-address namespace, independent of call/metadata policy.
/// Zero denotes an unresolvable address, never AVM's reference-zero alias.
class ApplicationTarget
{
public:
	using Expr = std::shared_ptr<awst::Expression>;
	static Expr canonicalId(Expr address, awst::SourceLocation const& loc);
	static Expr resolve(TargetProfile const& profile, Expr address, awst::SourceLocation const& loc);
	/// Validate before storing an address in the compact function-pointer form.
	/// The zero pointer is a valid value, but requireApplication rejects its call.
	static Expr pointerId(TargetProfile const& profile, Expr address, awst::SourceLocation const& loc);
	static Expr requireApplication(Expr id, awst::SourceLocation const& loc);
};
} // namespace puyasol::builder
