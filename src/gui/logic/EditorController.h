// The editor form logic: port of unit plugEdit (TplugEditForm, 0x4826e4..0x487374) on top of
// the ported controls of EditorView (layers 1-2). It installs the event handlers the original
// form installs (DFM events + the run-time ones of FormShow FUN_00483d9c), owns the VFD page
// controller (LcdController = ClcdCtr) and the menus, and talks to the engine through
// EditorHost and to the platform through PlatformUi.
//
// Time and asynchronous events follow the original's Windows message queue: the 20 ms timer
// posts WM_APP+2, the engine posts WM_APP+3 notifications, a chosen popup menu item arrives as
// WM_COMMAND. Call idle() (timer + pump, like the oracle's GuiState.idle) or timerTick() /
// pump() yourself.
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "EditorHost.h"
#include "EditorView.h"
#include "LcdControl.h"
#include "MouseJump.h"
#include "PlatformUi.h"

namespace sq8l::gui {

constexpr int kNumKnobs = 10;  // lcdKnob0..9

// Window messages of the form (TplugEditForm dynamic methods 0x8000..0x8003 + WM_COMMAND).
enum : uint32_t {
    kMsgCommand = 0x111,       // WM_COMMAND from a popup menu: wParam = MenuItem id
    kMsgSysexReceived = 0x8000,// 0x4835ec: "** SysEx dump received **"
    kMsgRefresh = 0x8001,      // 0x483620: FUN_004842d8
    kMsgTimer = 0x8002,        // FUN_00483628
    kMsgNotify = 0x8003,       // FUN_0048365c: wParam hi 0 = settings, 1 = program loaded
};

// A menu item object (TMenuItem) of the form's menus.
struct MenuNode {
    std::string caption;
    int tag = 0;
    bool visible = true;
    bool enabled = true;
    bool radio = false;
    bool checked = false;
    bool isDefault = false;
    bool barBreak = false;
    int groupIndex = 0;         // TMenuItem.GroupIndex (radio groups)
    std::function<void(MenuNode&)> onClick;
    std::vector<MenuNode> items;
    MenuNode* parent = nullptr;

    void setChecked(bool c);   // TMenuItem.SetChecked (radio items turn their siblings off)
};

struct PopupMenuDef {           // TPopupMenu
    std::vector<MenuNode> items;
    bool autoHotkeys = true;    // maAutomatic (DFM menus); dynamic menus use maManual
};

class EditorController : public LcdListener {
public:
    // firstInstance: the global "first editor" flag of the DLL (DAT_004c3070) that shows the
    // welcome message.
    EditorController(EditorView& view, EditorHost& host, PlatformUi& ui, bool firstInstance = true);
    ~EditorController() override;

    // FormShow (TplugEditForm_FormShow 0x48423c): the run-time set up FUN_00483d9c, once.
    void show();

    // ------------------------------------------------------------ messages / time
    void post(uint32_t msg, uint32_t wParam = 0, int32_t lParam = 0);
    void timerTick();            // CsimpleTimer callback 0x4835d0: PostMessage(WM_APP+2)
    int pump(int limit = 1000);  // dispatch posted messages (oldest first)
    void idle(int ms);           // per 20 ms: timerTick() + pump()
    bool hasPending() const { return !queue_.empty(); }

    // ------------------------------------------------------------ input that is not a mouse
    // message on an EditorView control
    // WM_CONTEXTMENU at form client coordinates (Windows sends it after a right click; the
    // VCL gives it to a graphic control with an auto-popup menu first, else to the form).
    void contextMenu(int x, int y);
    // progNameEdit focus (WM_SETFOCUS / WM_KILLFOCUS through the hooked window procedure
    // FUN_004839a4) with the edit text, and its OnKeyDown (VK_RETURN ends editing).
    void nameEditFocus(bool focused, const std::string& text);
    void nameEditKeyDown(int key);
    // SysEx program received on the MIDI input after REQ (FUN_0048427c).
    void sysexReceived(const uint8_t* data, size_t size);

