#include "common/Logger.h"

#include <plog/Appenders/ConsoleAppender.h>
#include <plog/Formatters/MessageOnlyFormatter.h>
#include <plog/Init.h>

#include "llvm/Support/CommandLine.h"

static llvm::cl::opt<std::string>
    LogLevelOpt("ckpt-log-level",
                llvm::cl::desc("Log level for checkpoint passes (error/warning/info/debug)"),
                llvm::cl::init("info"));

namespace checkpoint {

void initLogging() {
    static bool initialized = false;
    if (initialized)
        return;
    initialized = true;

    plog::Severity level = plog::info;
    std::string opt = LogLevelOpt.getValue();
    if (opt == "error")
        level = plog::error;
    else if (opt == "warning")
        level = plog::warning;
    else if (opt == "debug")
        level = plog::debug;

    static plog::ConsoleAppender<plog::MessageOnlyFormatter> appender(plog::streamStdErr);
    plog::init(level, &appender);
}

} // namespace checkpoint
