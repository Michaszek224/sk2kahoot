cmake_minimum_required(VERSION 3.10)

# Ustaw nazwę projektu
project(KahootServer)

# Dodaj plik źródłowy do programu
add_executable(kahoot server.cpp)

# Dodaj flagi kompilatora, jeśli są potrzebne (np. -Wall -Wextra dla ostrzeżeń)
target_compile_options(kahoot PRIVATE -Wall -Wextra)

# Wymuś użycie standardu C++11 lub wyższego, jeśli potrzebny
set_target_properties(kahoot PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED YES)
