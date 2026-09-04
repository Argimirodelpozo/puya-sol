#include "ArtifactIO.h"

#include <libsolutil/picosha2.h>

#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <limits>
#include <set>
#include <tuple>

namespace fs = boost::filesystem;
using njson = nlohmann::ordered_json;

namespace puyasol::artifact
{
namespace
{

Digest digest(std::vector<std::uint8_t> const& _contents)
{
	return {
		.bytes = _contents.size(),
		.sha256 = picosha2::hash256_hex_string(_contents),
	};
}

bool validJson(std::string const& _contents)
{
	return !njson::parse(_contents, nullptr, false).is_discarded();
}

class TempFile
{
public:
	explicit TempFile(fs::path _path): m_path(std::move(_path)) {}
	~TempFile()
	{
		if (!m_keep)
		{
			boost::system::error_code ignored;
			fs::remove(m_path, ignored);
		}
	}

	fs::path const& path() const { return m_path; }
	void release() { m_keep = true; }

private:
	fs::path m_path;
	bool m_keep = false;
};

} // namespace

bool readBinary(
	fs::path const& _path,
	std::vector<std::uint8_t>& _contents,
	Digest& _digest,
	std::string& _error,
	std::uintmax_t _maxBytes)
{
	boost::system::error_code ec;
	auto status = fs::status(_path, ec);
	if (ec || !fs::exists(status))
	{
		_error = "missing artifact " + _path.string()
			+ (ec ? ": " + ec.message() : "");
		return false;
	}
	if (!fs::is_regular_file(status))
	{
		_error = "artifact is not a regular file: " + _path.string();
		return false;
	}

	auto const size = fs::file_size(_path, ec);
	if (ec)
	{
		_error = "cannot determine artifact size " + _path.string()
			+ ": " + ec.message();
		return false;
	}
	if (size > _maxBytes)
	{
		_error = "artifact " + _path.string() + " is " + std::to_string(size)
			+ " bytes; maximum is " + std::to_string(_maxBytes);
		return false;
	}
	if (size > std::numeric_limits<std::size_t>::max()
		|| size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max()))
	{
		_error = "artifact is too large to read: " + _path.string();
		return false;
	}

	std::ifstream input(_path.string(), std::ios::binary);
	if (!input)
	{
		_error = "cannot open artifact " + _path.string();
		return false;
	}
	std::vector<std::uint8_t> contents(static_cast<std::size_t>(size));
	if (size != 0)
	{
		input.read(
			reinterpret_cast<char*>(contents.data()),
			static_cast<std::streamsize>(size));
		if (input.gcount() != static_cast<std::streamsize>(size) || input.bad())
		{
			_error = "short or failed read of artifact " + _path.string();
			return false;
		}
	}
	char extra = 0;
	if (input.get(extra))
	{
		_error = "artifact changed while being read: " + _path.string();
		return false;
	}
	if (input.bad())
	{
		_error = "failed reading artifact " + _path.string();
		return false;
	}

	_contents = std::move(contents);
	_digest = digest(_contents);
	return true;
}

