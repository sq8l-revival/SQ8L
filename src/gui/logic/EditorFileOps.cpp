// FILE menu, SEND/REQ and the MIDI port selection of TplugEditForm (unit plugEdit
// 0x48532c..0x486e8c).
#include <cstring>

#include "Dialogs.h"
#include "EditorController.h"

namespace sq8l::gui {

namespace {

const char kLibFilter[] = "Library/Backup data (*.8XL; *.dat)|*.8XL;*.dat";
const char kSyxFilter[] = "ESQ-1/SQ-80 SysEx (*.SYX)|*.SYX";

// SQ-80/ESQ-1 SysEx messages of the SEND/REQ functions (DAT_004c3074 / 004c307c / 004c3084)
const std::vector<uint8_t> kSysexPre = {0xF0, 0x0F, 0x02, 0x00, 0x0E, 0x26, 0xF7};
const std::vector<uint8_t> kSysexPost = {0xF0, 0x0F, 0x02, 0x00, 0x0E, 0x2F, 0x62, 0xF7};
const std::vector<uint8_t> kSysexRequest = {0xF0, 0x0F, 0x02, 0x00, 0x09, 0xF7};

// FUN_0041783c: characters not allowed in file names
std::string sanitizeFileName(const std::string& s) {
    std::string r = s;
    for (char& c : r) {
        switch (c) {
        case '<': c = '['; break;
        case '>': c = ']'; break;
        case '"': c = '\''; break;
        case '*':
        case '?': c = '~'; break;
        case '/':
        case ':':
        case '\\':
        case '|': c = '-'; break;
        default: break;
        }
    }
    return r;
}

// TOpenDialog.SetInitialDir (VCL): a trailing '\' is removed unless the path is a drive root
std::string initialDir(const std::string& dir) {
    size_t n = dir.size();
    if (n > 1 && dir[n - 1] == '\\' && dir[n - 2] != ':') n--;
    return dir.substr(0, n);
}

// FUN_00417798: directory + '\' + file name
std::string joinPath(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    return dir.back() == '\\' ? dir + name : dir + "\\" + name;
}

}  // namespace

// TOpenDialog / TSaveDialog.Execute: the dialog object keeps its FileName (set by the program
// export, or the file chosen last); cancelling leaves it unchanged.
bool EditorController::fileDialog(FileDialogRequest& rq, std::string& path) {
    std::string& last = rq.save ? saveFileName_ : openFileName_;
    if (rq.fileName.empty())
        rq.fileName = last;
    else
        last = rq.fileName;
    if (!ui_.fileDialog(rq, path)) return false;
    last = path;
    return true;
}

// ==================================================================== library

void EditorController::loadLibrary() {  // menu_loadLibClick
    FileDialogRequest rq;
    rq.initialDir = initialDir(libDir_);
    rq.filter = kLibFilter;
    rq.defaultExt = "8XL";
    std::string path;
    if (!fileDialog(rq, path)) return;
    std::vector<uint8_t> data;
    if (!ui_.readFile(path, data) || data.empty()) return;
    if (host_.settings().confirmLoading() &&
        ui_.messageBox("Load library? (This will erase all programs!)", "Warning", kMbOkCancel) != kIdOk)
        return;
    if (host_.library().loadLibrary(data.data(), data.size()))
        showMessage("** Library loaded **");  // (FUN_004843c8: empty)
    else
        ui_.messageBox("Could not load file.", "Error", kMbOk);
}

void EditorController::saveLibrary() {  // menu_saveLibClick
    FileDialogRequest rq;
    rq.save = true;
    rq.initialDir = initialDir(libDir_);
    rq.filter = kLibFilter;
    rq.defaultExt = "8XL";
    std::string path;
    if (!fileDialog(rq, path)) return;
    const std::vector<uint8_t> data = host_.library().saveLibrary();
    if (ui_.writeFile(path, data))
        showMessage("** Library saved **");
    else
        ui_.messageBox("Could not save file.", "Error", kMbOk);
}

void EditorController::initLibrary() {  // menu_initLibClick
    if (ui_.messageBox("Init library? (This will erase all programs!)", "Warning", kMbOkCancel) != kIdOk) return;
    host_.library().initLibrary(false);
    showMessage("** Library initialized **");
}

// ==================================================================== bank

void EditorController::loadBank() {  // menu_loadBankClick
    FileDialogRequest rq;
    rq.initialDir = initialDir(libDir_);
    rq.filter = kLibFilter;
    rq.defaultExt = "8XL";
    if (SoundLibrary::isBankWriteProtected(bank())) {
        ui_.messageBox("Bank is write protected.", "Error", kMbOk);
        return;
    }
    std::string path;
    if (!fileDialog(rq, path)) return;
    std::vector<uint8_t> data;
    if (!ui_.readFile(path, data) || data.empty()) return;
    const std::string msg = std::string("Load bank? (This will erase all programs in bank ") +
                            char(bank() >= 0 && bank() < 4 ? 'A' + bank() : '?') + " !)";
    if (host_.settings().confirmLoading() && ui_.messageBox(msg, "Warning", kMbOkCancel) != kIdOk) return;
    if (host_.library().loadBank(data.data(), data.size(), bank()))
        showMessage("** Bank loaded **");
    else
        ui_.messageBox("Could not load file.", "Error", kMbOk);
}

void EditorController::saveBank() {  // menu_saveBankClick
    FileDialogRequest rq;
    rq.save = true;
    rq.initialDir = initialDir(libDir_);
    rq.filter = kLibFilter;
    rq.defaultExt = "8XL";
    std::string path;
    if (!fileDialog(rq, path)) return;
    const std::vector<uint8_t> data = host_.library().saveBank(bank());
    if (ui_.writeFile(path, data))
        showMessage("** Bank saved **");
    else
        ui_.messageBox("Could not save file.", "Error", kMbOk);
}

void EditorController::initBank() {  // menu_initBankClick
    if (SoundLibrary::isBankWriteProtected(bank())) {
        ui_.messageBox("Bank is write protected.", "Error", kMbOk);
        return;
    }
    const std::string msg = std::string("Init bank? (This will erase all programs in bank ") +
                            char(bank() >= 0 && bank() < 4 ? 'A' + bank() : '?') + " !)";
    if (ui_.messageBox(msg, "Warning", kMbOkCancel) != kIdOk) return;
    host_.library().initBank(bank());
    showMessage("** Bank initialized **");
}

// ==================================================================== SysEx files

void EditorController::importBank() {  // menu_impBankClick
    FileDialogRequest rq;
    rq.initialDir = initialDir(sysexDir_);
    rq.filter = kSyxFilter;
    rq.defaultExt = "SYX";
    std::string path;
    if (!fileDialog(rq, path)) return;
    std::vector<uint8_t> data;
    if (!ui_.readFile(path, data) || data.empty()) return;
    int b = bank();
    if (SoundLibrary::isBankWriteProtected(b)) b = 0;
    int p = prog();
    selectProgramDialog("Select destination...", "", "", b, p);
    if (SoundLibrary::isBankWriteProtected(b)) {
        ui_.messageBox("Bank is write protected.", "Error", kMbOk);
        return;
    }
    if (p < 0 || p >= 0x80) return;
    int last = p + 0x27;
    if (last >= 0x80) last = 0x7f;
    const std::string msg =
        "Overwrite programs " + delphi::intToStrZ(p, 3) + "..." + delphi::intToStrZ(last, 3) + "?";
    if (host_.settings().confirmLoading() && ui_.messageBox(msg, "Warning", kMbOkCancel) != kIdOk) return;
    bool ok = false;
    const int idx = host_.editBuffer().libraryIndex(p, b);
    if (host_.library().importSysexBank(data.data(), data.size(), idx, true)) {
        ok = true;
    } else if (ui_.messageBox("This file could not be identified as ESQ-1/SQ-80 SysEx. Try to load anyway?",
                              "Warning", kMbYesNo) == kIdYes) {
        if (host_.library().importSysexBank(data.data(), data.size(), host_.editBuffer().libraryIndex(p, b), false))
            ok = true;
        else
            ui_.messageBox("Could not import file.", "Error", kMbOk);
    }
    if (ok) showMessage("** SysEx bank imported **");
}

void EditorController::exportBank() {  // menu_expBankClick
    FileDialogRequest rq;
    rq.save = true;
    rq.initialDir = initialDir(sysexDir_);
    rq.filter = kSyxFilter;
    rq.defaultExt = "SYX";
    int b = bank(), p = prog();
    selectProgramDialog("Select source...", "", "", b, p);
    if (p < 0 || p >= 0x80) return;
    const int last = p + 0x27;
    if (last >= 0x80) {
        ui_.messageBox("Not enough programs (40 needed) at this position!", "Error", kMbOk);
        return;
    }
    bool ok = false;
    const std::vector<uint8_t> data = host_.library().exportSysexBank(host_.editBuffer().libraryIndex(p, b));
    if (!data.empty()) {
        std::string path;
        if (!fileDialog(rq, path)) return;
        if (ui_.writeFile(path, data)) ok = true;
    }
    if (!ok) {
        ui_.messageBox("Error while exporting.", "Error", kMbOk);
        return;
    }
    showMessage2("** SysEx bank exported **",
                 "Programs " + delphi::intToStrZ(p, 3) + "..." + delphi::intToStrZ(last, 3), 0x2a);
}

void EditorController::importProgram() {  // menu_impSingleClick
    FileDialogRequest rq;
    rq.initialDir = initialDir(sysexDir_);
    rq.filter = kSyxFilter;
    rq.defaultExt = "SYX";
    std::string path;
    if (!fileDialog(rq, path)) return;
    std::vector<uint8_t> data;
    if (!ui_.readFile(path, data) || data.empty()) return;
    if (ui_.messageBox("Overwrite edit program?", "Warning", kMbOkCancel) != kIdOk) return;
    bool ok = false;
    EditBuffer& eb = host_.editBuffer();
    if (eb.importSysex(data.data(), data.size(), true)) {
        ok = true;
    } else if (ui_.messageBox("This file could not be identified as ESQ-1/SQ-80 SysEx. Try to load anyway?",
                              "Warning", kMbYesNo) == kIdYes) {
        // the original retries with the same header check
        if (eb.importSysex(data.data(), data.size(), true))
            ok = true;
        else
            ui_.messageBox("Could not import file.", "Error", kMbOk);
    }
    refreshAll();
    if (ok) showMessage("** SysEx program imported **");
}

void EditorController::exportProgram() {  // menu_expSingleClick
    FileDialogRequest rq;
    rq.save = true;
    rq.initialDir = initialDir(sysexDir_);
    rq.filter = kSyxFilter;
    rq.defaultExt = "SYX";
    bool ok = false;
    const std::vector<uint8_t> data = host_.editBuffer().exportSysex();
    if (!data.empty()) {
        rq.fileName = joinPath(rq.initialDir, sanitizeFileName(host_.editBuffer().name(0)));
        std::string path;
        if (!fileDialog(rq, path)) return;
        if (ui_.writeFile(path, data)) ok = true;
    }
    if (ok)
        showMessage("** Edit program exported **");
    else
        ui_.messageBox("Error while exporting.", "Error", kMbOk);
}

// ==================================================================== SEND / REQ

int EditorController::selectMidiPort(const std::string& title, bool output, int current) {  // FUN_0047d5f4
    MidiSelDialog dlg(title, output, current, output ? host_.midiOutPorts() : host_.midiInPorts());
    keyCapture_ = keyCaptMode_ >= 0;
    dlg.showModal(ui_);
    keyCapture_ = false;
    return dlg.result();
}

bool EditorController::sendSysex(int outPort) {  // FUN_004869b0
    host_.midiCloseAll();
    if (!host_.midiOpenOut(outPort)) return false;
    showMessage("** Sending SysEx dump **", 0x23);
    host_.midiSendOut(kSysexPre);
    host_.processMessagesFor(200);
    pump();
    const std::vector<uint8_t> data = host_.editBuffer().exportSysex();
    if (data.empty()) return false;
    host_.midiSendOut(data);
    host_.processMessagesFor(200);
    pump();
    host_.midiSendOut(kSysexPost);
    showMessage("** SysEx dump sent **", 0x23);
    return true;
}

bool EditorController::requestSysex(int outPort, int inPort) {  // FUN_00486bb0
    host_.midiCloseAll();
    if (!host_.midiOpenIn(inPort)) return false;
    sysexReq_ = 1;
    showMessage("** Waiting for SysEx dump **");
    if (host_.midiOpenOut(outPort)) {
        host_.midiSendOut(kSysexPre);
        host_.processMessagesFor(200);
        pump();
        host_.midiSendOut(kSysexRequest);
    }
    return true;
}

void EditorController::requestProgram() {  // menu_reqSingleClick
    int port = selectMidiPort("Select MIDI in port...", false, midiIn_);
    if (port < 0) return;
    midiIn_ = port;
    port = selectMidiPort("Select MIDI out port...", true, midiOut_);
    if (port < 0) return;
    midiOut_ = port;
    requestSysex(port, midiIn_);
}

void EditorController::receiveProgram() {  // menu_recSingleClick
    const int port = selectMidiPort("Select MIDI in port...", false, midiIn_);
    if (port < 0) return;
    midiIn_ = port;
    // 0x486b14: open the input and wait (no request sent)
    host_.midiCloseAll();
    if (!host_.midiOpenIn(port)) {
        sysexReq_ = 0;
        return;
    }
    sysexReq_ = 1;
    showMessage("** Waiting for SysEx dump **");
}

void EditorController::sendProgram() {  // menu_sendSingleClick
    const int port = selectMidiPort("Select MIDI out port...", true, midiOut_);
    if (port < 0) return;
    midiOut_ = port;
    sendSysex(port);
}

void EditorController::sysexSendButton() {  // sysExSendButtonClick
    if (midiOut_ >= 0)
        sendSysex(midiOut_);
    else
        sendProgram();
}

void EditorController::sysexReqButton() {  // sysExReqButtonClick
    if (midiIn_ < 0) {
        const int port = selectMidiPort("Select MIDI in port...", false, midiIn_);
        if (port < 0) return;
        midiIn_ = port;
    }
    if (midiOut_ < 0) {
        const int port = selectMidiPort("Select MIDI out port...", true, midiOut_);
        if (port < 0) return;
        midiOut_ = port;
    }
    requestSysex(midiOut_, midiIn_);
}

void EditorController::sysexReceived(const uint8_t* data, size_t size) {  // FUN_0048427c
    if (data && sysexReq_ == 1) {
        host_.editBuffer().importSysex(data, size, false);
        post(kMsgSysexReceived);
    }
    sysexReq_ = 0;
}

}  // namespace sq8l::gui