    // ------------------------------------------------------------ state (tests)
    LcdController& lcd() { return *ctr_; }
    MouseJump& mouseJump() { return mouseJump_; }
    const PopupMenuDef& fileMenu() const { return fileMenu_; }
    const PopupMenuDef& optionsMenu() const { return optionsMenu_; }
    const PopupMenuDef& infoMenu() const { return infoMenu_; }
    bool restMouseMenu() const { return restMouseMenu_; }   // +0x504
    bool hideCursorEnabled() const { return hideCursor_; }  // +0x505 (port: see setHideCursor)
    int lcdDragKnob() const { return lcdDragKnob_; }        // +0x508 (-1 none)
    int keyCaptMode() const { return keyCaptMode_; }        // +0x50c
    bool swapProgUpDn() const { return swapProgUpDn_; }     // +0x510
    bool rmbScroll() const { return rmbScroll_; }           // +0x511
    int tickDivider() const { return tickDivider_; }        // +0x4e4
    int midiInPort() const { return midiIn_; }              // +0x4f0
    int midiOutPort() const { return midiOut_; }            // +0x4f4
    int sysexRequestPending() const { return sysexReq_; }   // +0x4ec
    bool keyboardCaptured() const { return keyCapture_; }

    // LcdListener (ClcdCtr callbacks)
    void lcdKnobValue(int knob, int value) override;
    void lcdKnobRange(int knob, int min, int max) override;
    void lcdDrawn() override;
    void lcdParamMouseDown(int param) override;
    void lcdHint(int param) override;
    void lcdSaveMouse() override;
    void lcdRestoreMouse() override;
    void lcdScrollArrows(bool down, bool up) override;
    void lcdParamChanged(int page, int param) override;
    void lcdShowPopup(const LcdPopup& popup) override;

private:
    // FormShow pieces
    void buildMenus();
    void wireControls();
    // message handlers
    void onTimer();                           // FUN_00483628
    void updateVoices();                      // 0x483804
    void onNotify(uint32_t wParam, int32_t lParam);  // FUN_0048365c
    void settingsChanged();                   // FUN_00483b08
    void refreshAll();                        // FUN_004842d8
    void refreshProgram();                    // FUN_00484304
    void updateLeds(bool redraw);             // FUN_004843cc
    void setRestMouseMenu(bool b);            // FUN_00483aac
    // (port) OPTIONS -> Mouse -> "Hide cursor when editing": the cursor disappears for
    // the length of a knob turn and comes back where the turn started (issue #25).
    void setHideCursor(bool b);
    void hideCursor();  // a knob turn began / ended; both are no-ops unless the option is on
    void showCursor();
    // status bar / VFD messages
    void setStatus(const std::string& s);     // FUN_0048367c
    void setVoicesText(const std::string& s); // FUN_004836cc
    void showMessage(const std::string& text, int ticks = -1);                          // FUN_0048371c
    void showMessage2(const std::string& l1, const std::string& l2, int ticks = -1);     // FUN_00483784
    // mouse jump wrappers (FUN_00483a5c / FUN_00483a84): owner 0 = the form
    void saveMouse(const void* owner);
    void restoreMouse(const void* owner);
    void focusForm();                         // FUN_00483a3c
    // program selection
    int bank() const;                         // FUN_0048495c
    int prog() const;                         // FUN_004849a0
    void selectProgram(int p);                // FUN_004849b0
    void selectBank(int b);                   // FUN_0048496c
    // pages
    void pageButton(int tag, bool cycle);     // FUN_00484a10
    void highlightParam(int param);           // SUB_00484800
    // popups
    void popup(PopupMenuDef& menu, int x, int y);
    void buildPagePopup();                    // FUN_00484ab0
    void buildProgramPopup();                 // FUN_00484eb4
    void programDblClick();                   // TplugEditForm_programDblClick
    void dispatchCommand(int id);
    // event handlers (DFM names)
    void mouseMoveHint(Control& sender);      // TplugEditForm_MouseMoveHint
    void menuButtonClick(ImageArea& image, PopupMenuDef& menu);
    void panicClick();                        // menuPanicImageClick
    void upButtonClick();
    void downButtonClick();
    void writeButtonClick();
    void initButtonClick();
    void bankButtonClick();
    void syncClick();
    void amClick(int tag);
    void monoClick();
    void lcdKnobChange(Knob& k);
    void displayPanelContextPopup(int x, int y);
    // FILE menu
    void loadLibrary();
    void saveLibrary();
    void initLibrary();
    void loadBank();
    void saveBank();
    void initBank();
    void importBank();
    void exportBank();
    void importProgram();
    void exportProgram();
    void requestProgram();      // menu_reqSingleClick
    void receiveProgram();      // menu_recSingleClick
    void sendProgram();         // menu_sendSingleClick
    void sysexSendButton();
    void sysexReqButton();
    bool sendSysex(int outPort);          // FUN_004869b0
    bool requestSysex(int outPort, int inPort);  // FUN_00486bb0
    int selectMidiPort(const std::string& title, bool output, int current);  // FUN_0047d5f4
    bool fileDialog(FileDialogRequest& rq, std::string& path);  // TOpenDialog/TSaveDialog.Execute
    // OPTIONS / INFO
    void emuModeClick(MenuNode& item);
    void restMouseMenuClick(MenuNode& item);
    void hideCursorClick(MenuNode& item);
    void rmbScrDispClick(MenuNode& item);
    void swapProgUpDnClick(MenuNode& item);  // port addition
    void confirmLoadClick(MenuNode& item);   // port addition
    void polyphonyClick(MenuNode& item);     // port addition
    void zoomClick(MenuNode& item);          // port addition
    void showModInfo();         // FUN_0047c9c0
    void showAbout();           // 0x47d260
    // dialogs
    bool selectProgramDialog(const std::string& caption, const std::string& okCaption,
                             const std::string& cancelCaption, int& bank, int& prog);  // 0x47dd28

