# --- Vendored dwc2 EP0-SETUP-DMA crash fix (idempotent, auto-applied) ---------
# esp_tinyusb is a managed component (re-downloaded pristine on each fetch), so the
# ESP32-S3 EP0-SETUP DMA crash fix in dcd_dwc2.c must be re-applied here. Safe to
# run every configure -- it no-ops if already patched. See patches/README.md.
idf_build_get_property(_hawk_python PYTHON)
execute_process(
    COMMAND "${_hawk_python}" "${CMAKE_CURRENT_SOURCE_DIR}/../patches/apply_tinyusb_patch.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/.."
    RESULT_VARIABLE _hawk_patch_rc
    OUTPUT_VARIABLE _hawk_patch_out
    ERROR_VARIABLE  _hawk_patch_out)
message(STATUS "Hawk dwc2 patch: ${_hawk_patch_out}")
if(NOT _hawk_patch_rc EQUAL 0)
    message(WARNING "Hawk: dwc2 EP0 patch NOT applied (DMA mode would crash on the Xbox). See patches/README.md")
endif()

# Faithful Xbox Communicator: descriptors, the custom TinyUSB application class
# driver (both iso endpoints + vendor EP0), and the tone source.
set(hawk_srcs
    "hawk_main.c"
    "usb_descriptors.c"
    "hawk_class.c"
    "tone.c")
set(hawk_priv_requires "")

# Optional WiFi UDP debug logging (Kconfig: HAWK_WIFI_LOG, default off). Only
# compiled when enabled, so the project builds with no wifi_creds.h present.
if(CONFIG_HAWK_WIFI_LOG)
    list(APPEND hawk_srcs "wifi_log.c")
    list(APPEND hawk_priv_requires esp_wifi nvs_flash esp_netif esp_event esp_hw_support)
endif()

# Byte-faithful EP0 size 8 for the communicator (esp_tinyusb defaults to 64).
set(hawk_tusb_defs CFG_TUD_ENDPOINT0_SIZE=8)

idf_component_register(
    SRCS ${hawk_srcs}
    INCLUDE_DIRS "."
    PRIV_REQUIRES ${hawk_priv_requires}
)

# Inject the settings the TinyUSB library must share with our sources directly onto
# its target (esp_tinyusb passes its own tusb_config.h to the library privately).
idf_build_get_property(build_components BUILD_COMPONENTS)
if(tinyusb IN_LIST build_components)
    set(hawk_tusb_name tinyusb)               # local component
else()
    set(hawk_tusb_name espressif__tinyusb)    # managed component
endif()
idf_component_get_property(hawk_tusb_lib ${hawk_tusb_name} COMPONENT_LIB)
target_compile_definitions(${hawk_tusb_lib} PRIVATE ${hawk_tusb_defs})
