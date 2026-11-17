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
  /* 【2026-11-17 修复，影响稳定性】
   * 原写法把 IsAllowed_real 定义成**局部**变量且未初始化：
   *     bool (*IsAllowed_real)(...);      // 栈上随机值
   *     if (!IsAllowed_real) ...dlsym...  // 随机值非 0 就跳过 dlsym
   *     if (IsAllowed_real) return IsAllowed_real(...);  // 把栈垃圾当函数指针调用
   * 一旦栈上残留非 0 值，就会跳到野地址 → zygote 里 SIGSEGV。
   * （编译器已用 -Wuninitialized 报警，本次全编暴露。）
   *
   * 同时原实现每次调用都 dlsym 一次，而 fd 白名单检查在 zygote 启动阶段很频繁。
   * 改成函数内 static：C++11 保证只初始化一次且线程安全，失败也为 nullptr。 */
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
