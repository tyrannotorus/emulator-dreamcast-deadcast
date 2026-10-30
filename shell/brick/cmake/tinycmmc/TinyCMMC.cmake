# See tinycmmcConfig.cmake: just what tinygettext's CMakeLists.txt uses.
include(GNUInstallDirs)
set(TINYCMMC_WARNINGS_CXX_FLAGS "")
# the namespaced alias Flycast links to; the export and install rules are left out
function(tinycmmc_export_and_install_library target)
	add_library(${target}::${target} ALIAS ${target})
endfunction()
