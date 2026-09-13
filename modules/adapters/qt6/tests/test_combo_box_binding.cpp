#include <doctest/doctest.h>

#include "aria/adapters/qt6/qt_combo_box_binding.hpp"
#include "aria/adapters/qt6/qt_list_model_adapter.hpp"
#include "aria/derived/filtered_list.hpp"

#include <QApplication>
#include <QKeyEvent>
#include <QString>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

using namespace aria;
using namespace aria::adapters::qt6;

namespace {

int key_conversion_failures = 0;

void record_key_conversion_failure(const CallbackFailure& failure) {
    if (failure.category == "qt.combo_box_selection" && failure.exception) {
        ++key_conversion_failures;
    }
}

constexpr int id_role = Qt::UserRole + 1;

struct Choice {
    std::string id;
    std::string label;
    bool visible = true;
};

std::shared_ptr<Choice> choice(std::string id, std::string label = "Same",
                               bool visible = true) {
    return std::make_shared<Choice>(Choice{std::move(id), std::move(label), visible});
}

QVariant choice_data(const Choice& item, int role) {
    if (role == id_role) return QString::fromStdString(item.id);
    if (role == Qt::DisplayRole) return QString::fromStdString(item.label);
    return {};
}

std::string string_key(const QVariant& data) {
    return data.toString().toStdString();
}

void press_key(QComboBox& combo, Qt::Key key) {
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
    QApplication::sendEvent(&combo, &press);
    QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
    QApplication::sendEvent(&combo, &release);
}

struct ComboFixture {
    ObservableList<Choice> options;
    Property<std::optional<std::string>> selected{std::nullopt};
    ObservableListModel<Choice> model{options, {{id_role, "id"}}, choice_data};
    QComboBox combo;
    Subscription binding;

    ComboFixture() { combo.setModel(&model); }

    void bind(std::optional<std::string> initial) {
        selected.set(std::move(initial));
        binding = bind_combo_box_selection(selected, combo, string_key, id_role);
    }
};

}  // namespace

TEST_CASE("qt combo: stable IDs bind both directions despite duplicate labels") {
    ComboFixture f;
    f.options.push_back(choice("a"));
    f.options.push_back(choice("b"));
    f.selected.set("b");
    int changes = 0;
    auto changed = f.selected.on_changed([&](const auto&) { ++changes; });

    REQUIRE(f.combo.currentIndex() == 0); // Qt's default before binding.
    f.bind("b");
    CHECK(f.combo.currentIndex() == 1);
    CHECK(changes == 0);

    // Real key events exercise QComboBox::activated, not a synthetic
    // currentIndexChanged signal or a direct call to the binding.
    press_key(f.combo, Qt::Key_Up);
    CHECK(f.selected.peek() == std::optional<std::string>{"a"});
    CHECK(changes == 1);

    f.combo.setCurrentIndex(1);
    CHECK(f.selected.peek() == std::optional<std::string>{"a"});
    CHECK(changes == 1); // Programmatic widget changes do not write back.

    f.selected.set("b");
    CHECK(f.combo.currentIndex() == 1);
    CHECK(changes == 2);
    f.selected.set(std::nullopt);
    CHECK(f.combo.currentIndex() == -1);
    CHECK(changes == 3);
}

TEST_CASE("qt combo: row changes and same-ID replacement preserve selection") {
    ComboFixture f;
    f.options.push_back(choice("a"));
    f.options.push_back(choice("b"));
    f.options.push_back(choice("c"));
    f.bind("b");
    int changes = 0;
    auto changed = f.selected.on_changed([&](const auto&) { ++changes; });

    f.options.insert(0, choice("prefix"));
    CHECK(f.combo.currentIndex() == 2);
    f.options.remove_at(0);
    CHECK(f.combo.currentIndex() == 1);
    f.options.move(1, 2);
    CHECK(f.combo.currentIndex() == 2);
    f.options.replace_at(0, choice("other"));
    CHECK(f.combo.currentIndex() == 2);
    f.options.replace_at(2, choice("b", "Renamed"));
    CHECK(f.combo.currentText() == "Renamed");
    f.model.reload(); // A reset that retains the ID also retains selection.
    CHECK(f.combo.currentIndex() == 2);
    CHECK(f.selected.peek() == std::optional<std::string>{"b"});
    CHECK(changes == 0);
}

