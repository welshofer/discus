cmake_minimum_required(VERSION 3.16)

# Builds install the exact versions in dependencies.lock. Skip the component
# manager's "newer versions available" check: it re-solves from scratch, and
# that solve fails on LVGL 9.6's Kconfig-guarded dependencies
# (MissingKconfigError: LV_USE_FREETYPE). Delete this once upstream fixes it.
set(ENV{IDF_COMPONENT_CHECK_NEW_VERSION} 0)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(discus)
