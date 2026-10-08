// Compiled with -fobjc-arc. Objective-C class names carry a unique prefix: the runtime is
// shared by every plugin loaded in the host process.
#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "PlatformUiMac.h"

#include <fstream>
#include <iterator>

#include "logic/Dialogs.h"

using sq8l::gui::MenuItem;
using sq8l::gui::ModalDialog;

namespace {
constexpr double kFormW = 626.0, kFormH = 430.0;
NSString* ns(const std::string& s) {
    return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSWindowsCP1252StringEncoding] ?: @"";
}
std::string cpp(NSString* s) {
    NSData* d = [s dataUsingEncoding:NSWindowsCP1252StringEncoding allowLossyConversion:YES];
    return std::string(static_cast<const char*>(d.bytes), d.length);
}
// VCL captions use '&' for the hotkey and '&&' for a literal ampersand.
NSString* menuTitle(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '&') {
            if (i + 1 < s.size() && s[i + 1] == '&') out += '&', i++;
            continue;
        }
        out += s[i];
    }
    return ns(out);
}
}  // namespace

// ---------------------------------------------------------------- Objective-C helpers
@interface SQ8LPortV1MenuTarget : NSObject
@property(nonatomic) NSInteger chosen;
- (void)pick:(NSMenuItem*)item;
@end
@implementation SQ8LPortV1MenuTarget
- (void)pick:(NSMenuItem*)item { self.chosen = item.tag; }
@end

@interface SQ8LPortV1DialogController : NSObject <NSTableViewDataSource, NSTableViewDelegate, NSWindowDelegate>
@property(nonatomic) ModalDialog* dialog;
@property(nonatomic, strong) NSPanel* panel;
@property(nonatomic, strong) NSTableView* table;
@property(nonatomic, strong) NSButton* compare;
@property(nonatomic) std::function<void(std::function<void()>)> act;  // run under the engine lock + pump
@property(nonatomic) BOOL syncing;
@end

@implementation SQ8LPortV1DialogController
// (dialog is nullptr once the dialog has ended: see PlatformUiMac::runModal)
- (NSInteger)numberOfRowsInTableView:(NSTableView*)tv {
    return self.dialog ? (NSInteger)self.dialog->items.size() : 0;
}
- (id)tableView:(NSTableView*)tv objectValueForTableColumn:(NSTableColumn*)col row:(NSInteger)row {
    if (!self.dialog || row < 0 || (size_t)row >= self.dialog->items.size()) return nil;
    return ns(self.dialog->items[(size_t)row]);
}
- (void)refresh {
    if (!self.dialog) return;
    self.syncing = YES;
    [self.table reloadData];
    if (self.dialog->itemIndex >= 0 && self.dialog->itemIndex < (int)self.dialog->items.size()) {
        [self.table selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)self.dialog->itemIndex]
                byExtendingSelection:NO];
        [self.table scrollRowToVisible:self.dialog->itemIndex];
    } else {
        [self.table deselectAll:nil];
    }
    if (self.compare) {
        auto* sel = dynamic_cast<sq8l::gui::SelSingleDialog*>(self.dialog);
        self.compare.state = sel && sel->compareChecked ? NSControlStateValueOn : NSControlStateValueOff;
    }
    self.syncing = NO;
    [self checkDone];
}
- (void)checkDone {
    if (!self.dialog) return;
    if (self.dialog->modalResult != 0) [NSApp stopModal];
}
- (void)tableViewSelectionDidChange:(NSNotification*)n {
    if (!self.dialog) return;
    if (self.syncing) return;
    const int row = (int)self.table.selectedRow;
    if (row < 0) return;
    ModalDialog* d = self.dialog;
    self.act([d, row] { d->clickItem(row); });
    [self refresh];
}
- (void)doubleClick:(id)sender {
    if (!self.dialog) return;
    const int row = (int)self.table.clickedRow;
    if (row < 0) return;
    ModalDialog* d = self.dialog;
    self.act([d, row] { d->dblClickItem(row); });
    [self refresh];
}
- (void)ok:(id)sender {
    if (!self.dialog) return;
    ModalDialog* d = self.dialog;
    self.act([d] { d->clickOk(); });
    [self refresh];
}
- (void)cancel:(id)sender {
    if (!self.dialog) return;
    ModalDialog* d = self.dialog;
    self.act([d] { d->clickCancel(); });
    [self refresh];
}
- (void)toggleCompare:(id)sender {
    if (!self.dialog) return;
    auto* sel = dynamic_cast<sq8l::gui::SelSingleDialog*>(self.dialog);
    if (sel) self.act([sel] { sel->clickCompare(); });
    [self refresh];
}
- (void)bank:(id)sender {
    auto* sel = dynamic_cast<sq8l::gui::SelSingleDialog*>(self.dialog);
    if (sel) self.act([sel] { sel->clickBank(); });
    [self refresh];
}
- (BOOL)windowShouldClose:(NSWindow*)w {
    [self cancel:nil];
    return NO;
}
@end

