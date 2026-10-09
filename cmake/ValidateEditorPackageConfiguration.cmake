string(TOLOWER "${GE_CONFIG}" GE_CONFIG_LOWER)
if(GE_DISTRIBUTION_BUILD AND NOT GE_CONFIG_LOWER STREQUAL "release")
    message(FATAL_ERROR "A distribution package must be built in Release configuration")
endif()
