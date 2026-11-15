#include "script/EmbeddedEngine.h"
#include "util/Url.h"
#include "quickjs.h"
#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ac::script {
namespace {
using html::Document;
using html::Node;

struct State {
    Document* document = nullptr;
    std::wstring baseUrl;
    Execution execution;
    unsigned ticks = 0;
};
int Interrupt(JSContext*, void* opaque) {
    auto* state = static_cast<State*>(opaque);
    return ++state->ticks > 40000 ? 1 : 0;
}
std::string StringArg(JSContext* context, JSValueConst value) {
    const char* chars = JS_ToCString(context, value);
    if (!chars) return {};
    std::string text(chars);
    JS_FreeCString(context, chars);
    return text;
}
Node* FindTag(Node* current, std::string_view tag) {
    if (!current) return nullptr;
    if (current->type == Node::Type::Element && current->tag == tag) return current;
    for (auto& child : current->children)
        if (Node* found = FindTag(child.get(), tag)) return found;
    return nullptr;
}
Node* FindId(Node* current, std::string_view id) {
    if (!current) return nullptr;
    if (current->type == Node::Type::Element && current->Attribute("id") == id) return current;
    for (auto& child : current->children)
        if (Node* found = FindId(child.get(), id)) return found;
    return nullptr;
}
Node* Element(State& state, std::string_view id) {
    return id == "@body" ? FindTag(state.document->root.get(), "body") :
                           FindId(state.document->root.get(), id);
}
void Text(Node& node, std::string& output) {
    if (node.type == Node::Type::Text) output += node.text;
    for (auto& child : node.children) Text(*child, output);
}
void Mutate(State& state, Node& target, std::string_view value,
            bool rawHtml, bool append) {
    if (!append) target.children.clear();
    if (!rawHtml) {
        auto child = std::make_unique<Node>();
        child->type = Node::Type::Text;
        child->text.assign(value);
        child->parent = &target;
        target.children.push_back(std::move(child));
    } else {
        auto fragment = html::Parse(value);
        state.document->embeddedCss += fragment.embeddedCss;
        if (fragment.root) {
            for (auto& child : fragment.root->children) {
                child->parent = &target;
                target.children.push_back(std::move(child));
            }
        }
    }
    ++state.execution.operations;
}
JSValue Exists(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    auto& state = *static_cast<State*>(JS_GetContextOpaque(context));
    const auto id = argc > 0 ? StringArg(context, argv[0]) : std::string{};
    return JS_NewBool(context, Element(state, id) != nullptr);
}
JSValue Read(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    auto& state = *static_cast<State*>(JS_GetContextOpaque(context));
    const auto id = argc > 0 ? StringArg(context, argv[0]) : std::string{};
    const auto property = argc > 1 ? StringArg(context, argv[1]) : std::string{};
    if (id == "@document" && property == "title")
        return JS_NewString(context, state.document->title.c_str());
    auto* node = Element(state, id);
    if (!node) return JS_UNDEFINED;
    if (property == "id") return JS_NewString(context, node->Attribute("id").c_str());
    if (property == "value") return JS_NewString(context, node->Attribute("value").c_str());
    if (property == "textContent" || property == "innerText" || property == "innerHTML") {
        std::string text;
        Text(*node, text);
        return JS_NewStringLen(context, text.data(), text.size());
    }
    return JS_UNDEFINED;
}
JSValue Write(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    auto& state = *static_cast<State*>(JS_GetContextOpaque(context));
    if (argc < 3) return JS_UNDEFINED;
    const auto id = StringArg(context, argv[0]);
    const auto property = StringArg(context, argv[1]);
    const auto value = StringArg(context, argv[2]);
    if (value.size() > 1024 * 1024) return JS_UNDEFINED;
    if (id == "@document" && property == "title") {
        state.document->title = value;
        ++state.execution.operations;
    } else if (Node* node = Element(state, id)) {
        if (property == "innerHTML" || property == "textContent" ||
            property == "innerText" || property == "write") {
            Mutate(state, *node, value, property == "innerHTML" || property == "write",
                   property == "write");
        } else if (property == "value") {
            node->attributes["value"] = value;
            ++state.execution.operations;
        }
    }
    return JS_UNDEFINED;
}
JSValue Navigate(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    auto& state = *static_cast<State*>(JS_GetContextOpaque(context));
    if (!argc) return JS_UNDEFINED;
    const auto resolved = ac::url::Resolve(state.baseUrl,
                                           ac::url::ToWide(StringArg(context, argv[0])));
    if (ac::url::IsWeb(resolved)) {
        state.execution.navigation = resolved;
        ++state.execution.operations;
    }
    return JS_UNDEFINED;
}
JSValue CurrentUrl(JSContext* context, JSValueConst, int, JSValueConst*) {
    auto& state = *static_cast<State*>(JS_GetContextOpaque(context));
    const auto url = ac::url::ToUtf8(state.baseUrl);
    return JS_NewStringLen(context, url.data(), url.size());
}
void Collect(const Node& node, std::wstring_view base,
             const std::unordered_map<std::string, std::string>& external,
             std::vector<std::string>& source, bool inert = false) {
    if (source.size() >= 64) return;
    if (node.tag == "template" || node.tag == "noscript") inert = true;
    if (!inert && node.tag == "script") {
        const auto type = node.Attribute("type");
        if (!type.empty() && type != "text/javascript" &&
            type != "application/javascript" && type != "module") {
            // Non-script tags include application/json and import maps.
        } else if (const auto src = node.Attribute("src"); !src.empty()) {
            const auto url = ac::url::Resolve(base, ac::url::ToWide(src));
            const auto found = external.find(ac::url::ToUtf8(url));
            if (found != external.end() && found->second.size() <= 4 * 1024 * 1024)
                source.push_back(found->second);
        } else {
            for (auto& child : node.children)
                if (child->type == Node::Type::Text && child->text.size() <= 4 * 1024 * 1024)
                    source.push_back(child->text);
        }
    }
    for (auto& child : node.children) Collect(*child, base, external, source, inert);
}
void Bind(JSContext* context, JSValue global, const char* name, JSCFunction* callback,
          int arguments) {
    JS_SetPropertyStr(context, global, name,
                      JS_NewCFunction(context, callback, name, arguments));
}

constexpr char kDomBootstrap[] = R"JAVASCRIPT(
(function () {
    function element(id) {
        if (!__acExists(id)) return null;
        return {
            get id() { return __acRead(id, "id"); },
            get innerHTML() { return __acRead(id, "innerHTML"); },
            set innerHTML(v) { __acWrite(id, "innerHTML", String(v)); },
            get textContent() { return __acRead(id, "textContent"); },
            set textContent(v) { __acWrite(id, "textContent", String(v)); },
            get innerText() { return __acRead(id, "innerText"); },
            set innerText(v) { __acWrite(id, "innerText", String(v)); },
            get value() { return __acRead(id, "value"); },
            set value(v) { __acWrite(id, "value", String(v)); }
        };
    }
    const locationObject = {
        get href() { return __acCurrentUrl(); },
        set href(v) { __acNavigate(String(v)); },
        assign(v) { __acNavigate(String(v)); },
        replace(v) { __acNavigate(String(v)); },
        toString() { return this.href; }
    };
    const documentObject = {
        get title() { return __acRead("@document", "title"); },
        set title(v) { __acWrite("@document", "title", String(v)); },
        get body() { return element("@body"); },
        getElementById(id) { return element(String(id)); },
        querySelector(s) {
            const selector = String(s);
            return selector.startsWith("#") ? element(selector.slice(1)) : null;
        },
        write(...parts) { __acWrite("@body", "write", parts.join("")); },
        get location() { return locationObject; },
        set location(v) { __acNavigate(String(v)); }
    };
    Object.defineProperty(globalThis, "document", { value: documentObject, configurable: false });
    Object.defineProperty(globalThis, "window", { value: globalThis, configurable: false });
    Object.defineProperty(globalThis, "location", {
        get() { return locationObject; },
        set(v) { __acNavigate(String(v)); },
        configurable: false
    });
})();
)JAVASCRIPT";
} // namespace

