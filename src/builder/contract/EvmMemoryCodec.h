#pragma once

#include "awst/Node.h"

#include "builder/sol-types/SolcFwd.h"

#include <memory>
#include <string>
#include <vector>

namespace puyasol::builder
{
class TypeMapper;

/// Read a uint64-sized EVM memory word (length, pointer, or offset), asserting
/// that its high 24 bytes are zero.  Shared by recursive materialisation and
/// AST path resolution so pointer validation cannot drift between them.
std::shared_ptr<awst::Expression> readEvmMemoryUint64Word(
	TypeMapper& typeMapper,
	std::shared_ptr<awst::Expression> offset,
	awst::SourceLocation const& loc,
	std::vector<std::shared_ptr<awst::Statement>>& out);

/// Materialise a value recursively from a Solidity EVM-memory region.  The
/// supplied offset points at the value's data (a length word for dynamic
/// arrays/bytes, element zero for fixed arrays, member zero for structs).
std::shared_ptr<awst::Expression> materializeEvmMemoryValue(
	TypeMapper& typeMapper,
	solidity::frontend::Type const* solType,
	awst::WType const* wtype,
	std::shared_ptr<awst::Expression> offset,
	awst::SourceLocation const& loc,
	std::vector<std::shared_ptr<awst::Statement>>& out);

/// Allocate an EVM-memory region and recursively spill a native/ARC4 value
/// into it.  `_offVar` is rebound to the new root pointer.
bool spillEvmMemoryValue(
	TypeMapper& typeMapper,
	solidity::frontend::Type const* solType,
	awst::WType const* wtype,
	std::shared_ptr<awst::Expression> value,
	std::string const& offVar,
	int uniqueId,
	awst::SourceLocation const& loc,
	std::vector<std::shared_ptr<awst::Statement>>& out);

/// Recursively overwrite an existing EVM-memory value region.  Static
/// aggregates are traversed using solc's memory layout; reference children
/// are freshly allocated and their pointer slots updated.  A dynamic root
/// cannot be resized without rebinding its owning pointer and returns false.
bool writeEvmMemoryValueAt(
	TypeMapper& typeMapper,
	solidity::frontend::Type const* solType,
	std::shared_ptr<awst::Expression> value,
	std::shared_ptr<awst::Expression> offset,
	awst::SourceLocation const& loc,
	std::vector<std::shared_ptr<awst::Statement>>& out);

} // namespace puyasol::builder
