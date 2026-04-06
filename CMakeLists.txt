cmake_minimum_required(VERSION 3.10)
project(os_course_project)

include_directories(include)

# Тест для первого алгоритма (Двойники)
add_executable(test_buddy tests/main_test.c src/allocator_buddy.c)
target_compile_definitions(test_buddy PRIVATE USE_BUDDY)

# Тест для второго алгоритма (McKusick-Karels)
add_executable(test_mk tests/main_test.c src/allocator_mk.c)
target_compile_definitions(test_mk PRIVATE USE_MK)