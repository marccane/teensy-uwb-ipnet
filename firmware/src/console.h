#pragma once

class Print;

// Text console on the second USB serial port. The same commands are
// accepted over the host link (CMD messages).
namespace console {

void poll();

// Runs one command line (modified in place while tokenizing).
void exec(char* line, Print& out);

}  // namespace console
