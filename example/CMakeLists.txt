set(target example)

file(GLOB hdrs "*.h")
file(GLOB srcs "*.cpp")

add_executable(${target} ${srcs} ${hdrs} )
target_link_libraries(${target} ${PROG_LIBRARY})