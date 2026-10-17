# Every source file shared by the plugin and the tests: all .cpp files under
# plugin/Source except the plugin entry point (PluginEntry.cpp), which only the
# plugin target compiles. New files are picked up automatically on the next build.
file(GLOB_RECURSE CONTEXT_SHARED_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_SOURCE_DIR}/plugin/Source/*.cpp")
list(FILTER CONTEXT_SHARED_SOURCES EXCLUDE REGEX ".*/PluginEntry\\.cpp$")
