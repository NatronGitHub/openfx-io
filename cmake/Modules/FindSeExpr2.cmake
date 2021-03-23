include(GNUInstallDirs)

find_library(
    SEEXPR2_LIBRARY
    NAMES SeExpr)

find_path(SEEXPR2_INCLUDE_DIR
  NAMES SeExpression.h
  HINTS ${CMAKE_INSTALL_INCLUDEDIR} /usr/local/include)

include(FindPackageHandleStandardArgs)

find_package_handle_standard_args(SeExpr2 DEFAULT_MSG
                                  SEEXPR2_LIBRARY
                                  SEEXPR2_INCLUDE_DIR)

mark_as_advanced(SEEXPR2_LIBRARY SEEXPR2_INCLUDE_DIR)

if(SEEXPR2_FOUND AND NOT TARGET SeExpr2::SeExpr)
  add_library(SeExpr2::SeExpr SHARED IMPORTED)
  set_target_properties(
    SeExpr2::SeExpr
    PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${SEEXPR2_INCLUDE_DIR}"
      IMPORTED_LOCATION ${SEEXPR2_LIBRARY})
endif()
