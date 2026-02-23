#pragma once

#include "awst/WType.h"

#include <libsolidity/ast/Types.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace puyasol::builder
{

/// Maps Solidity types to AWST WTypes.
class TypeMapper
{
public:
	/// Map a Solidity type to an AWST WType.
	/// Returns nullptr for unsupported types.
	awst::WType const* map(solidity::frontend::Type const* _solType);

	/// Get or create an ARC4Struct WType for a Solidity struct.
	awst::WType const* mapStruct(solidity::frontend::StructType const* _structType);

	/// Create and register a new owned type.
	template <typename T, typename... Args>
	awst::WType const* createType(Args&&... _args)
	{
		auto ptr = std::make_unique<T>(std::forward<Args>(_args)...);
		auto* raw = ptr.get();
		m_ownedTypes.push_back(std::move(ptr));
		return raw;
	}

private:
	/// Owns all dynamically-created WTypes.
	std::vector<std::unique_ptr<awst::WType>> m_ownedTypes;

	/// Cache: Solidity type string → WType.
	std::map<std::string, awst::WType const*> m_cache;
};

} // namespace puyasol::builder
