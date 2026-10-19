#include "log.h"

#include <Arduino.h>
#include <stdarg.h>

void logPrintf(const char* fmt, ...) {
  if (!SerialUSB1.dtr()) return;
  char line[200];
  const uint32_t ms = millis();
  int n = snprintf(line, sizeof(line), "[%5lu.%03lu] ", ms / 1000, ms % 1000);
  va_list ap;
  va_start(ap, fmt);
  n += vsnprintf(line + n, sizeof(line) - n - 2, fmt, ap);
  va_end(ap);
  if (n > (int)sizeof(line) - 3) n = sizeof(line) - 3;
  line[n++] = '\r';
  line[n++] = '\n';
  if (SerialUSB1.availableForWrite() < n) return;
  SerialUSB1.write(line, n);
}
