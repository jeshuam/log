#include "log_new.h"

#include <array>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <fstream>
#include <mutex>
#include <thread>

#include <gflags/gflags.h>
#include <boost/filesystem.hpp>

#include "util/string/constants.h"
#include "util/string/util.h"

// TODO: features to add:
//    - cleanup the code a lot
//    - setting the minimum log level for display
//    - logging to multiple streams (different files per message type etc.)
//      e.g. LOG.TRACE, LOG.INFO (where LOG.DEBUG includes DEBUG and above)
//    X get colors working properly
//    - log file rotation based on a flag
//    - trim filenames in log messages and line numbers for consistent line
//      lengths
//    - display more precise time with the datetime
//    - allow displaying thread number
//    - LOG_IF(n)
//    - LOG_EVERY_N
//    - LOG_IF_EVERY_N
//    - LOG_FIRST_N
//    - DLOG* (logs compiled out depending on configuration)
//    - VLOG(level); support arbitrary levels

// OUTPUT TOGGLES
DEFINE_bool(logtofile, false,
            "Whether or not log messages should be output to a file.");
DEFINE_bool(logtostderr, false,
            "Whether or not log messages should be output to stderr.");

// OUTPUT OPTIONS
DEFINE_string(logfile_dir, "log", "The directory to store logfiles in.");

DEFINE_string(logfile_name, "",
              "The name of the logfile to create. Will be suffixed with the "
              "logging level. Defaults to the binary name seen by gflags.");

DEFINE_bool(colorize_output, true,
            "When enabled, colorize output messages. This is not done when "
            "logging to files.");

DEFINE_string(min_log_level, "info",
              "The minimum log level to display when writing to stderr. Note "
              "that multiple log files are written, so this flag will not "
              "affect those.");

DEFINE_string(min_log_level_file, "trace",
              "The minimum log level to display when writing to files. If set "
              "to anything other than TRACE, files lower than the given level "
              "will not be created.");

DEFINE_uint32(v, 0,
              "The verbosity level to log at. Only messages with a verbosity "
              "level <= this will be logged.");

// OUTPUT FILE OPTIONS
DEFINE_uint32(logfile_max_size_mb, 50,
              "The maximum number of MiB a single logging file will take up. "
              "Note that actual disk usage might vary, because the logging "
              "system will keep the previous log file in addition to the "
              "current log file.");

// OUTPUT FORMATS
DEFINE_string(line_format,
              "{nc}{lc}{level}{nc} {bold}{white}@{nc} {gray}{datetime}{nc} "
              ": {white}{italic}{file}:{line}{nc} {bold}{white}::{nc} "
              "{lc}{message}{nc}",
              "A Python-ish format string of the log header information.");

DEFINE_string(datetime_format, "%a %b %d %T",
              "The strftime formatting to use for {datetime} substitution.");

// PROCESSING OPTIONS
DEFINE_bool(async_logging, false,
            "When enabled, perform logging asynchronously.");

DEFINE_uint32(async_queue_max_len, 10000,
              "Maximum number of log messages to be stored in the queue until "
              "any additional messages are blocked.");

