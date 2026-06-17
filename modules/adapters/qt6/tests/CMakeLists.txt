find_package(Qt6 REQUIRED COMPONENTS Gui Widgets)

set(CMAKE_AUTOMOC ON)

add_executable(test_qt6
    test_main.cpp
    test_qt_adapter.cpp
    test_list_model.cpp
    test_qt_conformance.cpp
)
target_link_libraries(test_qt6 PRIVATE
    # aria::qt6 transitively brings in binding/core/... so we only name
    # the outermost targets we actually use directly.
    aria::qt6
    Qt6::Widgets
    doctest::doctest
    aria::warnings
)
add_test(NAME qt6_tests COMMAND test_qt6)
set_tests_properties(qt6_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${QT6_INSTALL_PREFIX}/${QT6_INSTALL_PLUGINS}"
)
