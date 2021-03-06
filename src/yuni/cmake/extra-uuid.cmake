
ynmessage(MODULE "UUID")

LIBYUNI_CONFIG_LIB("both" "uuid" "yuni-static-uuid")
LIBYUNI_CONFIG_DEPENDENCY("uuid" "core") # core is required
set(__yuni_libuuid OFF)

if (APPLE)
	LIBYUNI_CONFIG_FRAMEWORK("both" "uuid" CoreFoundation)
else()
	if (WIN32 OR WIN64)
		# -lole32
		LIBYUNI_CONFIG_LIB("both" "uuid" "ole32")
		# -lrpcrt4
		LIBYUNI_CONFIG_LIB("both" "uuid" "rpcrt4")
	else()
		# -luuid
		LIBYUNI_CONFIG_LIB("both" "uuid" "uuid")
		set(__yuni_libuuid ON)
	endif()
endif()


if (${__yuni_libuuid})
	#FIXME
	find_path(UUID_INCLUDE_DIR NAMES uuid/uuid.h PATHS /usr/include /usr/local/include /opt/local/include /sw/include)
	if(NOT UUID_INCLUDE_DIR)
		set(YUNI_CMAKE_ERROR 1)
		ynmessage("[!!] Impossible to find uuid/uuid.h.")
		ynmessage(" * Packages needed on Debian: libuuid1, uuid-dev")
		ynmessage(" * Packages needed on redhat: libuuid-devel")
	endif()
endif()


add_library(yuni-static-uuid STATIC
	uuid/uuid.h
	uuid/uuid.hxx
	uuid/uuid.cpp
	uuid/fwd.h
	yuni.h
)

# Setting output path
SET_TARGET_PROPERTIES(yuni-static-uuid PROPERTIES
		ARCHIVE_OUTPUT_DIRECTORY "${YUNI_OUTPUT_DIRECTORY}/lib")

# Installation
install(TARGETS yuni-static-uuid
	COMPONENT ${YUNICOMPONENT_UUID}
	ARCHIVE DESTINATION lib/${YUNI_VERSIONED_INST_PATH})

# Install UUID-related headers
install(
	DIRECTORY uuid
	COMPONENT ${YUNICOMPONENT_UUID}
	DESTINATION include/${YUNI_VERSIONED_INST_PATH}
	FILES_MATCHING
		PATTERN "*.h"
		PATTERN "*.hxx"
	PATTERN ".svn" EXCLUDE
	PATTERN "CMakeFiles" EXCLUDE
	PATTERN "cmake" EXCLUDE
)


