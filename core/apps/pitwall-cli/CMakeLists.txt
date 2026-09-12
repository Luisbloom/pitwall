add_executable(pitwall-cli main.cpp)
target_link_libraries(pitwall-cli PRIVATE pw_core pw_obs pw_config)
pw_target_warnings(pitwall-cli)
set_target_properties(pitwall-cli PROPERTIES OUTPUT_NAME "pitwall-cli")
