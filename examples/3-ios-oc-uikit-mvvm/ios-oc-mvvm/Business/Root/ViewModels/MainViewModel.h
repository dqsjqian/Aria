#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@class DataModel;

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// 5 patterns for View ↔ ViewModel ↔ Model communication
//
//  Pattern 1 — Completion Block:    VM exposes a block property, View sets it
//  Pattern 2 — NSNotification:      VM posts notification, View observes
//  Pattern 3 — KVO:                 VM writes observable property, View KVO-observes
//  Pattern 4 — Protocol Delegate:   VM calls delegate method on View
//  Pattern 5 — Target-Action:       VM stores target+selector, calls performSelector
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

// ── Notification names (Pattern 2 & 5) ─────────────────────────────────────
extern NSString * const MVVMNotificationBlockResult;    // Pattern 2
extern NSString * const MVVMNotificationKvoResult;      // Pattern 3
extern NSString * const MVVMNotificationDelegateResult;  // Pattern 4
extern NSString * const MVVMNotificationTargetActionResult; // Pattern 5
extern NSString * const MVVMNotificationResultKey;       // userInfo key
extern NSString * const MVVMNotificationPatternKey;      // userInfo key

// ── Protocol (Pattern 4) ────────────────────────────────────────────────────
@protocol MainViewModelDelegate <NSObject>
@optional
- (void)viewModel:(id)vm didReturnResult:(NSString *)result pattern:(NSString *)pattern;
- (void)viewModel:(id)vm didReturnError:(NSString *)error pattern:(NSString *)pattern;
@end

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
@interface MainViewModel : NSObject

@property (nonatomic, strong, readonly) DataModel *model;

// ── Pattern 1: Completion Block ─────────────────────────────────────────────
// View sets this block; VM calls it on completion.
@property (nonatomic, copy, nullable) void (^blockResultHandler)(NSString *message);

// ── Pattern 3: KVO observable properties ────────────────────────────────────
// View can KVO-observe these. VM writes them after model responds.
@property (nonatomic, copy, nullable) NSString *kvoResult;
@property (nonatomic, copy, nullable) NSString *kvoStatus;

// ── Pattern 4: Delegate ─────────────────────────────────────────────────────
@property (nonatomic, weak, nullable) id<MainViewModelDelegate> delegate;

// ── Pattern 5: Target-Action ────────────────────────────────────────────────
// View registers target+selector; VM calls it on completion.
@property (nonatomic, weak, nullable) id targetActionTarget;
@property (nonatomic, nullable) SEL targetActionSelector;

- (instancetype)initWithModel:(DataModel *)model;

// ── Actions (called by View when button is tapped) ──────────────────────────
- (void)fetchViaBlock;
- (void)fetchViaNotification;
- (void)fetchViaKVO;
- (void)fetchViaDelegate;
- (void)fetchViaTargetAction;

@end

NS_ASSUME_NONNULL_END
