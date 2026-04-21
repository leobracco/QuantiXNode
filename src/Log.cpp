#include "Log.h"

namespace quantix {
namespace log {

namespace {
Level g_level = Level::INFO;

const char* levelTag(Level lvl)
{
    switch (lvl) {
        case Level::ERROR: return "E";
        case Level::WARN:  return "W";
        case Level::INFO:  return "I";
        case Level::DEBUG: return "D";
    }
    return "?";
}
}  // namespace

void init(Level level)
{
    g_level = level;
}

void setLevel(Level level)
{
    g_level = level;
}

Level getLevel()
{
    return g_level;
}

void logf(Level level, const char* tag, const char* fmt, ...)
{
    if ((int)level > (int)g_level)
        return;

    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;

    Serial.printf("[%lu][%s][%s] %s\n",
                  (unsigned long)millis(),
                  levelTag(level),
                  tag ? tag : "",
                  buf);
}

}  // namespace log
}  // namespace quantix
