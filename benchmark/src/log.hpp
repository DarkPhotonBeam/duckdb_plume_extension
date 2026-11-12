#pragma once

#include <iostream>
#include <mutex>
#include <sstream>

// Minimal logging helpers for plume_ext_bench.
//
//   plume_ext::log::Info("loaded ", n, " configs");
//   plume_ext::log::Debug("plan:\n", plan);   // only printed when verbose
//
// Error/Warn go to stderr, Info/Debug to stdout. Each call emits one whole
// line under a mutex, so lines from different threads don't interleave.
namespace plume_ext::log {

enum class Level { Error = 0, Warn = 1, Info = 2, Debug = 3 };

inline Level &CurrentLevel() {
	static Level level = Level::Info;
	return level;
}

inline void SetLevel(Level level) {
	CurrentLevel() = level;
}

inline bool Enabled(Level level) {
	return static_cast<int>(level) <= static_cast<int>(CurrentLevel());
}

namespace detail {
inline std::mutex &Mutex() {
	static std::mutex m;
	return m;
}

template <typename... Args>
void Write(Level level, std::ostream &out, const char *prefix, Args &&...args) {
	if (!Enabled(level)) {
		return;
	}
	std::ostringstream line;
	line << prefix;
	(line << ... << args);
	line << '\n';

	std::lock_guard<std::mutex> lock(Mutex());
	out << line.str() << std::flush;
}
} // namespace detail

template <typename... Args>
void Error(Args &&...args) {
	detail::Write(Level::Error, std::cerr, "error: ", std::forward<Args>(args)...);
}
template <typename... Args>
void Warn(Args &&...args) {
	detail::Write(Level::Warn, std::cerr, "warning: ", std::forward<Args>(args)...);
}
template <typename... Args>
void Info(Args &&...args) {
	detail::Write(Level::Info, std::cout, "", std::forward<Args>(args)...);
}
template <typename... Args>
void Debug(Args &&...args) {
	detail::Write(Level::Debug, std::cout, "debug: ", std::forward<Args>(args)...);
}

} // namespace plume_ext::log