TEST_CASE("qt combo: a removed selected ID clears once without choosing a neighbor") {
    ComboFixture f;
    f.options.push_back(choice("a"));
    f.options.push_back(choice("b"));
    f.options.push_back(choice("c"));
    f.bind("b");
    int changes = 0;
    auto changed = f.selected.on_changed([&](const auto&) { ++changes; });

    SUBCASE("remove") { f.options.remove_at(1); }
    SUBCASE("replace with another ID") { f.options.replace_at(1, choice("new")); }
    SUBCASE("reset to empty") { f.options.clear(); }

    CHECK_FALSE(f.selected.peek().has_value());
    CHECK(f.combo.currentIndex() == -1);
    CHECK(changes == 1);
}

TEST_CASE("qt combo: pending IDs survive unrelated options until they arrive") {
    ComboFixture f;
    f.bind("later");
    int changes = 0;
    auto changed = f.selected.on_changed([&](const auto&) { ++changes; });
    CHECK(f.combo.currentIndex() == -1);

    f.options.push_back(choice("other"));
    CHECK(f.combo.currentIndex() == -1);
    f.options.clear();
    CHECK(f.selected.peek() == std::optional<std::string>{"later"});
    f.options.push_back(choice("later"));
    CHECK(f.combo.currentIndex() == 0);
    CHECK(changes == 0);

    f.options.push_back(choice("other"));
    f.options.remove_at(0);
    CHECK_FALSE(f.selected.peek().has_value());
    CHECK(f.combo.currentIndex() == -1);
    CHECK(changes == 1);
}

TEST_CASE("qt combo: null selection survives Qt auto-selection of the first option") {
    ComboFixture f;
    f.bind(std::nullopt);
    int changes = 0;
    auto changed = f.selected.on_changed([&](const auto&) { ++changes; });

    f.options.push_back(choice("a"));
    CHECK(f.combo.currentIndex() == -1);
    f.options.insert(0, choice("b"));
    CHECK(f.combo.currentIndex() == -1);
    f.model.reload();
    CHECK(f.combo.currentIndex() == -1);
    CHECK_FALSE(f.selected.peek().has_value());
    CHECK(changes == 0);

    press_key(f.combo, Qt::Key_Down);
    CHECK(f.selected.peek() == std::optional<std::string>{"b"});
    CHECK(changes == 1);
}

TEST_CASE("qt combo: a filtered options model follows selected ID membership") {
    auto options = std::make_shared<ObservableList<Choice>>();
    options->push_back(choice("a"));
    options->push_back(choice("b"));
    FilteredList<Choice> visible{options, [](const Choice& item) { return item.visible; }};
    Property<std::optional<std::string>> selected{"b"};
    ObservableListModel<Choice> model{visible, {{id_role, "id"}}, choice_data};
    QComboBox combo;
    combo.setModel(&model);
    auto binding = bind_combo_box_selection(selected, combo, string_key, id_role);
    int changes = 0;
    auto changed = selected.on_changed([&](const auto&) { ++changes; });

    options->insert(0, choice("hidden", "Hidden", false));
    CHECK(combo.count() == 2);
    CHECK(combo.currentIndex() == 1);
    options->replace_at(1, choice("a", "Same", false));
    CHECK(combo.currentIndex() == 0);
    CHECK(changes == 0);
    options->replace_at(2, choice("b", "Same", false));
    CHECK_FALSE(selected.peek().has_value());
    CHECK(combo.currentIndex() == -1);
    CHECK(changes == 1);
}

