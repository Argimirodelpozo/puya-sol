#pragma once

#include "awst/Node.h"

namespace puyasol::builder
{
class TypeMapper;

/// Padded source read: offsets may be EVM words; the checked output length
/// is uint64. Source bytes beyond the buffer are zero, without narrowing a
/// wide offset before comparing it to the source size.
std::shared_ptr<awst::Expression> readPaddedBytes(
	TypeMapper& types, std::shared_ptr<awst::Expression> bytes,
	std::shared_ptr<awst::Expression> offset, std::shared_ptr<awst::Expression> length,
	awst::SourceLocation const& loc);

} // namespace puyasol::builder
