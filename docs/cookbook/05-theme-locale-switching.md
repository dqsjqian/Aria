# Recipe 5 — Theme / Locale reactive switching

**Goal:** flip the app theme or language at runtime and have every
derived label / colour update automatically, with no manual refresh.

The framework piece is just `Property<T>` + `Computed<T>`: model the
current theme/locale as a `Property`, and derive every user-visible
string/colour as a `Computed` that reads it. The app defines its own
`Theme` / `Locale` enums — Aria provides the reactivity, not the
catalogue.

```cpp
#include "aria/reactive/reactive.hpp"   // Property, Computed, batch

enum class Locale { En, Zh, Ja };
enum class Theme  { Light, Dark };

aria::Property<Locale> locale{Locale::En};
aria::Property<Theme>  theme {Theme::Light};

// A derived label recomputes whenever `locale` changes.
aria::Computed<std::string> greeting{[&]{
    switch (locale.get()) {
        case Locale::Zh: return std::string{"你好"};
        case Locale::Ja: return std::string{"こんにちは"};
        default:         return std::string{"Hello"};
    }
}};

// A derived colour recomputes whenever `theme` changes.
aria::Computed<Color> bg{[&]{
    return theme.get() == Theme::Dark ? Color::Charcoal : Color::White;
}};
```

Bind the computed values to widgets; switching is a single Property
write:

```cpp
auto g_sub = greeting.bind([](const std::string& s){ title_label.text = s; });
auto c_sub = bg.bind     ([](Color c){ root_view.background = c; });

void switch_to_chinese_dark() {
    aria::batch([&]{          // coalesce both writes into one flush
        locale.set(Locale::Zh);
        theme.set(Theme::Dark);
    });
    // greeting -> "你好", bg -> Charcoal, both delivered once, glitch-free.
}
```

## Why this is glitch-free

- **`Computed` is pull-based and equality-gated** (L-21 / L-22): a
  recompute that produces the same value does not notify downstream, so
  flipping theme does not needlessly refire locale-only labels.
- **`batch` coalesces** (L-20): wrapping the two writes means the graph
  flushes once, after both Properties are set — observers never see the
  intermediate "Chinese + Light" half-state.
- **Dynamic dependencies** (L-17): a `Computed` that reads `locale` only
  in some branches is subscribed to `locale` only while that branch is
  live — no ghost subscriptions when the branch flips.

## Scaling up

For a real string catalogue, make the `Computed` read both `locale` and
a `Property<const StringTable*>` (hot-reloadable translations), or keep a
`Computed<const StringTable&>` selected by `locale` and have each label
`Computed` read `table().lookup(key)`. The pattern composes: every label
is a node that re-pulls only when something it actually read changed.
