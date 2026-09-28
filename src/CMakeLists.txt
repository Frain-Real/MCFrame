# Собираем все исходники ядра
file(GLOB_RECURSE OS_CORE_SOURCES 
    "os/core/memory/*.cpp" 
    "os/core/task/*.cpp" 
    "os/core/primitives/*.cpp" 
    "os/interfaces/*.cpp"
)

# Собираем исходники выбранной платформы
file(GLOB OS_PLATFORM_SOURCES 
    "platforms/${MCU_TARGET}/*.cpp"
    "platforms/${MCU_TARGET}/*.c"
    "platforms/${MCU_TARGET}/*.s"
    "platforms/${MCU_TARGET}/*.S"
    )

# Создаем статическую библиотеку (на выходе получим libmyos.a)
add_library(MCFrame STATIC 
    ${OS_CORE_SOURCES} 
    ${OS_PLATFORM_SOURCES}
)

# Указываем компилятору, где искать заголовочные файлы.
# PUBLIC означает, что эти пути унаследуют все, кто подключит нашу библиотеку.
target_include_directories(MCFrame PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/os
    ${CMAKE_CURRENT_SOURCE_DIR}/interfaces
    ${CMAKE_CURRENT_SOURCE_DIR}/platforms/${MCU_TARGET}
)

