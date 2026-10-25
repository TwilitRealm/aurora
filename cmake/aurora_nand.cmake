add_library(aurora_nand STATIC
    lib/revolution/nand.cpp
)
add_library(aurora::nand ALIAS aurora_nand)
set_target_properties(aurora_nand PROPERTIES FOLDER "aurora")

target_link_libraries(aurora_nand PUBLIC aurora::core)
target_include_directories(aurora_nand PRIVATE include)
