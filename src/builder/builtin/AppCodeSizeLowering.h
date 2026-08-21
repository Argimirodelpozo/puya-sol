#pragma once

#include "awst/Node.h"

#include <memory>
#include <vector>

namespace puyasol::builder
{

class TypeMapper;

/// Non-materialising approximation of EVM code size for an AVM application.
///
/// AVM can return an approval program as bytes, but a program larger than the
/// stack byte-value limit cannot be loaded merely to take its length.  The
/// application metadata does expose existence and allocated program pages, so
/// both Solidity `address.code.length` and Yul `extcodesize` use this shared
/// lowering: zero for a missing app, otherwise its allocated byte capacity.
/// This preserves the portable zero/nonzero contract-existence semantics for
/// every program size without special-casing particular program shapes.
class AppCodeSizeLowering
{
public:
	static std::shared_ptr<awst::Expression> lower(
		TypeMapper& _typeMapper,
		std::shared_ptr<awst::Expression> _application,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _effects);
};

} // namespace puyasol::builder
