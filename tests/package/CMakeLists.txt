# A consumer of the installed package, standing in for the host this port exists to serve. It is
# configured from scratch against an install tree -- no vcpkg toolchain, no dependencies, nothing
# but find_package -- because that is precisely the claim the package makes.
cmake_minimum_required(VERSION 3.24)

project(TabulaSonoraConsumer LANGUAGES CXX)

find_package(TabulaSonora REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE ts::tabulasonora)
