#include "Logger.h"

#include <cstdlib>
#include <iostream>
#include <unistd.h>

namespace puyasol
{

Logger& Logger::instance()
{
	static Logger logger;
	return logger;
}

Logger::Logger()
	: m_colorEnabled(detectColorSupport())
{
}

void Logger::setMinLevel(LogLevel _level)
{
	m_minLevel = _level;
}

void Logger::setColorEnabled(bool _enabled)
{
	m_colorEnabled = _enabled;
}

void Logger::debug(std::string const& _msg)
{
	log(LogLevel::Debug, _msg, nullptr);
}

void Logger::debug(std::string const& _msg, awst::SourceLocation const& _loc)
{
	log(LogLevel::Debug, _msg, &_loc);
}

void Logger::info(std::string const& _msg)
{
	log(LogLevel::Info, _msg, nullptr);
}

void Logger::info(std::string const& _msg, awst::SourceLocation const& _loc)
{
	log(LogLevel::Info, _msg, &_loc);
}

void Logger::warning(std::string const& _msg)
{
	log(LogLevel::Warning, _msg, nullptr);
	++m_warningCount;
}

void Logger::warning(std::string const& _msg, awst::SourceLocation const& _loc)
{
	log(LogLevel::Warning, _msg, &_loc);
	++m_warningCount;
}

void Logger::error(std::string const& _msg)
{
	log(LogLevel::Error, _msg, nullptr);
	++m_errorCount;
}

void Logger::error(std::string const& _msg, awst::SourceLocation const& _loc)
{
	log(LogLevel::Error, _msg, &_loc);
	++m_errorCount;
}

int Logger::warningCount() const
{
	return m_warningCount;
}

int Logger::errorCount() const
{
	return m_errorCount;
}

bool Logger::hasErrors() const
{
	return m_errorCount > 0;
}

void Logger::resetCounters()
{
	m_warningCount = 0;
	m_errorCount = 0;
}

void Logger::setOutputLogFile(std::string const& _path)
{
	m_logFile.open(_path, std::ios::out | std::ios::trunc);
}

void Logger::closeLogFile()
{
	if (m_logFile.is_open())
		m_logFile.close();
}

void Logger::log(LogLevel _level, std::string const& _msg, awst::SourceLocation const* _loc)
{
	if (_level < m_minLevel)
		return;

	std::cerr << levelColor(_level);
	if (_loc && !_loc->file.empty())
		std::cerr << formatLocation(*_loc) << " ";
	std::cerr << levelString(_level) << ": " << resetColor() << _msg << std::endl;

	if (m_logFile.is_open())
	{
		if (_loc && !_loc->file.empty())
			m_logFile << formatLocation(*_loc) << " ";
		m_logFile << levelString(_level) << ": " << _msg << std::endl;
	}
}

std::string Logger::formatLocation(awst::SourceLocation const& _loc)
{
	std::string result = _loc.file;
	if (_loc.line > 0)
		result += ":" + std::to_string(_loc.line);
	return result;
}

char const* Logger::levelString(LogLevel _level)
{
	switch (_level)
	{
	case LogLevel::Debug:   return "debug";
	case LogLevel::Info:    return "info";
	case LogLevel::Warning: return "warning";
	case LogLevel::Error:   return "error";
	}
	return "unknown";
}

char const* Logger::levelColor(LogLevel _level) const
{
	if (!m_colorEnabled)
		return "";
	switch (_level)
	{
	case LogLevel::Debug:   return "\033[36m";      // cyan
	case LogLevel::Info:    return "\033[32m";       // green
	case LogLevel::Warning: return "\033[33m";       // yellow
	case LogLevel::Error:   return "\033[1;31m";     // bold red
	}
	return "";
}

char const* Logger::resetColor() const
{
	if (!m_colorEnabled)
		return "";
	return "\033[0m";
}

bool Logger::detectColorSupport()
{
	if (std::getenv("NO_COLOR"))
		return false;
	return isatty(fileno(stderr)) != 0;
}

} // namespace puyasol
