cmake_minimum_required(VERSION 3.25)
project(cerf_icon_tests LANGUAGES CXX)
enable_testing()
find_package(Qt6 REQUIRED COMPONENTS Gui)
get_filename_component(repo ../.. ABSOLUTE)
add_executable(cerf_icon_test icon_test.cpp
    "${repo}/cerf-qt/qt_resource_paths.cpp"
    "${repo}/cerf-qt/svg_document.cpp"
    "${repo}/cerf-qt/tinted_symbol_icon.cpp")
target_compile_features(cerf_icon_test PRIVATE cxx_std_20)
target_compile_options(cerf_icon_test PRIVATE -Wall -Wextra -Werror)
target_include_directories(cerf_icon_test PRIVATE "${repo}/cerf-qt" "${CERFQT_RESVG_PREFIX}/include")
target_link_libraries(cerf_icon_test PRIVATE Qt6::Gui "${CERFQT_RESVG_PREFIX}/lib/libresvg.a" dl pthread m)
set_source_files_properties("${repo}/external/material_symbols/home.svg" PROPERTIES QT_RESOURCE_ALIAS home.svg)
qt_add_resources(cerf_icon_test test_icons PREFIX /icons FILES
    "${repo}/external/material_symbols/home.svg" invalid.svg)
add_test(NAME cerf_icons COMMAND cerf_icon_test)

# Optional integration check using the style library from an existing CERF build.
if(CERFQT_STYLE_BUILD)
    find_package(Qt6 REQUIRED COMPONENTS Widgets Quick QuickControls2)
    qt_add_executable(cerf_file_dialog_test file_dialog_test.cpp
        "${repo}/cerf-qt/cerfman/svg_icon_provider.cpp"
        "${repo}/cerf-qt/qt_resource_paths.cpp"
        "${repo}/cerf-qt/svg_document.cpp"
        "${repo}/cerf-qt/tinted_symbol_icon.cpp")
    target_compile_features(cerf_file_dialog_test PRIVATE cxx_std_20)
    target_include_directories(cerf_file_dialog_test PRIVATE "${repo}/cerf-qt" "${CERFQT_RESVG_PREFIX}/include")
    target_link_libraries(cerf_file_dialog_test PRIVATE Qt6::Widgets Qt6::Quick Qt6::QuickControls2
        "-Wl,--whole-archive" "${CERFQT_STYLE_BUILD}/libcerfqt_styleplugin.a"
        "${CERFQT_STYLE_BUILD}/libcerfqt_style.a" "-Wl,--no-whole-archive"
        "${CERFQT_RESVG_PREFIX}/lib/libresvg.a" dl pthread m)
    add_test(NAME cerf_file_dialog COMMAND cerf_file_dialog_test)
    set_tests_properties(cerf_file_dialog PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
endif()
