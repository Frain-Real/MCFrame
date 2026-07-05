cmake_minimum_required(VERSION 3.16)

set(PROJECT_NAME MCFrame)
project(${PROJECT_NAME})

#настраиваем стандарты языка
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

#настраиваем места хранения скомпилированных файлов
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib) 
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE})

#даем названия подпроектам 
set(PROG_LIBRARY "${PROJECT_NAME}") 

#добавляем директории наших подпроектов в область видимости CMake 
add_subdirectory(src)
add_subdirectory(example)
