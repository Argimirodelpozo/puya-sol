#pragma once

/// Byte-level integer encoding helpers. Solidity legality is checked by
/// ConversionPlan; ARC4 widths/signed aliases preserve solc's element facts.

#include "awst/Node.h"

#include <memory>
#include <vector>

namespace puyasol::builder
{

/// Low N two's-complement bits of a uint64, encoded as arc4.uintN/intN.
std::shared_ptr<awst::Expression> tryNarrowUInt64ToArc4UIntN(
	std::shared_ptr<awst::Expression> _value,
	awst::WType const* _targetType,
	awst::SourceLocation const& _loc);

/// Integer-element widening across supported fixed/dynamic array shapes.
/// Selects the conversion before emitting effects; nullptr means no effects
/// were emitted. The source is evaluated once. Runtime loops need _pre.
std::shared_ptr<awst::Expression> tryWidenArc4ArrayInt(
	std::shared_ptr<awst::Expression> _value,
	awst::WType const* _targetType,
	std::vector<std::shared_ptr<awst::Statement>>* _pre,
	awst::SourceLocation const& _loc);

} // namespace puyasol::builder
