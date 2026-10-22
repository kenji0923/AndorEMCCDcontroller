# Locate CFITSIO and provide the imported target CFITSIO::CFITSIO.
#
# Linux: pkg-config (dnf cfitsio-devel). Windows: vcpkg "cfitsio" or a manual
# install pointed to by CFITSIO_ROOT.

find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(PC_CFITSIO QUIET cfitsio)
endif()

find_path(CFITSIO_INCLUDE_DIR fitsio.h
    HINTS ${PC_CFITSIO_INCLUDE_DIRS} ${CFITSIO_ROOT}/include
    PATH_SUFFIXES cfitsio)
find_library(CFITSIO_LIBRARY NAMES cfitsio libcfitsio
    HINTS ${PC_CFITSIO_LIBRARY_DIRS} ${CFITSIO_ROOT}/lib)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(CFITSIO
    REQUIRED_VARS CFITSIO_LIBRARY CFITSIO_INCLUDE_DIR)

if(CFITSIO_FOUND AND NOT TARGET CFITSIO::CFITSIO)
    add_library(CFITSIO::CFITSIO UNKNOWN IMPORTED)
    set_target_properties(CFITSIO::CFITSIO PROPERTIES
        IMPORTED_LOCATION "${CFITSIO_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${CFITSIO_INCLUDE_DIR}")
endif()

mark_as_advanced(CFITSIO_INCLUDE_DIR CFITSIO_LIBRARY)
