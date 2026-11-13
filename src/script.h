// script.h - a little script that drives PicoAmp (the tests, the CI's run on
// Windows): commands separated by ';' or new lines
//   wait <ms> | key <char or name> | type <text> | click <x> <y> [right] |
//   drop <path> | play <url> | shot <file.png> [scale] | status | list | page | where | quit
#pragma once
#include <stdbool.h>
#include <stdio.h>

void script_start(const char *text, FILE *out);
bool script_step(void);         // once a frame; false when it has ended
