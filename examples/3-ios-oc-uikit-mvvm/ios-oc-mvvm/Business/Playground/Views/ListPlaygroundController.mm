#import "ListPlaygroundController.h"
#import "ListPlaygroundView.h"

#import "UIKitTableSource.hpp"   // aria::adapters::uikit::ObservableTableSource

#include "aria/observable_list.hpp"
#include "aria/derived/mapped_list.hpp"

#include <memory>
#include <string>

// ════════════════════════════════════════════════════════════════════════════════
// ListPlaygroundController — Panel C (full-screen pageSheet, UITableView)
//
// Showcases the aria ObservableList → MappedList → UITableView pipeline.
////   Model (domain)        :  Person   { name; tag; }
//                                    │
//                                    │  std::shared_ptr<ObservableList<Person>>
//                                    ▼
//   View-model (per-row)  :  PersonVM { display; subtitle; iconName; }
//                                    │
//                                    │  MappedList<Person, PersonVM>
//                                    ▼
//   Bridge                :  aria::adapters::uikit::ObservableTableSource<PersonVM>
//                                    │
//                                    │  UITableViewDataSource / Delegate
//                                    ▼
//   UI                    :  UITableView  (insertRows / deleteRows / moveRow)
//
// Highlights:
//   * The controller does NOT implement UITableViewDataSource/Delegate
//     and does NOT keep an NSMutableArray. The dataSource is the
//     AriaUITableDataSource that ObservableTableSource builds internally
//     (declared in UIKitTableSource.hpp).
//   * "Add" / "Shuffle" / "Reset" all call straight into
//     ObservableList<Person>::push_back / move / clear; UITableView
//     animations are driven by ListChange events. MappedList projects
//     each Person into a PersonVM, and TableSource turns the
//     ListChange<PersonVM> stream into insertRowsAtIndexPaths /
//     moveRowAtIndexPath:toIndexPath: / deleteRowsAtIndexPaths.
//   * Swipe-delete is wired through trailingSwipeActionsConfiguration;
//     its handler calls source->remove_at(row).
//
// ════════════════════════════════════════════════════════════════════════════════
namespace {

// ── Model ──────────────────────────────────────────────────────────────────
struct Person {
    std::string name;
    std::string tag;
};

// ── View-model ─────────────────────────────────────────────────────────────
//
// Project Person into the row-view-friendly strings. MappedList calls
// the mapper once per Insert/Replace; ItemChanged does NOT rebuild
// (the Target identity is preserved), and Move does not rebuild either.
// That means a Move triggers zero mapper invocations on the cell side.
struct PersonVM {
    std::string display;
    std::string subtitle;
    std::string icon_name;
};

PersonVM make_vm(const Person& p, NSInteger row_hint) {
    static NSArray<NSString *> *symbols = @[ @"star.fill", @"heart.fill",
                                              @"bolt.fill", @"flame.fill",
                                              @"leaf.fill" ];
    NSString *symbol = symbols[(NSUInteger)(row_hint % (NSInteger)symbols.count)];
    return PersonVM{
        /*display=*/  p.name,
        /*subtitle=*/ "tag: " + p.tag,
        /*icon_name=*/ std::string(symbol.UTF8String),
    };
}

}  // namespace

// ─── Controller ────────────────────────────────────────────────────────────

@interface ListPlaygroundController () {
    // Domain-level list. ObservableList<Person> is the source of truth.
    std::shared_ptr<aria::ObservableList<Person>> _people;

    // Derived per-row VM list. Mapper closure also bakes in the row
    // hint (we use the insertion order to pick the icon — purely
    // cosmetic; the binding contract works fine without it).
    //
    // Hold by shared_ptr so the bridge can observe it.
    std::shared_ptr<aria::MappedList<Person, PersonVM>> _vms;

    // The bridge between MappedList and UITableView. Owning unique_ptr
    // — destruction order matters: `~ObservableTableSource` detaches
    // its source subscription BEFORE clearing tableView.dataSource,
    // which is the contract we want during view tear-down.
    std::unique_ptr<aria::adapters::uikit::ObservableTableSource<PersonVM>> _bridge;

    // Counter for the "Add" button so each new row has a unique name.
    NSInteger _addCounter;
}
@end

@implementation ListPlaygroundController

#pragma mark - View lifecycle

