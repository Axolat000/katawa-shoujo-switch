#include "scene.h"
#include <algorithm>

double g_frameTime = 0;
double g_interactTime = 0;
const std::vector<std::string> kLayers = {"master", "transient", "screens", "overlay"};

RenderP renderChild(const DispP &d, float w, float h, double st, double at);

RenderP LayerDisp::render(float w, float h, double, double) {
    auto rv = Render::make(w, h);
    for (auto &e : entries) {
        if (e.showTime < 0)
            e.showTime = g_interactTime;
        if (e.animTime < 0)
            e.animTime = g_interactTime;
        RenderP surf = renderChild(e.d, w, h, g_frameTime - e.showTime, g_frameTime - e.animTime);
        e.d->place(*rv, 0, 0, w, h, surf);
    }
    return rv;
}

RenderP RootDisp::render(float w, float h, double st, double at) {
    auto rv = Render::make(w, h);
    for (auto &l : layers) {
        RenderP surf = renderChild(l.second, w, h, st, at);
        l.second->place(*rv, 0, 0, w, h, surf);
    }
    return rv;
}

RenderP AdjustTimesDisp::render(float w, float h, double, double) {
    double st = showTime < 0 ? 0 : g_frameTime - showTime;
    double at = animTime < 0 ? 0 : g_frameTime - animTime;
    return renderChild(child, w, h, st, at);
}

SceneLists::SceneLists() {
    for (auto &l : kLayers) {
        layers[l];
        atLists[l];
    }
}

static int findAddIndex(const std::vector<SceneEntry> &l, const std::string &tag, int zorder,
                        const std::vector<std::string> &behind, int &removeIndex) {
    int addIndex = -1;
    removeIndex = -1;
    for (int i = 0; i < (int)l.size(); i++) {
        const SceneEntry &e = l[i];
        if (addIndex < 0) {
            if (e.zorder == zorder) {
                if (!e.tag.empty() &&
                    (e.tag == tag || std::find(behind.begin(), behind.end(), e.tag) != behind.end()))
                    addIndex = i;
            } else if (e.zorder > zorder) {
                addIndex = i;
            }
        }
        if (removeIndex < 0 && !e.tag.empty() && e.tag == tag)
            removeIndex = i;
    }
    if (addIndex < 0)
        addIndex = (int)l.size();
    return addIndex;
}

// Transform._change_transform_child: copy the running transform chain, replacing the innermost child.
static DispP changeTransformChild(const DispP &old, const DispP &newChild) {
    if (!old || !old->isTransform())
        return newChild;
    auto o = std::static_pointer_cast<TransformDisp>(old);
    auto t = std::make_shared<TransformDisp>(*o);
    t->proxy = Value();
    t->child = o->child ? changeTransformChild(o->child, newChild) : newChild;
    t->active = false;
    return t;
}

// SceneLists.transform_state
static DispP transformState(const DispP &old, const DispP &thing) {
    if (!old || !old->isTransform())
        return thing;
    auto o = std::static_pointer_cast<TransformDisp>(old);
    TransformP nt;
    if (thing->isTransform()) {
        nt = std::static_pointer_cast<TransformDisp>(thing);
    } else {
        nt = std::make_shared<TransformDisp>();
        nt->kind = "Transform";
        nt->child = thing;
    }
    nt->state.takeState(o->state);
    return nt;
}

void SceneLists::add(const std::string &layer, DispP thing, const std::string &key, int zorder,
                     const std::vector<std::string> &behind, const std::vector<Value> &atList, const std::string &name,
                     bool hasAtl) {
    auto &l = layers[layer];
    if (!key.empty()) {
        std::string ht = "hide$" + key, rt = "replaced$" + key;
        l.erase(std::remove_if(l.begin(), l.end(), [&](const SceneEntry &e) { return e.tag == ht || e.tag == rt; }),
                l.end());
        atLists[layer][key] = atList;
    }
    int removeIndex;
    int addIndex = findAddIndex(l, key, zorder, behind, removeIndex);
    double at = -1;
    if (removeIndex >= 0) {
        const SceneEntry &old = l[removeIndex];
        at = old.animTime;
        if (!hasAtl && atList.empty() && old.d && old.d->isTransform())
            thing = changeTransformChild(old.d, thing);
        else
            thing = transformState(old.d, thing);
    }
    SceneEntry e;
    e.tag = key;
    e.zorder = zorder;
    e.showTime = -1;
    e.animTime = at;
    e.d = thing;
    e.name = name;
    e.atList = atList;
    l.insert(l.begin() + addIndex, e);
    if (removeIndex >= 0) {
        if (addIndex <= removeIndex)
            removeIndex++;
        l.erase(l.begin() + removeIndex);
    }
}

void SceneLists::remove(const std::string &layer, const std::string &tag) {
    auto &l = layers[layer];
    for (size_t i = 0; i < l.size(); i++)
        if (!l[i].tag.empty() && l[i].tag == tag) {
            atLists[layer].erase(tag);
            l.erase(l.begin() + i);
            return;
        }
}

void SceneLists::clear(const std::string &layer) {
    layers[layer].clear();
    atLists[layer].clear();
}

bool SceneLists::showing(const std::string &layer, const std::string &tag) const {
    return entryByTag(layer, tag) != nullptr;
}

const SceneEntry *SceneLists::entryByTag(const std::string &layer, const std::string &tag) const {
    auto it = layers.find(layer);
    if (it == layers.end())
        return nullptr;
    for (auto &e : it->second)
        if (e.tag == tag)
            return &e;
    return nullptr;
}

LayerP SceneLists::makeLayer(const std::string &layer) const {
    auto d = std::make_shared<LayerDisp>();
    d->kind = "Layer";
    d->layer = layer;
    auto it = layers.find(layer);
    if (it != layers.end())
        d->entries = it->second;
    return d;
}

std::shared_ptr<RootDisp> SceneLists::computeScene() const {
    auto root = std::make_shared<RootDisp>();
    root->kind = "Root";
    for (auto &l : kLayers)
        root->layers.emplace_back(l, makeLayer(l));
    return root;
}

void SceneLists::setTimes(double t) {
    for (auto &kv : layers)
        for (auto &e : kv.second) {
            if (e.showTime < 0)
                e.showTime = t;
            if (e.animTime < 0)
                e.animTime = t;
        }
}

void SceneLists::removeHidden() {}