@interface SQ8LPortV1NameField : NSTextField
@property(nonatomic) std::function<void(int)> onKey;
@end
@implementation SQ8LPortV1NameField
@end

@interface SQ8LPortV1NameDelegate : NSObject <NSTextFieldDelegate>
@property(nonatomic) std::function<void(const std::string&)> onEnd;
@property(nonatomic) std::function<void(int)> onKey;
@end
@implementation SQ8LPortV1NameDelegate
- (void)controlTextDidChange:(NSNotification*)n {
    NSTextField* f = n.object;
    NSString* up = [f.stringValue uppercaseString];
    if (up.length > 15) up = [up substringToIndex:15];
    if (![up isEqualToString:f.stringValue]) f.stringValue = up;
}
- (BOOL)control:(NSControl*)c textView:(NSTextView*)tv doCommandBySelector:(SEL)sel {
    if (sel == @selector(insertNewline:)) {
        if (self.onKey) self.onKey(0x0d);
        return YES;
    }
    return NO;
}
- (void)controlTextDidEndEditing:(NSNotification*)n {
    NSTextField* f = n.object;
    if (self.onEnd) self.onEnd(cpp(f.stringValue));
}
@end

// ---------------------------------------------------------------- implementation
namespace sq8l::gui {

struct PlatformUiMac::Impl {
    NSView* view = nil;
    Hooks hooks;
    SQ8LPortV1NameField* nameField = nil;
    SQ8LPortV1NameDelegate* nameDelegate = nil;

    double scale() const { return view.bounds.size.width / kFormW; }
    // form point -> view point
    NSPoint toView(int x, int y) const {
        const double s = scale();
        return view.isFlipped ? NSMakePoint(x * s, y * s) : NSMakePoint(x * s, view.bounds.size.height - y * s);
    }
    NSRect formRect(int l, int t, int w, int h) const {
        const double s = scale();
        const NSPoint a = toView(l, view.isFlipped ? t : t + h);
        return NSMakeRect(a.x, a.y, w * s, h * s);
    }
    NSPoint screenOfForm(int x, int y) const {
        NSPoint p = [view convertPoint:toView(x, y) toView:nil];
        return [view.window convertPointToScreen:p];
    }
    struct Released {
        const Hooks& h;
        explicit Released(const Hooks& hk) : h(hk) { if (h.releaseEngine) h.releaseEngine(); }
        ~Released() { if (h.acquireEngine) h.acquireEngine(); }
    };

    NSMenu* buildMenu(const std::vector<MenuItem>& items, SQ8LPortV1MenuTarget* target) {
        NSMenu* m = [[NSMenu alloc] initWithTitle:@""];
        m.autoenablesItems = NO;
        for (const MenuItem& it : items) {
            if (it.separator) {
                [m addItem:[NSMenuItem separatorItem]];
                continue;
            }
            NSMenuItem* mi = [[NSMenuItem alloc] initWithTitle:menuTitle(it.text) action:nil keyEquivalent:@""];
            mi.tag = it.id;
            mi.enabled = it.enabled;
            mi.state = it.checked ? NSControlStateValueOn : NSControlStateValueOff;
            if (it.isDefault) {
                mi.attributedTitle = [[NSAttributedString alloc]
                    initWithString:menuTitle(it.text)
                        attributes:@{NSFontAttributeName : [NSFont boldSystemFontOfSize:[NSFont systemFontSize]]}];
            }
            if (!it.sub.empty()) {
                mi.submenu = buildMenu(it.sub, target);
            } else {
                mi.target = target;
                mi.action = @selector(pick:);
            }
            [m addItem:mi];
        }
        return m;
    }

