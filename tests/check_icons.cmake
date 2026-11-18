# ctest io-icons: every plug-in of the bundle has its icon, Contents/Resources/<identifier>.png (the file name that
# hosts look for, OFX packaging spec), except the plug-ins listed in NO_ICON.
#
#   cmake -DVERIFY=<io_verify_plugin_loads> -DBINARY=<.../IO.ofx> -DBUNDLE=<.../IO.ofx.bundle> -P check_icons.cmake

# plug-ins without an icon in this repository
set(NO_ICON net.sf.openfx.SeGrain net.sf.openfx.SeNoise)

execute_process(COMMAND "${VERIFY}" "${BINARY}" OUTPUT_VARIABLE _out ERROR_VARIABLE _err RESULT_VARIABLE _r)
if(_r)
  message(FATAL_ERROR "${VERIFY} ${BINARY} failed (${_r}):\n${_out}${_err}")
endif()
string(REGEX MATCHALL "plugin\\[[0-9]+\\] : [^ \n]+" _lines "${_out}")
if(NOT _lines)
  message(FATAL_ERROR "no plug-in found in ${BINARY}:\n${_out}${_err}")
endif()
set(_missing "")
foreach(_l IN LISTS _lines)
  string(REGEX REPLACE "^plugin\\[[0-9]+\\] : " "" _id "${_l}")
  list(FIND NO_ICON "${_id}" _i)
  if(_i EQUAL -1 AND NOT EXISTS "${BUNDLE}/Contents/Resources/${_id}.png")
    string(APPEND _missing " ${_id}")
  endif()
endforeach()
if(_missing)
  message(FATAL_ERROR "plug-ins without Contents/Resources/<identifier>.png in ${BUNDLE}:${_missing}")
endif()
list(LENGTH _lines _n)
string(JOIN " " _no_icon ${NO_ICON})
message(STATUS "${_n} plug-ins, all with an icon (except ${_no_icon})")
