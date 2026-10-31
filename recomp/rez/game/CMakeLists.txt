set(_rez ${CMAKE_CURRENT_SOURCE_DIR}/../src)
dreamcomp_add_port(rez
  TITLE "Rez"
  OUTPUT_NAME rez
  SOURCES ${_rez}/rez.cpp ${_rez}/native_gfx.cpp ${_rez}/native_sys.cpp ${_rez}/native_snd.cpp
          ${_rez}/native_dev.cpp ${_rez}/native_gdfs.cpp ${_rez}/native_periph.cpp
          ${_rez}/native_sound.cpp ${_rez}/native_sound_driver.cpp ${_rez}/native_synth.cpp
          ${_rez}/native_irq.cpp ${_rez}/native_ninja.cpp)
target_include_directories(rez_boot PRIVATE ${_rez})
