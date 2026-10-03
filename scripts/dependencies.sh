#!/usr/bin/env bash
# Source this file from a release script. Git submodules follow the pinned parent.
# Keep pinned dependencies with older policy versions configurable on CMake 4.
export CMAKE_POLICY_VERSION_MINIMUM=3.5
QUICKJSPP_REF=01cdd3047ced48265b127790848a0ca88204f2c7
LIBCRON_REF=ee34810b11bd23c8be637345532f91059b68b2d7
YAML_CPP_REF=f7320141120f720aecc4c32be25586e7da9eb978
RAPIDJSON_REF=24b5e7a8b27f42fa16b96fc70aade9106cf7102f
TOML11_REF=be08ba2be2a964edcdb3d3e3ea8d100abc26f286
CURL_REF=68720b4837284335b2d63cb358f8f6ce65f5bc55

checkout_dependency() {
    local repository="$1" destination="$2" revision="$3"
    git init "$destination"
    git -C "$destination" remote add origin "$repository"
    git -C "$destination" fetch --depth=1 origin "$revision"
    git -C "$destination" checkout --detach FETCH_HEAD
    test "$(git -C "$destination" rev-parse HEAD)" = "$revision"
    git -C "$destination" submodule update --init --recursive --depth=1
    if [[ "$repository" == "https://github.com/jbeder/yaml-cpp" ]]; then
        # Backport yaml-cpp 7b469b4: GCC 15 requires an explicit <cstdint> include.
        git -C "$destination" apply <<'PATCH'
diff --git a/src/emitterutils.cpp b/src/emitterutils.cpp
--- a/src/emitterutils.cpp
+++ b/src/emitterutils.cpp
@@ -1,3 +1,4 @@
 #include <algorithm>
+#include <cstdint>
 #include <iomanip>
 #include <sstream>
PATCH
    fi
}