- (void)loadView {
    self.view = [[ListPlaygroundView alloc] init];
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Panel C · ObservableList";

    // ── 1. Domain list ──────────────────────────────────────────────
    _people = std::make_shared<aria::ObservableList<Person>>();
    for (int i = 1; i <= 12; ++i) {
        _people->push_back(std::make_shared<Person>(Person{
            /*name=*/ std::string("Person ") + std::to_string(i),
            /*tag=*/  std::string("seed#") + std::to_string(i),
        }));
    }

    // ── 2. Derived view-model list ──────────────────────────────────
    //
    // The mapper captures `_people.get()` only as a row-counter hint
    // for the icon; it does NOT need the source pointer for anything
    // load-bearing. (MappedList itself drives Insert/Replace/Move.)
    auto vms = std::make_shared<aria::MappedList<Person, PersonVM>>(
        _people,
        [](const Person& p) {
            // Row hint for the icon = current PersonVM count, but we
            // don't have access to it here cleanly. Use the name
            // length as a fallback "stable but spread" picker; the
            // important thing is we return something deterministic.
            NSInteger hint = (NSInteger)p.name.size();
            return std::make_shared<PersonVM>(make_vm(p, hint));
        });
    _vms = vms;

    // ── 3. UITableView bridge ───────────────────────────────────────
    UITableView *tv = self.rootView.tableView;
    NSString *reuse = self.rootView.cellReuseIdentifier;

    auto cellFn = [reuse](UITableView *tableView,
                           std::shared_ptr<PersonVM> vm,
                           NSIndexPath *indexPath) -> UITableViewCell * {
        LPGItemCell *cell = (LPGItemCell *)
            [tableView dequeueReusableCellWithIdentifier:reuse
                                            forIndexPath:indexPath];
        cell.titleLabel.text    = [NSString stringWithUTF8String:vm->display.c_str()];
        cell.subtitleLabel.text = [NSString stringWithUTF8String:vm->subtitle.c_str()];
        NSString *symbol = [NSString stringWithUTF8String:vm->icon_name.c_str()];
        cell.iconView.image = [UIImage systemImageNamed:symbol];
        return cell;
    };

    _bridge = std::make_unique<
        aria::adapters::uikit::ObservableTableSource<PersonVM>>(
            tv, *_vms, std::move(cellFn));

    // ── 4. Tap-to-toast — we still need a delegate hop for selection
    //                     and swipe-to-delete; the bridge owns the
    //                     dataSource, but we install ourselves as
    //                     the delegate AFTER the bridge so we get
    //                     the selection + swipe callbacks while the
    //                     bridge keeps the row-count / cellForRow
    //                     responsibility.
    //
    // NOTE: `tv.delegate` was already set by the bridge to its own
    // AriaUITableDataSource (which conforms to UITableViewDelegate
    // but doesn't implement selection). Overriding here is fine —
    // we simply intercept the events we care about.
    tv.delegate = (id<UITableViewDelegate>)self;

    // ── 5. Navigation Add button ────────────────────────────────────
    self.navigationItem.rightBarButtonItems = @[
        [[UIBarButtonItem alloc]
            initWithBarButtonSystemItem:UIBarButtonSystemItemAdd
                                 target:self
                                 action:@selector(onAddTapped)],
        [[UIBarButtonItem alloc]
            initWithImage:[UIImage systemImageNamed:@"shuffle"]
                    style:UIBarButtonItemStylePlain
                   target:self
                   action:@selector(onShuffleTapped)],
        [[UIBarButtonItem alloc]
            initWithImage:[UIImage systemImageNamed:@"trash"]
                    style:UIBarButtonItemStylePlain
                   target:self
                   action:@selector(onResetTapped)],
    ];

    _addCounter = 12;
}

#pragma mark - Typed accessor

- (ListPlaygroundView *)rootView {
    return (ListPlaygroundView *)self.view;
}

#pragma mark - Toolbar actions (drive the ObservableList)

- (void)onAddTapped {
    ++_addCounter;
    auto p = std::make_shared<Person>(Person{
        /*name=*/ "Person " + std::to_string(_addCounter) + " (new)",
        /*tag=*/  "added",
    });
    _people->insert(0, std::move(p));   // insert at top → Insert(0)
    UITableView *tv = self.rootView.tableView;
    [tv scrollToRowAtIndexPath:[NSIndexPath indexPathForRow:0 inSection:0]
              atScrollPosition:UITableViewScrollPositionTop
                      animated:YES];
}

- (void)onShuffleTapped {
    // Demonstrate Move bridging — pick the top row and move it to
    // the bottom. UITableView animates this via
    // [moveRowAtIndexPath:toIndexPath:].
    const std::size_t n = _people->size();
    if (n < 2) return;
    _people->move(0, n - 1);
    [self.rootView showToast:@"Moved row 0 → bottom"];
}

- (void)onResetTapped {
    _people->clear();
    [self.rootView showToast:@"List cleared"];
}

#pragma mark - UITableViewDelegate (selection + swipe-to-delete)

- (void)tableView:(UITableView *)tableView
didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    if (indexPath.row < 0
        || (NSUInteger)indexPath.row >= _people->size()) return;
    auto person = _people->at((std::size_t)indexPath.row);
    if (!person) return;
    NSString *msg = [NSString stringWithFormat:@"Tapped: %s",
                     person->name.c_str()];
    [self.rootView showToast:msg];
}

- (UISwipeActionsConfiguration *)tableView:(UITableView *)tableView
    trailingSwipeActionsConfigurationForRowAtIndexPath:(NSIndexPath *)indexPath {
    __weak __typeof(self) weakSelf = self;
    UIContextualAction *del = [UIContextualAction
        contextualActionWithStyle:UIContextualActionStyleDestructive
                            title:@"Delete"
                          handler:^(UIContextualAction * _Nonnull,
                                    __kindof UIView * _Nonnull,
                                    void (^ _Nonnull completion)(BOOL)) {
        __strong __typeof(weakSelf) self = weakSelf;
        if (!self) { completion(NO); return; }
        // Drive the model — ObservableTableSource turns this into a
        // [tableView deleteRowsAtIndexPaths:withRowAnimation:].
        if (indexPath.row >= 0
            && (NSUInteger)indexPath.row < self->_people->size()) {
            self->_people->remove_at((std::size_t)indexPath.row);
        }
        completion(YES);
    }];
    return [UISwipeActionsConfiguration configurationWithActions:@[del]];
}

@end
