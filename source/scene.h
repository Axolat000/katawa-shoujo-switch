#pragma once
#include "disp.h"
#include <map>

extern double g_frameTime;    // time of the frame being rendered
extern double g_interactTime; // start time of the current interaction

struct SceneEntry {
    std::string tag; // "" for untagged; "hide$tag" for hiding entries
    int zorder = 0;
    double showTime = -1, animTime = -1; // -1: set at first render
    DispP d;
    std::string name; // full image name
    std::vector<Value> atList;
};

struct LayerDisp : Displayable {
    std::string layer;
    std::vector<SceneEntry> entries;
    RenderP render(float w, float h, double st, double at) override;
    void children(std::vector<DispP> &out) override {
        for (auto &e : entries)
            out.push_back(e.d);
    }
};
using LayerP = std::shared_ptr<LayerDisp>;

struct RootDisp : Displayable {
    std::vector<std::pair<std::string, DispP>> layers; // in drawing order
    RenderP render(float w, float h, double st, double at) override;
    DispP layer(const std::string &name) const {
        for (auto &l : layers)
            if (l.first == name)
                return l.second;
        return nullptr;
    }
    void children(std::vector<DispP> &out) override {
        for (auto &l : layers)
            out.push_back(l.second);
    }
};

// Wraps a displayable with fixed show/animation times (renpy AdjustTimes).
struct AdjustTimesDisp : Displayable {
    DispP child;
    double showTime, animTime;
    RenderP render(float w, float h, double st, double at) override;
    Placement placement() override { return child ? child->placement() : Placement(); }
    void children(std::vector<DispP> &out) override { out.push_back(child); }
};

struct SceneLists {
    std::map<std::string, std::vector<SceneEntry>> layers;
    std::map<std::string, std::map<std::string, std::vector<Value>>> atLists;
    bool shownWindow = false;

    SceneLists();
    void add(const std::string &layer, DispP thing, const std::string &key, int zorder,
             const std::vector<std::string> &behind, const std::vector<Value> &atList, const std::string &name,
             bool hasAtl);
    void remove(const std::string &layer, const std::string &tag);
    void clear(const std::string &layer);
    bool showing(const std::string &layer, const std::string &tag) const;
    const SceneEntry *entryByTag(const std::string &layer, const std::string &tag) const;
    LayerP makeLayer(const std::string &layer) const;
    std::shared_ptr<RootDisp> computeScene() const;
    void setTimes(double t);
    void removeHidden();
};

extern const std::vector<std::string> kLayers; // master, transient, screens, overlay
