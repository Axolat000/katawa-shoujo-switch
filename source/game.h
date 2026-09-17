#pragma once
#include "scene.h"
#include "text.h"
#include <deque>
#include <functional>
#include <map>
#include <set>

// ------------------------------------------------------------------ script

enum OpCode : uint8_t {
    OP_NOP,
    OP_LABEL,
    OP_SAY,
    OP_SHOW,
    OP_SCENE,
    OP_HIDE,
    OP_WITH,
    OP_PYTHON,
    OP_JUMP,
    OP_JIFNOT,
    OP_MENU,
    OP_CALL,
    OP_RETURN,
    OP_PLAY,
    OP_QUEUE,
    OP_STOP,
    OP_WINDOW,
    OP_NVL,
    OP_JUMPIN,
    OP_JUMPOUT,
    OP_PAUSE,
    OP_USER,
};

struct ImSpecOp {
    std::vector<std::string> name;
    int expr = -1;
    std::string tag;
    std::vector<int> atList;
    std::string layer = "master";
    int zorder = -1;
    std::vector<std::string> behind;
    bool valid = false;
};

struct MenuItemOp {
    std::string label;
    int cond = -1;
    int target = -1;
};

struct Op {
    uint8_t op = OP_NOP;
    std::string s1;         // label name / say what / channel / user line / window show flag holder
    int c1 = -1, c2 = -1, c3 = -1, c4 = -1; // code indices
    int target = -1;        // jump / jifnot target pc
    bool b1 = false, b2 = false;
    int loop = -1;          // play: -1 default
    ImSpecOp im;
    Value atl;              // atl.Block
    std::vector<std::pair<std::string, int>> params; // label params / call args (name or "" , code)
    std::vector<MenuItemOp> items;
    int endPc = -1;
    std::string file;
    int line = 0;
};

struct Script {
    std::string name;
    std::vector<Op> ops;
    std::map<std::string, int> labels;
};

struct Loc {
    int script = -1;
    int pc = -1;
    bool valid() const { return script >= 0 && pc >= 0; }
    bool operator==(const Loc &o) const { return script == o.script && pc == o.pc; }
};

// ------------------------------------------------------------------ control flow signals

struct JumpSignal : EngineSignal {
    std::string label;
    explicit JumpSignal(std::string l) : label(std::move(l)) {}
};
struct FullRestartSignal : EngineSignal {
    std::string target; // native screen to open after restart ("" main menu, "scene_select")
};
struct QuitSignal : EngineSignal {};
struct LoadSignal : EngineSignal {
    std::string slot;
};
struct RollbackSignal : EngineSignal {};
struct ReturnFromMenuSignal : EngineSignal {}; // leave a native menu back to the interaction

// ------------------------------------------------------------------ state snapshots

struct Snapshot {
    Loc loc;
    std::vector<Loc> returnStack;
    std::vector<std::vector<std::pair<std::string, Value>>> dynamicStack;
    std::vector<std::pair<std::string, Value>> store; // data variables (deep copied)
    SceneLists scene;
    std::map<std::string, std::vector<std::string>> music; // channel -> looping files
    std::map<std::string, double> channelVolumes;
    double playTime = 0;
    std::string saveName;
    std::string language;
};

// ------------------------------------------------------------------ interaction

struct SayState {
    DispP window;             // transient say window
    std::shared_ptr<TextDisp> what;
    int start = 0, end = -1;  // reveal range
    double startTime = 0;
    bool slow = true;
    bool slowDone = false;
    DispP ctc;
    std::string ctcPosition;
    double afmChars = 0;
};

struct MenuChoice {
    std::string label;
    Value value;
    bool chosenBefore = false;
    bool dead = false;
    bool active = false;
    // layout
    float x = 0, y = 0, w = 0, h = 0;
};

enum class IType { Say, Menu, Pause, With, Movie, Screen };

struct Interaction {
    IType type = IType::Pause;
    bool clickable = true;   // saybehavior
    bool hard = false;
    double delay = -1;       // pause behavior
    bool transPause = false; // with statement
    bool suppressOverlay = false;
    bool suppressWindow = false;
    std::shared_ptr<SayState> say;
    std::vector<MenuChoice> choices;
    int focused = -1;
    bool menuShuffled = false;
    std::function<bool()> update; // custom per-frame hook, true = done
    std::function<void()> draw;   // custom overlay drawing (virtual coords)
    Value result;
    bool done = false;
    bool rollForward = false;
};

// ------------------------------------------------------------------ game

