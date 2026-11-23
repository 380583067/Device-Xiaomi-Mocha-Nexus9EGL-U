#include <stdlib.h>
#include <dlfcn.h>

#include <cutils/log.h>

#include "fd_utils.h"


static const char* kPathWhitelistExtra[] = {
#ifdef PATH_WHITELIST_EXTRA
PATH_WHITELIST_EXTRA
#endif
};

bool FileDescriptorAllowlist::IsAllowed(const std::string& path) const {
  /* [2026-11-17 fix, stability] The old code declared IsAllowed_real as a
   * **local** variable without initialising it:
   *     bool (*IsAllowed_real)(...);      // garbage from the stack
   *     if (!IsAllowed_real) ...dlsym...  // non-zero garbage skips the dlsym
   *     if (IsAllowed_real) return IsAllowed_real(...);  // calls stack garbage
   * Any non-zero leftover on the stack means a jump to a wild address, i.e. SIGSEGV
   * in zygote (the compiler already warned with -Wuninitialized; a full build exposed
   * it).
   * The old code also called dlsym on every call, and the fd whitelist check is very
   * frequent during zygote start-up. It is a function-local static now: C++11
   * guarantees one thread-safe initialisation, and a failure stays nullptr. */
  static bool (*const IsAllowed_real)(const FileDescriptorAllowlist*,
                                      const std::string&) =
      (bool (*)(const FileDescriptorAllowlist*, const std::string&))dlsym(
          RTLD_NEXT,
          "_ZNK23FileDescriptorAllowlist9IsAllowedERKNSt3__112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEE");

  // Check the static whitelist path.
  for (const auto& whitelist_path : kPathWhitelistExtra) {
    if (path == whitelist_path)
      return true;
  }

  if (IsAllowed_real)
    return IsAllowed_real(this, path);

  ALOGE("Could not find IsAllowed, aborting");
  abort();

  return false;
}
