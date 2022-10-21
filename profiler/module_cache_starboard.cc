#include "base/profiler/module_cache.h"

#include "build/build_config.h"

namespace base {

 std::unique_ptr<const ModuleCache::Module> ModuleCache::CreateModuleForAddress(
    uintptr_t address) {
        return nullptr;
}   
}