    NSPanel* makePanel(const std::string& caption, int w, int h) {
        NSPanel* p = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, w, h)
                                                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                  backing:NSBackingStoreBuffered
                                                    defer:NO];
        p.title = ns(caption);
        // centred on form point (315, 214), like the original
        const NSPoint c = screenOfForm(kDialogCenterX, kDialogCenterY);
        [p setFrameOrigin:NSMakePoint(c.x - w / 2.0, c.y - h / 2.0)];
        return p;
    }

    static NSButton* button(NSString* title, NSRect r, id target, SEL action) {
        NSButton* b = [NSButton buttonWithTitle:title target:target action:action];
        b.frame = r;
        b.bezelStyle = NSBezelStyleRounded;
        return b;
    }
};

PlatformUiMac::PlatformUiMac(void* view, Hooks hooks) : impl_(std::make_unique<Impl>()) {
    impl_->view = (__bridge NSView*)view;
    impl_->hooks = std::move(hooks);
}

PlatformUiMac::~PlatformUiMac() {
    if (impl_->nameField) [impl_->nameField removeFromSuperview];
}

int PlatformUiMac::popupMenu(const std::vector<MenuItem>& items, int x, int y) {
    SQ8LPortV1MenuTarget* target = [SQ8LPortV1MenuTarget new];
    target.chosen = 0;
    NSMenu* m = impl_->buildMenu(items, target);
    Impl::Released unlocked(impl_->hooks);
    [m popUpMenuPositioningItem:nil atLocation:impl_->toView(x, y) inView:impl_->view];
    return static_cast<int>(target.chosen);
}

int PlatformUiMac::messageBox(const std::string& text, const std::string& caption, int flags) {
    NSAlert* a = [NSAlert new];
    a.messageText = ns(caption);
    a.informativeText = ns(text);
    const int kind = flags & 0xf;
    if (kind == kMbYesNo) {
        [a addButtonWithTitle:@"Yes"];
        [a addButtonWithTitle:@"No"];
    } else if (kind == kMbOkCancel) {
        [a addButtonWithTitle:@"OK"];
        [a addButtonWithTitle:@"Cancel"];
    } else {
        [a addButtonWithTitle:@"OK"];
    }
    Impl::Released unlocked(impl_->hooks);
    const NSModalResponse r = [a runModal];
    const bool first = r == NSAlertFirstButtonReturn;
    if (kind == kMbYesNo) return first ? kIdYes : kIdNo;
    if (kind == kMbOkCancel) return first ? kIdOk : kIdCancel;
    return kIdOk;
}

bool PlatformUiMac::fileDialog(const FileDialogRequest& req, std::string& path) {
    NSSavePanel* p = req.save ? [NSSavePanel savePanel] : [NSOpenPanel openPanel];
    // filter "Desc (*.a; *.b)|*.a;*.b|..." -> extensions
    NSMutableArray<NSString*>* exts = [NSMutableArray array];
    {
        std::string f = req.filter;
        size_t bar = 0;
        int field = 0;
        while (true) {
            const size_t next = f.find('|', bar);
            const std::string part = f.substr(bar, next == std::string::npos ? std::string::npos : next - bar);
            if (field % 2 == 1) {
                size_t s = 0;
                while (s <= part.size()) {
                    const size_t e = part.find(';', s);
                    std::string pat = part.substr(s, e == std::string::npos ? std::string::npos : e - s);
                    const size_t dot = pat.rfind('.');
                    if (dot != std::string::npos && pat.substr(dot + 1) != "*") [exts addObject:ns(pat.substr(dot + 1))];
                    if (e == std::string::npos) break;
                    s = e + 1;
                }
            }
            field++;
            if (next == std::string::npos) break;
            bar = next + 1;
        }
    }
    // UTType and allowedContentTypes are macOS 11+; the plug-ins also run on 10.15, which
    // has the (now deprecated) extension list instead.
    if (exts.count) {
        if (@available(macOS 11.0, *)) {
            NSMutableArray<UTType*>* types = [NSMutableArray array];
            for (NSString* ext in exts)
                if (UTType* t = [UTType typeWithFilenameExtension:ext]) [types addObject:t];
            if (types.count) p.allowedContentTypes = types;
        } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            p.allowedFileTypes = exts;
#pragma clang diagnostic pop
        }
    }
    p.allowsOtherFileTypes = YES;
    if (!req.initialDir.empty()) p.directoryURL = [NSURL fileURLWithPath:ns(req.initialDir) isDirectory:YES];
    if (!req.fileName.empty()) {
        std::string name = req.fileName;
        const size_t slash = name.find_last_of("\\/");
        if (slash != std::string::npos) name = name.substr(slash + 1);
        p.nameFieldStringValue = ns(name);
    }
    Impl::Released unlocked(impl_->hooks);
    if ([p runModal] != NSModalResponseOK || !p.URL) return false;
    path = cpp(p.URL.path);
    return true;
}

