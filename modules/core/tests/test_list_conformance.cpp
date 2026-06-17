// ============================================================================
//  test_list_conformance.cpp
// ----------------------------------------------------------------------------
//  Plug ObservableList<T> into the framework-level list conformance suite
//  (docs/list-diff-contract.md D-N) and prove the canonical implementation
//  satisfies every generic contract.
//
//  Any newly added derived list (or test fake) is expected to add an
//  analogous TEST_CASE that calls `run_list_source_conformance`.
// ============================================================================

#include <doctest/doctest.h>

#include "aria/observable_list.hpp"
#include "aria/testing/list_conformance.hpp"

#include <memory>

TEST_CASE("ObservableList<int> satisfies the generic ListSource D-N contract") {
    aria::testing::run_list_source_conformance<aria::ObservableList<int>>(
        [] { return std::make_shared<aria::ObservableList<int>>(); });
}

TEST_CASE("ObservableList<std::string> satisfies the generic ListSource D-N contract") {
    aria::testing::run_list_source_conformance<aria::ObservableList<std::string>>(
        [] { return std::make_shared<aria::ObservableList<std::string>>(); });
}
