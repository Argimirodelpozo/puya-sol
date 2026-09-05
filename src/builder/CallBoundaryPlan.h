#pragma once

#include <cstddef>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace solidity::frontend { class VariableDeclaration; }
namespace puyasol::awst { class WType; struct Block; struct Expression; struct SourceLocation; }

namespace puyasol::builder
{
class TypeMapper;

enum class RefParamPassing { SlotHandle, BoxKeyPrefix, BlobOffset, Value };

struct CallParameterPlan
{
	solidity::frontend::VariableDeclaration const* declaration = nullptr;
	std::string name;
	awst::WType const* type = nullptr;
	awst::WType const* wireType = nullptr;
	unsigned signedDecodeBits = 0;
	RefParamPassing passing = RefParamPassing::Value;

	std::string wireName() const { return wireType == type ? name : "__arc4_" + name; }
	std::string offsetName() const { return name + "__off"; }
	std::shared_ptr<awst::Expression> encodeArgument(
		std::shared_ptr<awst::Expression> value, awst::SourceLocation const& loc) const;
};

/// One declaration/host-specific physical signature. All indices address
/// source parameters; companion offsets follow them, in offsetParams order.
struct CallBoundaryPlan
{
	std::vector<CallParameterPlan> parameters;
	std::set<size_t> slotParams, keyParams, blobParams, asmSlotParams;
	std::vector<size_t> offsetParams, storageWriteBackParams, memoryWriteBackParams, writeBackParams;

	awst::WType const* augmentReturn(TypeMapper& types, awst::WType const* original) const;
	void augmentReturns(awst::Block& body, awst::WType const* augmented) const;
};

/// Adapt the actual emitted callee return to its caller's native carrier.
std::shared_ptr<awst::Expression> decodeCallResult(
	std::shared_ptr<awst::Expression> value, awst::WType const* native,
	awst::SourceLocation const& loc);

} // namespace puyasol::builder
