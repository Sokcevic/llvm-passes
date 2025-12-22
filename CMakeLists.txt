cmake_minimum_required(VERSION 3.18)
project(llvm-passes)

add_subdirectory(passes/dead-code-elimination)
add_subdirectory(passes/memory-safety)