namespace cpplog {

namespace internal {

namespace {

/**
 * The message queue to store log messages in.
 */
std::deque<LogMessage> LOG_MESSAGE_QUEUE;
std::condition_variable LOG_MESSAGE_QUEUE_INSERT;

/**
 * Log files to write to. They will be opened only once (when they are used) and
 * will be written to from then on.
 */
std::array<std::ofstream, N_LEVELS> LOG_FILES;

// UTILITY FUNCTIONS.
std::string _LevelToString(Level level) {
  switch (level) {
    case TRACE:
      return "T";
    case DEBUG:
      return "D";
    case INFO:
      return "I";
    case WARNING:
      return "W";
    case ERROR:
      return "E";
    case FATAL:
      return "F";
    default:
      return "?";
  }
}

std::string _LevelToLongString(Level level) {
  switch (level) {
    case TRACE:
      return "TRACE";
    case DEBUG:
      return "DEBUG";
    case INFO:
      return "INFO";
    case WARNING:
      return "WARNING";
    case ERROR:
      return "ERROR";
    case FATAL:
      return "FATAL";
    default:
      return "?";
  }
}

Level _StringToLevel(const std::string& level) {
  auto level_upper = string::ToUpper(level);
  if (level_upper == "TRACE") {
    return TRACE;
  } else if (level_upper == "DEBUG") {
    return DEBUG;
  } else if (level_upper == "INFO") {
    return INFO;
  } else if (level_upper == "WARNING") {
    return WARNING;
  } else if (level_upper == "ERROR") {
    return ERROR;
  } else if (level_upper == "FATAL") {
    return FATAL;
  } else {
    return TRACE;
  }
}

std::string _GetColor(Level level) {
  switch (level) {
    case TRACE:
    case DEBUG:
      return string::color::kGray;
    case INFO:
#ifdef OS_WINDOWS
      return string::color::kCyan + string::color::kBold;
#else
      return string::color::kBlue + string::color::kBold;
#endif  // OS_WINDOWS
    case WARNING:
      return string::color::kYellow + string::color::kBold;
    case ERROR:
    case FATAL:
      return string::color::kRed + string::color::kBold;
    default:
      return "";
  }
}

/**
 * @brief      Actually emit a message to all output streams.
 *
 *             This will, based on the current logging configuration, either:
 *             - Output to a file, based on the logging level.
 *             - Output to the display, based on the minimum level.
 *
 *             Calls to Emit() will be locked during synchronous mode to ensure
 *             that multiple threads do not print over eachother.
 *
 * @param[in]  msg   The message to emit.
 */
void _DoEmitMessage(const LogMessage& msg) {
  static std::mutex emit_lock;

  // If we aren't logging, then stop. This might make things a bit faster when
  // logging is disabled.
  if (!FLAGS_logtofile && !FLAGS_logtostderr) {
    return;
  }

  if (!FLAGS_async_logging) emit_lock.lock();
  msg.Emit(FLAGS_line_format);
  if (!FLAGS_async_logging) emit_lock.unlock();
}

/**
 * @brief      Function called within a thread to process messages. Will only be
 *             used if --async_logging is enabled.
 */
void _ProcessMessageQueue() {
  std::mutex mutex;
  std::unique_lock<std::mutex> lock(mutex);

  while (true) {
    // Wait for something to appear.
    LOG_MESSAGE_QUEUE_INSERT.wait(lock,
                                  [] { return LOG_MESSAGE_QUEUE.size() > 1; });

    // Get the next thing to display.
    auto msg = LOG_MESSAGE_QUEUE.front();

    // Emit the message.
    _DoEmitMessage(msg);

    // Remove it from the queue now that it has been displayed.
    LOG_MESSAGE_QUEUE.pop_front();
  }
}

}  // namespace

LogMessage::LogMessage(Level level, int verbosity, int line,
                       const std::string& file, const std::string& msg_format,
                       const string::FormatListType& format_args)
    : level_(level),
      verbosity_(verbosity),
      line_(line),
      file_(boost::filesystem::path(file).filename().string()),
      log_time_(std::chrono::system_clock::now()),
      msg_format_(msg_format),
      format_args_(format_args) {}

void LogMessage::Emit(const std::string& line_fmt) const {
  // If this message is too verbose, then just ignore it.
  if (verbosity_ > FLAGS_v) {
    return;
  }

  // Format the message.
  auto msg_formatted = string::Format(msg_format_, format_args_);

  // Format the datetime.
  char time_str_buffer[256];
  auto log_time_c = std::chrono::system_clock::to_time_t(log_time_);
  std::strftime(time_str_buffer, sizeof(time_str_buffer),
                FLAGS_datetime_format.c_str(), std::localtime(&log_time_c));

  // Format the line.
  auto line_formatted =
      string::FormatMap(line_fmt, {{"message", msg_formatted},
                                   {"file", file_},
                                   {"line", line_},
                                   {"datetime", time_str_buffer},
                                   {"level", _LevelToString(level_)}},
                        true);

  // Add colors if requested.
  std::string line_color = line_formatted;
  if (FLAGS_colorize_output) {
    line_color = string::FormatMap(line_color,
                                   {
                                       {"nc", string::color::kReset},
                                       {"bold", string::color::kBold},
                                       {"italic", string::color::kItalic},
                                       {"black", string::color::kBlack},
                                       {"red", string::color::kRed},
                                       {"green", string::color::kGreen},
                                       {"yellow", string::color::kYellow},
                                       {"blue", string::color::kBlue},
                                       {"magenta", string::color::kMagenta},
                                       {"cyan", string::color::kCyan},
                                       {"white", string::color::kWhite},
                                       {"gray", string::color::kGray},
                                       {"lc", _GetColor(level_)},
                                   },
                                   true);
  }

  // Output to stdout.
  if (FLAGS_logtostderr && level_ >= _StringToLevel(FLAGS_min_log_level)) {
    std::cerr << string::FormatTrimTags(line_color) << std::endl;
  }

  // Output to files.
  if (FLAGS_logtofile) {
    // Log to all of the relevant files.
    auto min_level = _StringToLevel(FLAGS_min_log_level_file);
    for (int i = min_level; i <= level_; i++) {
      auto out_file_path =
          boost::filesystem::path(FLAGS_logfile_dir) /
          (FLAGS_logfile_name + "." + _LevelToLongString((Level)i));
      std::ofstream& out_file = LOG_FILES[i];
      if (!out_file.is_open()) {
        out_file.open(out_file_path.string());
      }

      // Check to see if we need to rotate the file.
      auto file_size = boost::filesystem::file_size(out_file_path);
      if (file_size / 1024.0 / 1024.0 > FLAGS_logfile_max_size_mb) {
        // Close the file.
        out_file.close();

        // Move the file to the file + 1.
        boost::filesystem::rename(out_file_path,
                                  out_file_path.string() + ".old");
        out_file.open(out_file_path.string());
      }

      out_file << string::FormatTrimTags(line_formatted) << std::endl;
      out_file.flush();
    }
  }
}

void QueueMessage(const LogMessage& msg) {
  static std::mutex insert_lock;

  if (FLAGS_async_logging) {
    insert_lock.lock();
    // Block until the message queue is a sensible size.
    while (LOG_MESSAGE_QUEUE.size() > FLAGS_async_queue_max_len) {
      ;
    }

    LOG_MESSAGE_QUEUE.push_back(msg);
    insert_lock.unlock();
    LOG_MESSAGE_QUEUE_INSERT.notify_one();
  } else {
    _DoEmitMessage(msg);
  }

  // If the message was fatal, die.
  if (msg.level() == FATAL) {
    std::exit(EXIT_FAILURE);
  }
}

Logger::~Logger() {
  while (internal::LOG_MESSAGE_QUEUE.size() > 1) {
    ;  // do nothing, wait for the thread
  }
}

}  // namespace internal

internal::Logger Init() {
  // Start the thread, if required.
  if (FLAGS_async_logging) {
    // Insert a dummy message into the log, which will never be logged.
    internal::LOG_MESSAGE_QUEUE.push_back(
        internal::LogMessage(internal::TRACE, 0, 0, "", "", {}));
    new std::thread(internal::_ProcessMessageQueue);
  }

  // Make the logging output directory.
  if (FLAGS_logtofile) {
    boost::filesystem::create_directories(FLAGS_logfile_dir);

    if (FLAGS_logfile_name.empty()) {
      FLAGS_logfile_name =
          boost::filesystem::basename(gflags::ProgramInvocationName());
    }
  }

  return internal::Logger();
}

}  // namespace cpplog

int main(int argc, char** argv) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  static auto _ = cpplog::Init();

  while (true) {
    LOG_TRACE("a = {}, {}", {1, "c"});
    LOG_DEBUG("a = {}, {}", {1, "c"});
    LOG_INFO("a = {}, {}", {1, "c"});
    LOG_WARNING("a = {}, {}", {1, "c"});
    LOG_ERROR("a = {}, {}", {1, "c"});
  }

  LOG_FATAL("a = {}, {}", {1, "c"});

  return 0;
}
