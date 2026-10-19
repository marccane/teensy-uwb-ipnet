#pragma once

// Timestamped log lines on the console port. Never blocks: if the console is
// closed or its USB buffer is full the line is dropped.
void logPrintf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
