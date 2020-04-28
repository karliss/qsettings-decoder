set (CMAKE_CXX_STANDARD 11)

set(SOURCE_FILES main.cpp)

add_executable(qsettings-decoder ${SOURCE_FILES})

find_package(Qt5 REQUIRED COMPONENTS Core Gui) # Gui required for reading types like color
target_link_libraries(qsettings-decoder PRIVATE Qt5::Core Qt5::Gui)