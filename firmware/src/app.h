#pragma once

#include "config.h"

// The configuration the radio currently runs with (implemented in main.cpp).
const Config& liveConfig();

// Makes cfg live and reconfigures the radio. cfg must pass config::validate().
bool applyConfig(const Config& cfg);