bool PlatformUiMac::readFile(const std::string& path, std::vector<uint8_t>& data) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    data.assign(std::istreambuf_iterator<char>(f), {});
    return !data.empty();
}

bool PlatformUiMac::writeFile(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(f);
}

Point PlatformUiMac::cursorPos() {
    NSView* v = impl_->view;
    if (!v.window) return {};
    const NSPoint screen = [NSEvent mouseLocation];
    const NSPoint win = [v.window convertPointFromScreen:screen];
    const NSPoint p = [v convertPoint:win fromView:nil];
    const double s = impl_->scale();
    const double y = v.isFlipped ? p.y : v.bounds.size.height - p.y;
    return {static_cast<int>(p.x / s), static_cast<int>(y / s)};
}

void PlatformUiMac::setCursorPos(Point p) {
    NSView* v = impl_->view;
    if (!v.window) return;
    const NSPoint screen = impl_->screenOfForm(p.x, p.y);
    // Cocoa screen coordinates (origin bottom-left of the main screen) -> CG global (top-left)
    const CGFloat mainH = NSScreen.screens.firstObject.frame.size.height;
    CGWarpMouseCursorPosition(CGPointMake(screen.x, mainH - screen.y));
    CGAssociateMouseAndMouseCursorPosition(true);
}

void PlatformUiMac::focusForm() {
    if (impl_->nameField) [impl_->view.window makeFirstResponder:impl_->view];
}

void PlatformUiMac::beginNameEdit(int left, int top, int width, int height, const std::string& text) {
    Impl& I = *impl_;
    if (!I.nameField) {
        I.nameField = [[SQ8LPortV1NameField alloc] initWithFrame:NSZeroRect];
        I.nameField.bezeled = NO;
        I.nameField.bordered = NO;
        I.nameField.drawsBackground = YES;
        I.nameField.backgroundColor = NSColor.blackColor;
        I.nameField.textColor = [NSColor colorWithSRGBRed:0x78 / 255.0 green:0x79 / 255.0 blue:0x8b / 255.0 alpha:1];
        I.nameField.focusRingType = NSFocusRingTypeNone;
        I.nameDelegate = [SQ8LPortV1NameDelegate new];
        PlatformUiMac* self = this;
        I.nameDelegate.onEnd = [self](const std::string& t) {
            Impl& J = *self->impl_;
            J.nameField.hidden = YES;
            if (J.hooks.nameFocus) J.hooks.nameFocus(false, t);
        };
        I.nameDelegate.onKey = [self](int key) {
            Impl& J = *self->impl_;
            if (J.hooks.nameKey) J.hooks.nameKey(key);
            [J.view.window makeFirstResponder:J.view];
        };
        I.nameField.delegate = I.nameDelegate;
        [I.view addSubview:I.nameField];
    }
    const double s = I.scale();
    I.nameField.font = [NSFont fontWithName:@"Arial" size:11 * s] ?: [NSFont systemFontOfSize:11 * s];
    I.nameField.frame = I.formRect(left, top, width, height);
    I.nameField.stringValue = ns(text);
    I.nameField.hidden = NO;
    [I.view.window makeFirstResponder:I.nameField];
    if (I.hooks.nameFocus) I.hooks.nameFocus(true, text);
}

bool PlatformUiMac::nameEditing() const { return impl_->nameField && !impl_->nameField.hidden; }

