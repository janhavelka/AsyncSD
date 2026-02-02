/**
 * @file BuildConfig.h
 * @brief Build-time configuration for examples.
 *
 * Define LOG_LEVEL via build_flags in platformio.ini, or accept defaults here.
 */

#pragma once

/// @brief Log level: 0=off, 1=error, 2=info, 3=debug, 4=trace
#ifndef LOG_LEVEL
#define LOG_LEVEL 2
#endif
