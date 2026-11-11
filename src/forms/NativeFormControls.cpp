#include "forms/NativeFormControls.h"
#include "util/Url.h"
#include <commctrl.h>
#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace ac::forms {
namespace {
using Kind = layout::PageLayout::Control::Kind;
constexpr unsigned kControlIdBase=7000;
std::wstring TextOf(const html::Node& node) {
    if (node.type == html::Node::Type::Text) return url::ToWide(node.text);
    std::wstring out;
    for (const auto& child : node.children) out += TextOf(*child);
    return out;
}
std::wstring Escape(std::wstring_view input) {
    constexpr wchar_t hex[]=L"0123456789ABCDEF";
    std::wstring output;
    for (unsigned char value : url::ToUtf8(input)) {
        if ((value>='a'&&value<='z') || (value>='A'&&value<='Z') ||
            (value>='0'&&value<='9') || value=='-' || value=='_' ||
            value=='.' || value=='~') output+=static_cast<wchar_t>(value);
        else if (value==' ') output+=L'+';
        else {
            output+=L'%';
            output+=hex[value>>4];
            output+=hex[value&0xF];
        }
    }
    return output;
}
std::wstring Read(HWND widget) {
    if (!widget) return {};
    const int length=GetWindowTextLengthW(widget);
    if (length<0 || length>32768) return {};
    std::wstring value(static_cast<size_t>(length)+1,L'\0');
    GetWindowTextW(widget,value.data(),static_cast<int>(value.size()));
    value.resize(static_cast<size_t>(length));
    return value;
}
std::wstring DefaultValue(const html::Node& node,Kind kind) {
    if (kind==Kind::Textarea) return TextOf(node);
    if (kind==Kind::Submit || kind==Kind::Button || kind==Kind::Reset) {
        const std::string label=node.Attribute("value");
        if (!label.empty()) return url::ToWide(label);
        return kind==Kind::Reset ? L"Restablecer" : L"Enviar";
    }
    return url::ToWide(node.Attribute("value"));
}
const html::Node* FindForm(const html::Node* node) {
    while (node) {
        if (node->tag=="form") return node;
        node=node->parent;
    }
    return nullptr;
}
bool IsField(const html::Node& node) {
    return node.tag=="input" || node.tag=="textarea" || node.tag=="select";
}
void CollectNamed(const html::Node& node,
                  std::vector<const html::Node*>& out) {
    if (IsField(node)) out.push_back(&node);
    for (const auto& child: node.children) CollectNamed(*child,out);
}
} // namespace

void NativeFormControls::Clear() {
    for (const auto& field:fields_) {
        if (!field.hwnd || !IsWindow(field.hwnd)) continue;
        RemoveWindowSubclass(field.hwnd,&NativeFormControls::TextProc,1);
        DestroyWindow(field.hwnd);
    }
    fields_.clear();
    parent_=nullptr;
}

LRESULT CALLBACK NativeFormControls::TextProc(HWND hwnd,UINT message,
    WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR context) {
    auto* self=reinterpret_cast<NativeFormControls*>(context);
    if (message==WM_KEYDOWN && w==VK_RETURN && self && self->parent_) {
        PostMessageW(self->parent_,kSubmitByEnter,reinterpret_cast<WPARAM>(hwnd),0);
        return 0;
    }
    return DefSubclassProc(hwnd,message,w,l);
}