    EditorView& view_;
    EditorHost& host_;
    PlatformUi& ui_;
    bool firstInstance_;
    std::unique_ptr<LcdController> ctr_;      // +0x4dc
    MouseJump mouseJump_;                     // global CmouseJump
    std::deque<std::array<uint32_t, 3>> queue_;

    // TplugEditForm fields
    bool firstShow_ = true;          // +0x524
    int tickDivider_ = 0;            // +0x4e4
    int sysexReq_ = 0;               // +0x4ec
    int midiIn_ = -1, midiOut_ = -1; // +0x4f0 / +0x4f4
    std::string libDir_, sysexDir_;  // +0x4f8 / +0x4fc
    // FileName of the form's TOpenDialog (+0x2fc) / TSaveDialog (+0x300): the last chosen file
    std::string openFileName_, saveFileName_;
    const Control* lastHint_ = nullptr;  // +0x500
    bool restMouseMenu_ = false;     // +0x504
    bool hideCursor_ = false;        // +0x505 (port)
    bool cursorHidden_ = false;      // a knob turn is hiding it right now
    int lcdDragKnob_ = -1;           // +0x508
    int keyCaptMode_ = 0;            // +0x50c
    bool swapProgUpDn_ = false;      // +0x510
    bool rmbScroll_ = false;         // +0x511
    bool keyCapture_ = false;        // CkeyCapt active for the form

    PopupMenuDef fileMenu_, optionsMenu_, infoMenu_, pageMenu_, programMenu_;
    // menu items referenced by the form (menu_restMouseMenu etc.)
    MenuNode* menuRestMouseMenu_ = nullptr;
    MenuNode* menuHideCursor_ = nullptr;
    MenuNode* menuRmbScrDisp_ = nullptr;
    MenuNode* menuSwapProgUpDn_ = nullptr;  // port addition (nullptr without portExtensions)
    MenuNode* menuConfirmLoad_ = nullptr;   // port addition
    MenuNode* menuPolyphony_ = nullptr;     // port addition (sub-menu, radio items tagged with the voices)
    MenuNode* menuZoom_ = nullptr;          // port addition (sub-menu, radio items tagged with the percent)
    MenuNode* menuSynth_[5][4] = {};  // [setting][0 prog, 1.., ..] radio items
    // the popup menu shown last: command id -> item
    std::vector<MenuNode*> commands_;
    bool lcdPopupCommands_ = false;   // ids of the last popup belong to the VFD value popup
};

}  // namespace sq8l::gui
