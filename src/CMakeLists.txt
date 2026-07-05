file(GLOB_RECURSE hdrs "*.h")      #команда для работы с файлами
file(GLOB_RECURSE srcs "*.cpp")

add_library(${PROG_LIBRARY} STATIC ${srcs} ${hdrs} )