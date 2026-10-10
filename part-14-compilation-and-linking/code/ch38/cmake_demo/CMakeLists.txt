cmake_minimum_required(VERSION 3.28)
project(modules_demo LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(shapes)
target_sources(shapes
  PUBLIC
    FILE_SET modules TYPE CXX_MODULES FILES
      shapes.cppm            # export module shapes;
      shapes_geometry.cppm   # export module shapes:geometry;   (partition interface)
  PRIVATE
    shapes_impl.cpp          # module shapes;                   (implementation unit)
)

add_executable(app main.cpp)
target_link_libraries(app PRIVATE shapes)
