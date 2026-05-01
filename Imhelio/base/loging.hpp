// Copyright 2017, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#pragma once

#include <glog/logging.h>

#define CONSOLE_INFO LOG_TO_SINK(base::ConsoleLogSink::instance(), INFO)


#include <string>

namespace base {

std::string ProgramAbsoluteFileName();

std::string ProgramBaseName();

std::string MyUserName();


inline void FlushLogs() {
  google::FlushLogFiles(google::INFO);
}

inline std::vector<std::string> GetLoggingDirectories() {
  return google::GetLoggingDirectories();
}

inline int SetVLogLevel(std::string_view module_pattern, int log_level) {
  return google::SetVLOGLevel(module_pattern.data(), log_level);
}

class ConsoleLogSink : public google::LogSink {
 public:
  virtual void send(google::LogSeverity severity, const char* full_filename,
                    const char* base_filename, int line, const struct ::tm* tm_time,
                    const char* message, size_t message_len) override;

  static ConsoleLogSink* instance();
};
#endif
extern const char* kProgramName;

}  // namespace base
