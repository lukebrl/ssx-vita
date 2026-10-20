# Run before each vitaGL build: vitaGL's objects live in lib/vitagl, shared by
# every build directory (debug and release), so objects built with other flags
# are removed and the whole library rebuilt.
set(stamp ${VITAGL_DIR}/vitagl_flags.stamp)
set(old_flags "")
if (EXISTS ${stamp})
  file(READ ${stamp} old_flags)
endif()
if (NOT old_flags STREQUAL VITAGL_MAKE_FLAGS)
  message(STATUS "vitaGL flags changed, rebuilding it: ${VITAGL_MAKE_FLAGS}")
  file(GLOB_RECURSE stale_objs ${VITAGL_DIR}/source/*.o)
  file(REMOVE ${stale_objs} ${VITAGL_DIR}/libvitaGL.a)
  file(WRITE ${stamp} "${VITAGL_MAKE_FLAGS}")
endif()