Execution ExecuteEmbedded(Document& document, std::wstring_view baseUrl,
    const std::unordered_map<std::string, std::string>& external) {
    State state;
    state.document = &document;
    state.baseUrl = std::wstring(baseUrl);
    if (!document.root) return state.execution;

    JSRuntime* runtime = JS_NewRuntime();
    if (!runtime) { ++state.execution.unsupported; return state.execution; }
    JS_SetMemoryLimit(runtime, 64 * 1024 * 1024);
    JS_SetMaxStackSize(runtime, 1024 * 1024);
    JS_SetInterruptHandler(runtime, Interrupt, &state);
    JSContext* context = JS_NewContext(runtime);
    if (!context) {
        JS_FreeRuntime(runtime);
        ++state.execution.unsupported;
        return state.execution;
    }
    JS_SetContextOpaque(context, &state);
    JSValue global = JS_GetGlobalObject(context);
    Bind(context, global, "__acExists", Exists, 1);
    Bind(context, global, "__acRead", Read, 2);
    Bind(context, global, "__acWrite", Write, 3);
    Bind(context, global, "__acNavigate", Navigate, 1);
    Bind(context, global, "__acCurrentUrl", CurrentUrl, 0);
    JS_FreeValue(context, global);
    JSValue setup = JS_Eval(context, kDomBootstrap, sizeof(kDomBootstrap) - 1,
                            "<another-chance-dom>", JS_EVAL_TYPE_GLOBAL);
    bool ready = !JS_IsException(setup);
    JS_FreeValue(context, setup);
    if (ready) {
        state.execution.engineReady = true;
        std::vector<std::string> scripts;
        Collect(*document.root, baseUrl, external, scripts);
        for (const auto& code : scripts) {
            if (state.ticks > 40000) break;
            ++state.execution.scripts;
            JSValue result = JS_Eval(context, code.data(), code.size(),
                                     "<document-script>", JS_EVAL_TYPE_GLOBAL);
            if (JS_IsException(result)) {
                ++state.execution.unsupported;
                JSValue exception = JS_GetException(context);
                JS_FreeValue(context, exception);
            }
            JS_FreeValue(context, result);
            // Process already-queued microtasks; network APIs are not yet exposed.
            for (unsigned jobs = 0; jobs < 128; ++jobs) {
                JSContext* jobContext = nullptr;
                if (JS_ExecutePendingJob(runtime, &jobContext) <= 0) break;
            }
            if (!state.execution.navigation.empty()) break;
        }
    } else {
        ++state.execution.unsupported;
        JSValue exception = JS_GetException(context);
        JS_FreeValue(context, exception);
    }
    JS_FreeContext(context);
    JS_FreeRuntime(runtime);
    return state.execution;
}
} // namespace ac::script
