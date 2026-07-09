#pragma once

/// @file NameGen.h
/// Deterministic generated-name counters for AWST temp/subroutine names.
///
/// Replaces ~47 function-local `static int fooCounter` sites. Those statics were
/// process-monotonic: a contract's generated names (`__mod_retval_3`, `f__mod0_17`)
/// depended on every contract compiled BEFORE it in the batch — so multi-contract
/// TEAL output was compile-order-dependent, and two ContractBuilders in parallel
/// would race the counters. NameGen is thread_local and reset at each contract
/// translation entry: names depend only on the contract's own content.
///
/// Semantics: `next(prefix)` returns the current per-prefix value and increments
/// (post-increment, matching the dominant `counter++` idiom; pre-increment sites
/// migrate as `next(p) + 1`). Prefixes are the old counters' names, one namespace
/// per old static, so per-contract sequences reproduce the old per-process ones
/// exactly for single-contract compiles.

#include <string>
#include <unordered_map>

namespace puyasol::awst
{

class NameGen
{
public:
	/// Current value for _prefix, then increment. Thread-local state.
	static int next(std::string const& _prefix)
	{
		return counters()[_prefix]++;
	}

	/// Reset ALL prefixes. Call at each contract-translation entry so a
	/// contract's generated names are independent of what compiled before it.
	static void resetAll()
	{
		counters().clear();
	}

private:
	static std::unordered_map<std::string, int>& counters()
	{
		thread_local std::unordered_map<std::string, int> s_counters;
		return s_counters;
	}
};

} // namespace puyasol::awst
