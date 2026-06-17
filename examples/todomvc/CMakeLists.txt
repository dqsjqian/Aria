# Headless TodoMVC — derived collections + reactive selection demo.
#
# A full ViewModel with no GUI toolkit: ObservableList<Todo> as the source
# of truth, two FilteredList<Todo> views (active / completed) that re-filter
# live, and a Selection<Todo> that auto-clears when its item is removed.
# Doubles as a self-checking smoke test (todomvc_smoke).

add_executable(todomvc main.cpp)
target_link_libraries(todomvc PRIVATE aria::core aria::runtime aria::warnings)

if(ARIA_BUILD_TESTS)
    add_test(NAME todomvc_smoke COMMAND todomvc)
endif()
