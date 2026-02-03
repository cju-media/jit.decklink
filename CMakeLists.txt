cmake_minimum_required(VERSION 3.19)
project(jit.decklink)

# Set path to min-api
set(MIN_API_DIR ${CMAKE_CURRENT_SOURCE_DIR}/min-api)

# Pre-target setup
set(C74_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/externals")
include(${MIN_API_DIR}/script/min-pretarget.cmake)

# Source files
set(SOURCE_FILES
    source/jit.decklink.cpp
)

# Check for DeckLink SDK Dispatch file (common on Mac/Linux)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/include/DeckLinkAPIDispatch.cpp")
    list(APPEND SOURCE_FILES include/DeckLinkAPIDispatch.cpp)
endif()

# Create the Max external (module)
add_library(${PROJECT_NAME} MODULE ${SOURCE_FILES})

# Include directories
target_include_directories(${PROJECT_NAME} PRIVATE
    include
    ${MIN_API_DIR}/include
    ${C74_INCLUDES}
)

# Post-target setup
include(${MIN_API_DIR}/script/min-posttarget.cmake)
