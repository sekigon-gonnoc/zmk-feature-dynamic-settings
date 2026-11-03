# SPDX-License-Identifier: MIT

if(NOT CONFIG_ZMK_DYNAMIC_SETTINGS)
  return()
endif()

zephyr_include_directories(include)
target_sources_ifdef(CONFIG_ZMK_DYNAMIC_SETTINGS app PRIVATE src/store.c)
target_sources_ifdef(CONFIG_ZMK_STUDIO_RPC app PRIVATE src/studio_rpc.c)

if(CONFIG_ZMK_DYNAMIC_SETTINGS_SCHEMA)
  set_property(GLOBAL PROPERTY DYNAMIC_SETTINGS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
  set_property(GLOBAL PROPERTY DYNAMIC_SETTINGS_BINARY
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
  function(dynamic_settings_generate_schema)
    get_property(dynamic_settings_root GLOBAL PROPERTY DYNAMIC_SETTINGS_ROOT)
    get_property(dynamic_settings_binary GLOBAL PROPERTY DYNAMIC_SETTINGS_BINARY)
    get_property(dynamic_settings_contributors GLOBAL PROPERTY DYNAMIC_SETTINGS_SCHEMA_CONTRIBUTORS)
    set(contributor_args)
    foreach(contributor IN LISTS dynamic_settings_contributors)
      list(APPEND contributor_args --contributor "${contributor}")
    endforeach()
    execute_process(
      COMMAND ${PYTHON_EXECUTABLE} "${dynamic_settings_root}/scripts/generate-settings-schema.py"
        --edt "${APPLICATION_BINARY_DIR}/zephyr/edt.pickle"
        --config "${APPLICATION_BINARY_DIR}/zephyr/.config"
        --zephyr "${ZEPHYR_BASE}"
        --out "${dynamic_settings_binary}"
        ${contributor_args}
      RESULT_VARIABLE result
    )
    if(NOT result EQUAL 0)
      message(FATAL_ERROR "Nemu-link settings schema generation failed")
    endif()
    target_sources(app PRIVATE "${dynamic_settings_binary}/settings_schema_generated.c")
    zephyr_include_directories("${dynamic_settings_binary}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
      "${dynamic_settings_root}/scripts/generate-settings-schema.py"
      ${dynamic_settings_contributors})
  endfunction()
  cmake_language(DEFER DIRECTORY "${APPLICATION_SOURCE_DIR}"
    CALL dynamic_settings_generate_schema)
endif()
