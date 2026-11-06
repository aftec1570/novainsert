// Anchor translation unit of novainsert_host (static library must have at least one object). Real sources join from Task 3.
#include "engine/version.hpp"
namespace ni::host { const char* engineVersion() noexcept { return ni::engine::kVersion.data(); } }
