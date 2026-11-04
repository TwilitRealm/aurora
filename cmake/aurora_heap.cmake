add_library(aurora_heap STATIC lib/heap.cpp)
add_library(aurora::heap ALIAS aurora_heap)
set_target_properties(aurora_heap PROPERTIES FOLDER "aurora")

target_include_directories(aurora_heap PUBLIC include)
target_compile_definitions(aurora_heap PUBLIC AURORA_ENABLE_HEAP=1)
target_link_libraries(aurora_heap PRIVATE aurora::core mimalloc-static)
