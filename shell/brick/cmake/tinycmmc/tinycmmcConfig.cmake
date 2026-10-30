# Stands in for tinygettext's nested submodule external/tinycmmc (CMake helpers only), which this
# build does not fetch. tinygettext takes warning flags and install/export rules from it, and
# neither matters to a library built inside Flycast's tree.
set(TINYCMMC_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}")
set(tinycmmc_FOUND TRUE)