struct Prefs {
    bool fullscreen = false;
    bool skipUnseen = false;
    bool skipAfterChoices = false;
    int textCps = 70;       // 0 = instant
    int afmTime = 0;        // 0 = off, else _preferences.afm_time
    double musicVolume = 1, sfxVolume = 1;
    bool transitions = true;
    bool muted = false;
};

class Game {
  public:
    // data
    std::vector<Script> scripts; // 0 = base, 1 = current language
    std::map<std::string, Loc> labels;
    std::string scriptLanguage;
    ValueFile data;
    Value storeV;
    InstObj *store = nullptr;
    Value persistentV, configV, preferencesV;
    Value languagesV; // per-language data
    std::vector<std::string> availableLanguages;
    std::set<std::string> dataVars; // store names holding plain data

    // execution context
    Loc next;
    Loc current;
    std::vector<Loc> returnStack;
    std::vector<std::vector<std::pair<std::string, Value>>> dynamicStack;
    SceneLists scene;
    bool inMainMenuContext = false;
    bool running = true;

    // interaction
    std::shared_ptr<RootDisp> oldScene;
    Value pendingTransition; // renpy.transition()
    Interaction *cur = nullptr;
    int interactDepth = 0;
    double now = 0;
    std::string skipping; // "", "slow", "fast"
    bool skipHeld = false;
    bool afterRollback = false;
    Prefs prefs;
    std::deque<Snapshot> rollbackLog;
    Snapshot lastCheckpoint;
    bool haveCheckpoint = false;
    bool rollbackBlocked = false;
    double playTime = 0;
    std::set<uint64_t> seenOps; // (script lang hash, pc) seen dialogue
    bool quitRequested = false;
    bool gameMenuRequested = false;
    std::string pendingNativeScreen;

    bool init();
    void run(); // main loop (never returns until quit)

    // script
    bool loadScript(const std::string &path, const std::string &name);
    void setScriptLanguage(const std::string &lang);
    bool hasLabel(const std::string &name) const;
    void jumpTo(const std::string &label);
    void callLabel(const std::string &label, const std::vector<Value> &args,
                   const std::vector<std::pair<std::string, Value>> &kwargs);
    void executeUntilInteraction();
    void execOp(const Op &op, Loc loc);
    const Op *opAt(const Loc &l) const;
    Value evalCode(int code);
    void execCode(int code, bool hide = false);

    // displayables & scene
    void show(const ImSpecOp &im, const Value &atl, const std::string &layerOverride = "");
    void showName(const std::string &name, const std::vector<Value> &atList, const std::string &layer,
                  const std::string &tag, int zorder, const std::vector<std::string> &behind, DispP what,
                  const Value &atl);
    void hide(const std::string &tag, const std::string &layer);
    void sceneClear(const std::string &layer);

    // interaction core
    Value interact(Interaction &in);
    bool withStatement(const Value &trans, bool clear = true);
    void withNone();
    void showWindowIfNeeded();
    void checkpoint();
    void frame(bool drawOnly = false);
    void render();

    // say / menu / nvl
    void say(const Value &who, const std::string &what, bool interact);
    void characterCall(const Value &ch, const std::string &what, bool interact, const CallArgs *extra);
    Value menu(const std::vector<std::pair<std::string, Value>> &items, const Value &setExpr);
    DispP buildSayWindow(const Value &ch, const std::string &who, const std::string &what, std::shared_ptr<TextDisp> &whatText);
    DispP buildNvlWindow(std::shared_ptr<TextDisp> &lastWhat);
    void nvlShow(const Value &trans, bool hide);

    // rollback / saves
    Snapshot capture();
    void restore(const Snapshot &s);
    void rollback();
    bool saveSlot(const std::string &slot, const std::string &extraInfo);
    bool loadSlot(const std::string &slot);
    void savePersistent();
    void loadPersistent();

    // helpers
    Value storeGet(const std::string &name) { return store->get(name); }
    void storeSet(const std::string &name, const Value &v) { store->attrs[name] = v; }
    Value persistentGet(const std::string &name);
    void persistentSet(const std::string &name, const Value &v);
    std::string lang() const;
    void switchLanguage(const std::string &lang);
    Value displayString(const std::string &key);
    std::string ds(const std::string &key) { return valueStr(displayString(key)); }

    // native screens
    void mainMenu();
    void gameMenu(const std::string &screen = "");
    void startGame();
    void fullRestart(const std::string &target);
};

extern Game G;

void registerGameNatives(Interp &I);
void registerTransformNatives(Interp &I);
void registerTransitionNatives(Interp &I);
void setStyleTable(const Value &styles);
