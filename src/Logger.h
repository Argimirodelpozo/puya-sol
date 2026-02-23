#pragma once

#include "awst/SourceLocation.h"

#include <string>

namespace puyasol
{

enum class LogLevel { Debug, Info, Warning, Error };

class Logger
{
public:
	static Logger& instance();

	void setMinLevel(LogLevel _level);
	void setColorEnabled(bool _enabled);

	void debug(std::string const& _msg);
	void debug(std::string const& _msg, awst::SourceLocation const& _loc);
	void info(std::string const& _msg);
	void info(std::string const& _msg, awst::SourceLocation const& _loc);
	void warning(std::string const& _msg);
	void warning(std::string const& _msg, awst::SourceLocation const& _loc);
	void error(std::string const& _msg);
	void error(std::string const& _msg, awst::SourceLocation const& _loc);

	int warningCount() const;
	int errorCount() const;
	bool hasErrors() const;
	void resetCounters();

private:
	Logger();
	void log(LogLevel _level, std::string const& _msg, awst::SourceLocation const* _loc);
	static std::string formatLocation(awst::SourceLocation const& _loc);
	static char const* levelString(LogLevel _level);
	char const* levelColor(LogLevel _level) const;
	char const* resetColor() const;
	static bool detectColorSupport();

	LogLevel m_minLevel = LogLevel::Info;
	bool m_colorEnabled = false;
	int m_warningCount = 0;
	int m_errorCount = 0;
};

} // namespace puyasol
