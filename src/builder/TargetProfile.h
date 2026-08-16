#pragma once

#include "builder/ScratchLayout.h"

#include <liblangutil/EVMVersion.h>

#include <optional>

namespace puyasol::builder
{

/// Target choices and unit-wide storage facts for one compiler invocation.
/// Builder services receive this through TypeMapper instead of consulting
/// mutable process-global switches.
struct TargetProfile
{
	bool evmStorageLayout = false;
	bool evmMemoryLayout = false;
	bool evmSelectors = false;
	bool viaIRSequencing = false;
	bool denseOnlyStorage = false;
	bool singlePageStorage = false;
	std::optional<solidity::langutil::EVMVersion> evmVersion;
	ScratchLayout scratchLayout;
};

} // namespace puyasol::builder
