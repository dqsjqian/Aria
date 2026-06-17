#import <UIKit/UIKit.h>

// Panel C — full-screen pageSheet "list sample".
//
// Wired to the aria three-stage data stack:
//   ObservableList<Person>
//        │  MappedList<Person, PersonVM>
//        │  ObservableTableSource<PersonVM>   (UIKit bridge)
//        ▼
//   UITableView   (insertRows / deleteRows / moveRow are all driven
//                  by aria list-change events)
//
// The controller does NOT implement UITableViewDataSource itself; it
// only keeps the selection / swipe-delete delegate callbacks and wires
// Add / Shuffle / Reset toolbar buttons to demonstrate Insert / Move /
// Reset ListChange events flowing through the bridge.
@interface ListPlaygroundController : UIViewController
@end
