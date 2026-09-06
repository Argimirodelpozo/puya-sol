#include "builder/storage/StorageKey.h"
#include "builder/sol-types/TypeCoercion.h"
#include "json/Base85.hpp"

#include <libsolidity/ast/Types.h>

namespace puyasol::builder::StorageKey
{
namespace
{
std::vector<uint8_t> coordinate(solidity::u256 const& slot, unsigned offset)
{
	if (offset >= 32) throw std::logic_error("invalid solc holder byte offset");
	auto result = solidity::toBigEndian(slot);
	result.push_back(static_cast<uint8_t>(offset));
	return result;
}

std::shared_ptr<awst::Expression> segment(
	char tag, std::shared_ptr<awst::Expression> parent,
	std::shared_ptr<awst::Expression> payload, awst::SourceLocation const& loc)
{
	// The runtime prefix can be either a printable root or a descendant hash.
	// Length framing and distinct tags make all segment boundaries explicit.
	parent = awst::makeEvalOnce(awst::makeAsBytes(std::move(parent), loc), loc);
	auto header = awst::makeUtf8BytesConstant(std::string("puya-sol/2/") + tag, loc);
	auto framed = awst::makeConcat(header, awst::makeItob(awst::makeLen(parent, loc), loc), loc);
	framed = awst::makeConcat(std::move(framed), std::move(parent), loc);
	framed = awst::makeConcat(std::move(framed), std::move(payload), loc);
	auto hash = awst::makeIntrinsicCall("sha256", awst::WType::boxKeyType(), loc);
	hash->stackArgs.push_back(std::move(framed));
	return hash;
}
}

std::string root(solidity::u256 const& slot, unsigned offset)
{
	// 54 printable bytes, below AVM's 64-byte box-name cap. The reserved prefix
	// cannot be a Solidity identifier. Base85 preserves the full coordinate.
	return "@puya-sol/2:" + json::base85Encode(coordinate(slot, offset));
}

std::shared_ptr<awst::Expression> member(
	std::shared_ptr<awst::Expression> parent,
	solidity::frontend::StructType const& type, std::string const& name,
	awst::SourceLocation const& loc)
{
	auto const& [slot, offset] = type.storageOffsetsOfMember(name);
	return segment('s', std::move(parent),
		awst::makeBytesConstant(coordinate(slot, offset), loc), loc);
}

std::shared_ptr<awst::Expression> arrayElement(
	std::shared_ptr<awst::Expression> parent,
	std::shared_ptr<awst::Expression> index, awst::SourceLocation const& loc)
{
	index = TypeCoercion::implicitNumericCast(std::move(index), awst::WType::biguintType(), loc);
	return segment('a', std::move(parent),
		awst::makeKeyBytes(std::move(index), awst::WType::biguintType(), loc), loc);
}

std::shared_ptr<awst::Expression> mappingEntry(
	std::shared_ptr<awst::Expression> parent,
	std::shared_ptr<awst::Expression> key, awst::WType const* keyType,
	awst::SourceLocation const& loc)
{
	return segment('m', std::move(parent),
		awst::makeKeyBytes(std::move(key), keyType, loc), loc);
}

} // namespace puyasol::builder::StorageKey
