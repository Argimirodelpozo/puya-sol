#include "builder/ScratchLayout.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace
{

bool require(bool _condition, char const* _message)
{
	if (_condition)
		return true;
	std::cerr << _message << '\n';
	return false;
}

} // namespace

int main()
{
	using puyasol::builder::ScratchLayout;
	bool ok = true;

	ScratchLayout defaults;
	ok &= require(defaults.memoryFirst() == 0 && defaults.memoryLast() == 4,
		"default memory layout changed");
	ok &= require(defaults.reservedSlots().size() == 16,
		"default reservations are incomplete");

	ScratchLayout extended{7};
	ok &= require(extended.memoryFirst() == 16 && extended.memoryLast() == 22,
		"extended memory overlaps fixed ABI scratch slots");
	auto extendedSlots = extended.reservedSlots();
	ok &= require(std::find(extendedSlots.begin(), extendedSlots.end(), 5)
			!= extendedSlots.end()
		&& std::find(extendedSlots.begin(), extendedSlots.end(), 15)
			!= extendedSlots.end(),
		"fixed transient/flash reservations are missing");

	ScratchLayout maximum{240};
	ok &= require(maximum.memoryLast() == ScratchLayout::maxScratchSlot,
		"maximum CLI layout does not end at the AVM scratch limit");

	for (int invalid: {0, 241})
	{
		bool rejected = false;
		try { ScratchLayout layout{invalid}; (void)layout; }
		catch (std::invalid_argument const&) { rejected = true; }
		ok &= require(rejected, "invalid scratch layout was accepted");
	}

	return ok ? 0 : 1;
}