bool writeJsonAtomically(
	fs::path const& _path,
	std::string const& _json,
	Digest& _digest,
	std::string& _error)
{
	if (!validJson(_json))
	{
		_error = "refusing to write invalid JSON to " + _path.string();
		return false;
	}
	if (_json.size() > static_cast<std::size_t>(
		std::numeric_limits<std::streamsize>::max()))
	{
		_error = "JSON artifact is too large to write: " + _path.string();
		return false;
	}

	auto parent = _path.parent_path();
	if (parent.empty())
		parent = ".";
	boost::system::error_code ec;
	if (!fs::is_directory(parent, ec) || ec)
	{
		_error = "artifact destination directory is unavailable: "
			+ parent.string() + (ec ? ": " + ec.message() : "");
		return false;
	}

	fs::path tempPath;
	try
	{
		tempPath = parent / (_path.filename().string() + ".tmp-"
			+ fs::unique_path("%%%%%%%%").string());
	}
	catch (fs::filesystem_error const& e)
	{
		_error = "cannot allocate temporary artifact path for " + _path.string()
			+ ": " + e.what();
		return false;
	}
	TempFile temp(tempPath);
	{
		std::ofstream output(temp.path().string(),
			std::ios::binary | std::ios::trunc);
		if (!output)
		{
			_error = "cannot open temporary artifact " + temp.path().string();
			return false;
		}
		output.write(_json.data(), static_cast<std::streamsize>(_json.size()));
		output.flush();
		output.close();
		if (!output)
		{
			_error = "failed writing temporary artifact " + temp.path().string();
			return false;
		}
	}

	std::vector<std::uint8_t> written;
	Digest writtenDigest;
	if (!readBinary(temp.path(), written, writtenDigest, _error, _json.size()))
		return false;
	std::string verified(written.begin(), written.end());
	if (verified != _json)
	{
		_error = "verification mismatch for temporary artifact "
			+ temp.path().string();
		return false;
	}
	if (!validJson(verified))
	{
		_error = "temporary artifact is not valid JSON: " + temp.path().string();
		return false;
	}

	fs::rename(temp.path(), _path, ec);
	if (ec)
	{
		_error = "cannot atomically replace " + _path.string()
			+ ": " + ec.message();
		return false;
	}
	temp.release();
	_digest = std::move(writtenDigest);
	return true;
}

bool removeFileIfPresent(fs::path const& _path, std::string& _error)
{
	boost::system::error_code ec;
	auto status = fs::symlink_status(_path, ec);
	if (ec)
	{
		if (ec.default_error_condition()
			== boost::system::errc::make_error_condition(
				boost::system::errc::no_such_file_or_directory))
			return true;
		_error = "cannot inspect stale artifact " + _path.string()
			+ ": " + ec.message();
		return false;
	}
	if (!fs::exists(status))
		return true;
	if (fs::is_directory(status))
	{
		_error = "refusing to remove artifact path because it is a directory: "
			+ _path.string();
		return false;
	}
	if (!fs::remove(_path, ec) || ec)
	{
		_error = "cannot remove stale artifact " + _path.string()
			+ (ec ? ": " + ec.message() : "");
		return false;
	}
	return true;
}

bool writeManifest(
	fs::path const& _path,
	std::string const& _phase,
	std::vector<Record> _records,
	std::string& _error)
{
	if (_phase.empty())
	{
		_error = "artifact manifest phase cannot be empty";
		return false;
	}
	std::sort(_records.begin(), _records.end(),
		[](Record const& a, Record const& b) {
			return std::tie(a.path, a.role) < std::tie(b.path, b.role);
		});
	std::set<std::string> paths;
	njson files = njson::array();
	for (auto const& record: _records)
	{
		fs::path relative(record.path);
		if (record.path.empty() || record.role.empty() || relative.is_absolute()
			|| record.digest.sha256.size() != 64 || !paths.insert(record.path).second)
		{
			_error = "invalid or duplicate artifact manifest record: " + record.path;
			return false;
		}
		for (auto const& component: relative)
			if (component == "..")
			{
				_error = "artifact manifest path escapes output directory: "
					+ record.path;
				return false;
			}
		files.push_back({
			{"path", record.path},
			{"role", record.role},
			{"bytes", record.digest.bytes},
			{"sha256", record.digest.sha256},
		});
	}

	njson manifest{
		{"schema", "puya-sol/artifact-manifest/v1"},
		{"phase", _phase},
		{"scope", "compiler artifacts; excludes the mutable log and this manifest"},
		{"files", std::move(files)},
	};
	Digest ignored;
	return writeJsonAtomically(_path, manifest.dump(2) + '\n', ignored, _error);
}

} // namespace puyasol::artifact
