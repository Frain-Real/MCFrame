# libFuzzer harnesses. Собираются только с MCFRAME_FUZZ=ON и Clang.
add_executable(fuzz_http_url fuzz_http_url.cpp)
target_link_libraries(fuzz_http_url PRIVATE MCFrame::MCFrame)
target_compile_options(fuzz_http_url PRIVATE -fsanitize=fuzzer,address)
target_link_options(fuzz_http_url PRIVATE -fsanitize=fuzzer,address)

add_executable(fuzz_ota_payload fuzz_ota_payload.cpp)
target_link_libraries(fuzz_ota_payload PRIVATE MCFrame::MCFrame)
target_compile_options(fuzz_ota_payload PRIVATE -fsanitize=fuzzer,address)
target_link_options(fuzz_ota_payload PRIVATE -fsanitize=fuzzer,address)