TEST_CASE("qt combo: clearing a missing ID respects a ViewModel fallback") {
    ComboFixture f;
    f.options.push_back(choice("a"));
    f.options.push_back(choice("b"));
    // Register before the binder to exercise a reentrant Property write.
    auto fallback = f.selected.on_changed([&](const auto& key) {
        if (!key) f.selected.set("a");
    });
    f.bind("b");

    f.options.remove_at(1);
    CHECK(f.selected.peek() == std::optional<std::string>{"a"});
    CHECK(f.combo.currentIndex() == 0);
}

TEST_CASE("qt combo: releasing the binding disconnects both directions") {
    ComboFixture f;
    f.options.push_back(choice("a"));
    f.options.push_back(choice("b"));
    f.options.push_back(choice("c"));
    f.bind("a");
    f.binding.release();

    f.selected.set("c");
    CHECK(f.combo.currentIndex() == 0);
    press_key(f.combo, Qt::Key_Down);
    CHECK(f.combo.currentIndex() == 1);
    CHECK(f.selected.peek() == std::optional<std::string>{"c"});
}

TEST_CASE("qt combo: widget destruction before subscription release is safe") {
    ObservableList<Choice> options;
    options.push_back(choice("a"));
    Property<std::optional<std::string>> selected{"a"};
    ObservableListModel<Choice> model{options, {{id_role, "id"}}, choice_data};
    auto combo = std::make_unique<QComboBox>();
    combo->setModel(&model);
    auto binding = bind_combo_box_selection(selected, *combo, string_key, id_role);

    SUBCASE("outside a callback") { combo.reset(); }
    SUBCASE("inside a Property observer") {
        auto destroy = selected.on_changed([&](const auto&) { combo.reset(); });
        options.remove_at(0);
    }

    CHECK_FALSE(combo);
    selected.set("pending");
    options.push_back(choice("pending"));
    binding.release();
    CHECK(selected.peek() == std::optional<std::string>{"pending"});
}

TEST_CASE("qt combo: enum IDs use the default item-data role") {
    enum class Mode { light = 10, dark = 20 };
    Property<std::optional<Mode>> selected{Mode::light};
    QComboBox combo;
    combo.addItem("Same", 10);
    combo.addItem("Same", 20);
    auto binding = bind_combo_box_selection(selected, combo,
        [](const QVariant& data) { return static_cast<Mode>(data.toInt()); });

    CHECK(combo.currentIndex() == 0);
    press_key(combo, Qt::Key_Down);
    CHECK(selected.peek() == std::optional<Mode>{Mode::dark});
    selected.set(Mode::light);
    CHECK(combo.currentIndex() == 0);
}

TEST_CASE("qt combo: failing ID conversion cannot unwind through Qt callbacks") {
    ComboFixture f;
    f.options.push_back(choice("a"));
    f.options.push_back(choice("b"));
    f.selected.set("a");
    bool fail = false;
    auto binding = bind_combo_box_selection(f.selected, f.combo,
        [&](const QVariant& data) {
            if (fail) throw std::runtime_error("invalid option ID");
            return string_key(data);
        }, id_role);
    key_conversion_failures = 0;
    auto previous = set_callback_failure_sink(record_key_conversion_failure);
    Subscription restore{std::function<void()>{[previous] {
        set_callback_failure_sink(previous);
    }}};

    fail = true;
    CHECK_NOTHROW(press_key(f.combo, Qt::Key_Down));
    CHECK(f.selected.peek() == std::optional<std::string>{"a"});
    CHECK(key_conversion_failures == 1);
    CHECK_NOTHROW(f.options.replace_at(0, choice("a", "Renamed")));
    CHECK(key_conversion_failures == 2);
    CHECK_NOTHROW(f.selected.set("b"));
    CHECK(key_conversion_failures == 3);

    fail = false;
    f.selected.set("a");
    CHECK(f.combo.currentIndex() == 0);
    CHECK(key_conversion_failures == 3);
}
