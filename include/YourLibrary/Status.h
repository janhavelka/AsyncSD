/**
 * @file Status.h
 * @brief Error handling types for YourLib.
 *
 * Provides a lightweight, zero-allocation error model. All error messages
 * are static string literals.
 */

#pragma once

#include <stdint.h>

namespace YourLibrary {

/**
 * @brief Error code enumeration.
 *
 * Covers common embedded error scenarios. When wrapping third-party libraries,
 * translate their errors to the most appropriate Err code and store the
 * original error in Status::detail.
 */
enum class Err : uint16_t {
  OK = 0,              ///< Success, no error
  INVALID_CONFIG,      ///< Invalid argument or configuration parameter
  TIMEOUT,             ///< Operation timed out waiting for response
  RESOURCE_BUSY,       ///< Resource is busy, cannot acquire lock
  COMM_FAILURE,        ///< Communication or I/O operation failed
  NOT_INITIALIZED,     ///< Library not initialized or begin() not called
  OUT_OF_MEMORY,       ///< Memory allocation failed
  HARDWARE_FAULT,      ///< Hardware peripheral returned error
  EXTERNAL_LIB_ERROR,  ///< Error from external library (see detail field)
  INTERNAL_ERROR       ///< Internal logic error (bug in library code)
};

/**
 * @brief Operation result with error details.
 *
 * Returned by fallible operations. Check with ok() or inspect code/msg.
 *
 * @note The msg field MUST point to a static string literal. Never assign
 *       dynamically allocated strings. This ensures zero heap allocation
 *       in error paths and safe usage across function boundaries.
 *
 * Example:
 * @code
 * Status st = lib.begin(config);
 * if (!st.ok()) {
 *   Serial.printf("Error: %s (code=%d, detail=%ld)\n",
 *                 st.msg, (int)st.code, (long)st.detail);
 * }
 * @endcode
 */
struct Status {
  Err code = Err::OK;       ///< Error category
  int32_t detail = 0;       ///< Vendor/library-specific error code (optional)
  const char* msg = "";     ///< Human-readable message (STATIC STRING ONLY)

  /**
   * @brief Default constructor - creates OK status.
   */
  constexpr Status() : code(Err::OK), detail(0), msg("") {}

  /**
   * @brief Constructor with all fields.
   */
  constexpr Status(Err c, int32_t d, const char* m) : code(c), detail(d), msg(m) {}

  /**
   * @brief Check if operation succeeded.
   * @return true if code == Err::OK
   */
  constexpr bool ok() const { return code == Err::OK; }
};

/**
 * @brief Create a success Status.
 * @return Status with Err::OK
 */
constexpr Status Ok() { return Status(Err::OK, 0, ""); }

}  // namespace YourLibrary
