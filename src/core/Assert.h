#pragma once

#include <cstdlib>
#include <string_view>

//
// Assertion and fatal error policy — A12
//
// PT_ASSERT(cond)           — debug-only; aborts with message if cond is false.
// PT_VERIFY(cond)           — release+debug; aborts with message if cond is false.
// PT_FATAL(msg)             — unconditional abort; always writes log + crash artifact.
//
// All three write to the logger (if available) and call the platform crash handler
// before terminating, so crash artifacts are produced.
//

// Forward-declare to avoid pulling the whole logger in every TU.
namespace vkpt::assert_detail {
  void handle_failure(std::string_view condition,
                      std::string_view message,
                      std::string_view file,
                      int line,
                      std::string_view function) noexcept;
}  // namespace vkpt::assert_detail

// ---- PT_FATAL ---------------------------------------------------------------

#define PT_FATAL(msg)                                                    \
  do {                                                                   \
    ::vkpt::assert_detail::handle_failure(                               \
        "FATAL", (msg), __FILE__, __LINE__, __func__);                   \
    ::std::abort();                                                      \
  } while (false)

// ---- PT_VERIFY (always enabled) --------------------------------------------

#define PT_VERIFY(cond)                                                  \
  do {                                                                   \
    if (!(cond)) {                                                        \
      ::vkpt::assert_detail::handle_failure(                             \
          #cond, "PT_VERIFY failed", __FILE__, __LINE__, __func__);      \
      ::std::abort();                                                     \
    }                                                                    \
  } while (false)

// ---- PT_ASSERT (debug-only) -------------------------------------------------

#if defined(NDEBUG)
  #define PT_ASSERT(cond) ((void)(cond))
#else
  #define PT_ASSERT(cond)                                                \
    do {                                                                 \
      if (!(cond)) {                                                     \
        ::vkpt::assert_detail::handle_failure(                           \
            #cond, "PT_ASSERT failed", __FILE__, __LINE__, __func__);    \
        ::std::abort();                                                  \
      }                                                                  \
    } while (false)
#endif
