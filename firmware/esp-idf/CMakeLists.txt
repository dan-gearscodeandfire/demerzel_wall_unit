cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(demerzel_wall_unit)

# esp-14.2.0 gcc flags a maybe-uninitialized in micro-opus's vendored
# silk/NLSF2A.c (false positive on cos_LSF_QA). Relax the warning for
# that target only — don't touch the global flags.
idf_component_get_property(_opus_lib esphome__micro-opus COMPONENT_LIB)
if(_opus_lib)
    target_compile_options(${_opus_lib} PRIVATE -Wno-error=maybe-uninitialized)
endif()