void PlatformUiMac::runModal(ModalDialog& dialog) {
    Impl& I = *impl_;
    const bool sel = dialog.kind() == ModalDialog::Kind::SelectProgram;
    // TSelSingleForm 196x352 / TmidiSelForm 194x314 client areas (original layout, y flipped)
    const int W = sel ? 196 : 194, H = sel ? 352 : 314;
    NSPanel* panel = I.makePanel(dialog.caption, W, H);
    SQ8LPortV1DialogController* c = [SQ8LPortV1DialogController new];
    c.dialog = &dialog;
    c.panel = panel;
    const Hooks hooks = I.hooks;
    c.act = [hooks](std::function<void()> f) {
        if (hooks.acquireEngine) hooks.acquireEngine();
        f();
        if (hooks.pump) hooks.pump();
        if (hooks.releaseEngine) hooks.releaseEngine();
    };
    NSView* content = panel.contentView;
    auto flip = [H](int top, int height) { return H - top - height; };
    const int listTop = sel ? 45 : 8;
    NSScrollView* scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(8, flip(listTop, 265), 179, 265)];
    NSTableView* table = [[NSTableView alloc] initWithFrame:scroll.bounds];
    NSTableColumn* col = [[NSTableColumn alloc] initWithIdentifier:@"name"];
    col.width = 170;
    [table addTableColumn:col];
    table.headerView = nil;
    table.rowHeight = 13;
    table.font = [NSFont systemFontOfSize:11];
    table.dataSource = c;
    table.delegate = c;
    table.target = c;
    table.doubleAction = @selector(doubleClick:);
    scroll.documentView = table;
    scroll.hasVerticalScroller = YES;
    [content addSubview:scroll];
    c.table = table;
    const int btnTop = sel ? 319 : 280;
    [content addSubview:Impl::button(ns(dialog.okCaption), NSMakeRect(8, flip(btnTop, 25), 82, 25), c, @selector(ok:))];
    [content addSubview:Impl::button(ns(dialog.cancelCaption), NSMakeRect(104, flip(btnTop, 25), 82, 25), c,
                                     @selector(cancel:))];
    if (sel) {
        NSButton* cmp = [NSButton checkboxWithTitle:@"COMPARE" target:c action:@selector(toggleCompare:)];
        cmp.frame = NSMakeRect(8, flip(8, 17), 90, 17);
        [content addSubview:cmp];
        c.compare = cmp;
        [content addSubview:Impl::button(@"Bank", NSMakeRect(112, flip(8, 25), 75, 25), c, @selector(bank:))];
    }
    panel.delegate = c;
    // ShowModal: OnShow runs inside the dialog logic before the loop (showModal), so the
    // platform only runs the loop here.
    [c refresh];
    if (dialog.modalResult == 0) {
        Impl::Released unlocked(I.hooks);
        [NSApp runModalForWindow:panel];
    }
    [panel orderOut:nil];
    // The panel and its table outlive this call (AppKit releases windows later), while the
    // dialog and this controller end here. macOS 10.15 redraws the hidden table during the
    // next modal loop (the NSAlert that often follows): with its data source still set it
    // read the destroyed dialog and crashed (issue #12). Detach everything, then close.
    table.dataSource = nil;
    table.delegate = nil;
    table.target = nil;
    panel.delegate = nil;
    c.dialog = nullptr;
    c.table = nil;
    c.compare = nil;
    panel.releasedWhenClosed = NO;
    [panel close];
}

void PlatformUiMac::showModInfo(const std::vector<std::string>& lines) {
    std::string text;
    for (const std::string& l : lines) text += l + "\n";
    NSAlert* a = [NSAlert new];
    a.messageText = @"Modulation sources";
    a.informativeText = ns(text);
    [a addButtonWithTitle:@"OK"];
    Impl::Released unlocked(impl_->hooks);
    [a runModal];
}

void PlatformUiMac::showAbout(const std::string& text) {
    NSAlert* a = [NSAlert new];
    a.messageText = @"SQ8L";
    std::string t;
    for (char ch : text)
        if (ch != '\r') t += ch;
    a.informativeText = ns(t);
    [a addButtonWithTitle:@"OK"];
    Impl::Released unlocked(impl_->hooks);
    [a runModal];
}

bool macRunLoopStep(double seconds) {
    @autoreleasepool {
        NSEvent* ev = [NSApp nextEventMatchingMask:NSEventMaskAny
                                         untilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]
                                            inMode:NSDefaultRunLoopMode
                                           dequeue:YES];
        if (ev) [NSApp sendEvent:ev];
    }
    return true;
}

}  // namespace sq8l::gui