const NativeFormControls::Field* NativeFormControls::Find(HWND hwnd) const {
    for (const auto& field:fields_) if (field.hwnd==hwnd) return &field;
    return nullptr;
}
const html::Node* NativeFormControls::ParentForm(const html::Node* node) {
    return FindForm(node);
}
bool NativeFormControls::Owns(HWND hwnd) const {
    return Find(hwnd)!=nullptr;
}
bool NativeFormControls::IsSubmit(HWND hwnd) const {
    const auto* field=Find(hwnd);
    return field && field->kind==Kind::Submit;
}
bool NativeFormControls::IsReset(HWND hwnd) const {
    const auto* field=Find(hwnd);
    return field && field->kind==Kind::Reset;
}
std::wstring NativeFormControls::Value(const Field& field) const {
    if (field.kind==Kind::Checkbox || field.kind==Kind::Radio) {
        return field.node->Attribute("value").empty() ? L"on" :
            url::ToWide(field.node->Attribute("value"));
    }
    if (field.kind==Kind::Select) {
        const LRESULT index=SendMessageW(field.hwnd,CB_GETCURSEL,0,0);
        if (index==CB_ERR) return {};
        const auto idx=static_cast<size_t>(index);
        size_t position=0;
        for (const auto& option:field.node->children) {
            if (option->tag!="option") continue;
            if (position++==idx) {
                const auto raw=option->Attribute("value");
                return raw.empty() ? TextOf(*option) : url::ToWide(raw);
            }
        }
        return {};
    }
    return Read(field.hwnd);
}
void NativeFormControls::Sync(HWND parent,HINSTANCE instance,
    const std::vector<layout::PageLayout::Control>& layout,
    const RECT& viewport,int scroll,HFONT font) {
    if (parent_!=parent) Clear();
    parent_=parent;
    // Remove widgets whose nodes no longer appear after CSS layout changes.
    for (size_t index=0; index<fields_.size();) {
        const auto present=std::find_if(layout.begin(),layout.end(),
            [&](const auto& item){return item.node==fields_[index].node;});
        if (present==layout.end()) {
            if (IsWindow(fields_[index].hwnd)) DestroyWindow(fields_[index].hwnd);
            fields_.erase(fields_.begin()+static_cast<std::ptrdiff_t>(index));
        } else ++index;
    }
    size_t created=fields_.size();
    for (const auto& item:layout) {
        if (created>=256) break;
        const auto existing=std::find_if(fields_.begin(),fields_.end(),
            [&](const Field& field){return field.node==item.node;});
        if (existing!=fields_.end()) {
            existing->bounds=item.rect;
            continue;
        }
        const auto kind=item.kind;
        const bool button=kind==Kind::Submit||kind==Kind::Reset||
                          kind==Kind::Button||kind==Kind::Checkbox||kind==Kind::Radio;
        const bool select=kind==Kind::Select;
        const wchar_t* cls=button ? L"BUTTON" : select ? L"COMBOBOX" : L"EDIT";
        DWORD style=WS_CHILD|WS_VISIBLE|WS_TABSTOP;
        if (button) {
            if (kind==Kind::Checkbox) style|=BS_AUTOCHECKBOX;
            else if(kind==Kind::Radio) style|=BS_AUTORADIOBUTTON;
            else style|=BS_PUSHBUTTON;
        } else if(select) {
            style|=CBS_DROPDOWNLIST|WS_VSCROLL;
        } else if(kind==Kind::Textarea) {
            style|=ES_MULTILINE|ES_AUTOVSCROLL|WS_VSCROLL;
        } else {
            style|=ES_AUTOHSCROLL;
            if(kind==Kind::Password) style|=ES_PASSWORD;
        }
        const auto text=DefaultValue(*item.node,kind);
        HWND widget=CreateWindowExW(button?0:WS_EX_CLIENTEDGE,cls,
            text.c_str(),style,item.rect.left,item.rect.top,
            std::max(1L,item.rect.right-item.rect.left),
            std::max(1L,item.rect.bottom-item.rect.top),
            parent,reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kControlIdBase+created)),
            instance,nullptr);
        if (!widget) continue;
        if (font) SendMessageW(widget,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
        if (kind==Kind::Checkbox||kind==Kind::Radio) {
            const bool checked=item.node->attributes.contains("checked");
            SendMessageW(widget,BM_SETCHECK,checked?BST_CHECKED:BST_UNCHECKED,0);
        } else if(kind==Kind::Select) {
            int selected=0, index=0;
            for(const auto& option:item.node->children) {
                if(option->tag!="option") continue;
                const auto label=TextOf(*option);
                SendMessageW(widget,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
                if(option->attributes.contains("selected")) selected=index;
                ++index;
            }
            if(index>0) SendMessageW(widget,CB_SETCURSEL,selected,0);
        } else if(!button && kind!=Kind::Textarea) {
            const auto cue=url::ToWide(item.node->Attribute("placeholder"));
            if (!cue.empty()) SendMessageW(widget,EM_SETCUEBANNER,TRUE,
                                         reinterpret_cast<LPARAM>(cue.c_str()));
        }
        if (kind==Kind::Text || kind==Kind::Password) {
            SetWindowSubclass(widget,&NativeFormControls::TextProc,1,
                              reinterpret_cast<DWORD_PTR>(this));
        }
        fields_.push_back({widget,item.node,kind,item.rect});
        ++created;
    }
    Reposition(viewport,scroll);
}
void NativeFormControls::Reposition(const RECT& viewport,int scroll) {
    for(const auto& field:fields_) {
        if(!IsWindow(field.hwnd)) continue;
        const int x=field.bounds.left;
        const int y=field.bounds.top+viewport.top-scroll;
        const int width=std::max(1L,field.bounds.right-field.bounds.left);
        const int height=std::max(1L,field.bounds.bottom-field.bounds.top);
        const bool visible=x+width>viewport.left && x<viewport.right &&
                           y+height>viewport.top && y<viewport.bottom;
        ShowWindow(field.hwnd,visible?SW_SHOWNA:SW_HIDE);
        if(!visible) continue;
        MoveWindow(field.hwnd,x,y,width,height,TRUE);
        const int left=std::max<int>(0,viewport.left-x);
        const int right=std::min<int>(width,viewport.right-x);
        const int top=std::max<int>(0,viewport.top-y);
        const int bottom=std::min<int>(height,viewport.bottom-y);
        HRGN clip=CreateRectRgn(left,top,right,bottom);
        if(clip) {
            if(SetWindowRgn(field.hwnd,clip,TRUE)==0) DeleteObject(clip);
        }
    }
}
FormRequest NativeFormControls::BuildRequest(HWND initiator,
                                              std::wstring_view pageUrl) const {
    const Field* sender=Find(initiator);
    if(!sender) return {};
    const html::Node* form=ParentForm(sender->node);
    if(!form) return {};
    std::string method=form->Attribute("method");
    std::transform(method.begin(),method.end(),method.begin(),
        [](unsigned char c){return static_cast<char>(std::tolower(c));});
    if(!method.empty() && method!="get" && method!="post") return {};
    const bool post=method=="post";
    const std::string action=form->Attribute("action");
    std::wstring result=action.empty() ? std::wstring(pageUrl) :
        url::Resolve(pageUrl,url::ToWide(action));
    if(!url::IsWeb(result)) return {};
    const auto fragment=result.find(L'#');
    if(fragment!=std::wstring::npos) result.erase(fragment);
    std::vector<const html::Node*> inputs;
    CollectNamed(*form,inputs);
    std::wstring query;
    for(const auto* node:inputs) {
        if(node->attributes.contains("disabled")) continue;
        const std::string name=node->Attribute("name");
        if(name.empty()) continue;
        const std::string type=node->Attribute("type");
        if(type=="reset" || type=="button" || type=="file") continue;
        if(type=="submit" && node!=sender->node) continue;
        std::wstring value;
        if(type=="hidden") {
            value=url::ToWide(node->Attribute("value"));
        } else {
            const auto bound=std::find_if(fields_.begin(),fields_.end(),
               [&](const Field& item){return item.node==node;});
            if(bound==fields_.end()) continue;
            if ((type=="checkbox" || type=="radio") &&
                SendMessageW(bound->hwnd,BM_GETCHECK,0,0)!=BST_CHECKED) continue;
            value=Value(*bound);
        }
        if(!query.empty()) query+=L'&';
        query+=Escape(url::ToWide(name));
        query+=L'=';
        query+=Escape(value);
        if(query.size()>16384) return {};
    }
    if(post) return {result,url::ToUtf8(query),true};
    if(!query.empty()) {
        result+=result.find(L'?')==std::wstring::npos?L'?':L'&';
        result+=query;
    }
    return {result,{},false};
}
void NativeFormControls::ResetForm(HWND initiator) {
    const auto* sender=Find(initiator);
    if(!sender) return;
    const auto* form=ParentForm(sender->node);
    if(!form) return;
    for(const auto& field:fields_) {
        if(ParentForm(field.node)!=form) continue;
        if(field.kind==Kind::Checkbox||field.kind==Kind::Radio) {
            const bool checked=field.node->attributes.contains("checked");
            SendMessageW(field.hwnd,BM_SETCHECK,
                         checked?BST_CHECKED:BST_UNCHECKED,0);
        } else if(field.kind==Kind::Select) {
            int index=0,selected=0;
            for(const auto& option:field.node->children) {
                if(option->tag!="option") continue;
                if(option->attributes.contains("selected")) selected=index;
                ++index;
            }
            SendMessageW(field.hwnd,CB_SETCURSEL,selected,0);
        } else if(field.kind==Kind::Text||field.kind==Kind::Password||
                  field.kind==Kind::Textarea) {
            SetWindowTextW(field.hwnd,DefaultValue(*field.node,field.kind).c_str());
        }
    }
}
} // namespace ac::forms
