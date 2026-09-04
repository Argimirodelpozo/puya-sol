/// @file ArtifactIO.h
/// Verified, atomic persistence for compiler artifacts.
#pragma once

#include <boost/filesystem/path.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace puyasol::artifact
{

struct Digest
{
	std::uintmax_t bytes = 0;
	std::string sha256;
};

struct Record
{
	std::string path;
	std::string role;
	Digest digest;
};

/// Read one regular file completely, enforcing an upper bound and returning a
/// digest of the exact bytes read.
bool readBinary(
	boost::filesystem::path const& _path,
	std::vector<std::uint8_t>& _contents,
	Digest& _digest,
	std::string& _error,
	std::uintmax_t _maxBytes = std::numeric_limits<std::uintmax_t>::max());

/// Validate JSON, write it to a fresh sibling temporary file, verify the exact
/// bytes from disk, then atomically replace the destination.
bool writeJsonAtomically(
	boost::filesystem::path const& _path,
	std::string const& _json,
	Digest& _digest,
	std::string& _error);

/// Remove one compiler-owned file if present. Directories are rejected rather
/// than recursively removed.
bool removeFileIfPresent(
	boost::filesystem::path const& _path,
	std::string& _error);

/// Atomically write the integrity manifest for a coherent artifact phase.
/// The manifest intentionally does not hash itself.
bool writeManifest(
	boost::filesystem::path const& _path,
	std::string const& _phase,
	std::vector<Record> _records,
	std::string& _error);

} // namespace puyasol::artifact
