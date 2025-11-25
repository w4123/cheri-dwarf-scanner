#include <cassert>
#include <iostream>
#include <thread>

#include "log.hh"

namespace cheri {

static std::mutex global_logger_mutex;
static std::unique_ptr<Logger> global_logger;

std::ostream &operator<<(std::ostream &os, LogLevel ll) {
  switch (ll) {
  case kTrace:
    os << "[TRACE]";
    break;
  case kDebug:
    os << "[DEBUG]";
    break;
  case kInfo:
    os << "[INFO]";
    break;
  case kWarn:
    os << "[WARN]";
    break;
  case kError:
    os << "[ERROR]";
    break;
  default:
    assert(false && "Not reached!");
  }

  return os;
}

Logger &Logger::Get() {
  std::lock_guard<std::mutex> lock(global_logger_mutex);
  if (!global_logger) {
    global_logger = std::unique_ptr<Logger>(new Logger(kInfo));
  }

  return *global_logger;
}

Logger &Logger::Default() {
  std::lock_guard<std::mutex> lock(global_logger_mutex);
  if (global_logger) {
    return *global_logger;
  }
  global_logger = std::unique_ptr<Logger>(new Logger(kInfo));
  auto sink = std::make_unique<ConsoleLogSink>();
  global_logger->AddSink(std::move(sink));

  return *global_logger;
}

Logger::Logger(LogLevel level) : level_{kInfo} {}

void Logger::AddSink(std::unique_ptr<Sink> sink) {
  std::lock_guard<std::mutex> lock(mutex_);
  sinks_.emplace_back(std::move(sink));
}

void Logger::SetLevel(LogLevel level) {
  std::lock_guard<std::mutex> lock(mutex_);
  level_ = level;
}

LogAs Logger::NewStream(LogLevel level) { return LogAs(*this, level); }

void Logger::Emit(LogMessage msg) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (msg.level > level_)
    return;
  for (auto &sink : sinks_) {
    sink->Emit(msg);
  }
}

LogAs::LogAs(Logger &logger, LogLevel level)
    : logger_{logger}, level_{level}, consumed_flag_{false} {}

LogAs::~LogAs() {
  if (consumed_flag_)
    logger_.Emit({level : level_, message : std::move(stream_.str())});
}

std::ostream &LogAs::Stream() { return stream_; }

LogAs::operator bool() {
  bool is_consumed = consumed_flag_;
  consumed_flag_ |= true;
  return is_consumed;
}

void ConsoleLogSink::Emit(LogMessage &msg) {
  switch (msg.level) {
  case kTrace:
  case kDebug:
  case kError:
    std::cerr << "[THR " << std::this_thread::get_id() << "]" << msg.level
              << " " << msg.message << std::endl;
    break;
  default:
    std::cout << "[THR " << std::this_thread::get_id() << "]" << msg.level
              << " " << msg.message << std::endl;
  }
}

} /* namespace cheri */
