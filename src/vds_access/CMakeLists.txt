add_library(remoteview_vds_access
    vds_reader.cpp
    vds_metadata.cpp
)

target_link_libraries(remoteview_vds_access
    spdlog::spdlog
    remoteview_config
)

# Link HueSpace library
target_link_libraries(remoteview_vds_access ${HUESPACE_LIBRARY})

target_include_directories(remoteview_vds_access PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${OPENVDS_INCLUDE_DIR}
)