#include "ime_uia.hpp"
#include "ime_region.hpp"
#include "ime_capture.hpp"
#include <UIAutomation.h>
#include <algorithm>
#include <chrono>
#include <string>
#include <cmath>

namespace surfxr::transport::desktop {
namespace {
struct Apartment {
    HRESULT result=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    ~Apartment(){if(SUCCEEDED(result))CoUninitialize();}
};
bool list_region(IUIAutomation* automation,IUIAutomationCondition* condition,HWND host,HWND foreground,RECT& rect){
    if(!IsWindowVisible(host)||IsIconic(host)||host==foreground)return false;
    Com<IUIAutomationElement> root,list;
    if(FAILED(automation->ElementFromHandle(host,&root))||!root||
       FAILED(root->FindFirst(TreeScope_Subtree,condition,&list))||!list)return false;
    BOOL offscreen=TRUE;RECT reported{},physical{};
    if(FAILED(list->get_CurrentIsOffscreen(&offscreen))||offscreen||
       FAILED(list->get_CurrentBoundingRectangle(&reported))||!GetWindowRect(host,&physical))return false;
    // Providers can retain expanded bounds after collapse. Never substitute
    // the whole host (which may cover a monitor) for an explicit candidate ROI.
    return IntersectRect(&rect,&reported,&physical)&&ime_region_related(host,foreground,rect)&&ime_region_visible(host,rect);
}
}
bool ime_uia_region(HWND foreground,HWND& window,RECT& rect,const std::vector<HWND>& preferred){
    Apartment apartment;if(FAILED(apartment.result)&&apartment.result!=RPC_E_CHANGED_MODE)return false;
    Com<IUIAutomation2> automation;
    if(FAILED(CoCreateInstance(__uuidof(CUIAutomation8),nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&automation))))return false;
    automation->put_ConnectionTimeout(100);automation->put_TransactionTimeout(100);
    Com<IUIAutomationCondition> identity;
    // Provider IDs are nonlocalized hints, not a language or executable allowlist.
    // Providers with arbitrary IDs can instead expose the standard IME WinEvents.
    for(const auto id:{L"IME_Candidate_Window",L"IME_Prediction_Window",L"CandidateWindow",L"CandidateList",L"CandidateListUI",L"PredictionList"}){
        VARIANT value{};value.vt=VT_BSTR;value.bstrVal=SysAllocString(id);
        Com<IUIAutomationCondition> next,combined;
        const auto result=automation->CreatePropertyCondition(UIA_AutomationIdPropertyId,value,&next);VariantClear(&value);
        if(FAILED(result))return false;
        if(!identity)identity=next;
        else {if(FAILED(automation->CreateOrCondition(identity.Get(),next.Get(),&combined)))return false;identity=combined;}
    }
    VARIANT visible{};visible.vt=VT_BOOL;visible.boolVal=VARIANT_FALSE;
    Com<IUIAutomationCondition> onscreen,condition;
    if(FAILED(automation->CreatePropertyCondition(UIA_IsOffscreenPropertyId,visible,&onscreen))||
       FAILED(automation->CreateAndCondition(identity.Get(),onscreen.Get(),&condition)))return false;
    std::vector<HWND> hosts;
    const auto append=[&](HWND host){if(host&&hosts.size()<32&&std::find(hosts.begin(),hosts.end(),host)==hosts.end())hosts.push_back(host);};
    for(const auto host:preferred)append(host);
    // FindWindowEx also sees shell-owned CoreWindows omitted by desktop/child
    // enumeration. A host from any process must still expose a candidate ROI.
    HWND top=nullptr;
    for(unsigned i=0;i<512&&hosts.size()<32;++i){
        top=FindWindowExW(nullptr,top,nullptr,nullptr);if(!top)break;
        if(!IsWindowVisible(top)||IsIconic(top)||top==foreground)continue;
        wchar_t name[160]{};GetClassNameW(top,name,160);const std::wstring cls=name;
        if(cls==L"Windows.UI.Core.CoreWindow")append(top);
        else if(cls==L"ApplicationFrameWindow"){
            HWND child=nullptr;for(unsigned j=0;j<16;++j){child=FindWindowExW(top,child,L"Windows.UI.Core.CoreWindow",nullptr);if(!child)break;append(child);}
        }else if(ime_owned_by(top,foreground)&&(GetWindowLongPtrW(top,GWL_EXSTYLE)&WS_EX_NOACTIVATE))append(top);
    }
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(250);
    for(const auto host:hosts){
        if(GetForegroundWindow()!=foreground||std::chrono::steady_clock::now()>=deadline)break;
        if(list_region(automation.Get(),condition.Get(),host,foreground,rect)){window=host;return true;}
    }
    return false;
}
ImeAction ime_uia_invoke(const ImeCandidate& candidate,float u,float v){
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(250);
    Apartment apartment;if(FAILED(apartment.result)&&apartment.result!=RPC_E_CHANGED_MODE)return ImeAction::Unavailable;
    Com<IUIAutomation2> automation;
    if(FAILED(CoCreateInstance(__uuidof(CUIAutomation8),nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&automation))))return ImeAction::Unavailable;
    automation->put_ConnectionTimeout(100);automation->put_TransactionTimeout(100);
    if(!candidate.contains(u,v))return ImeAction::Failed;
    POINT point{candidate.rect.left+static_cast<LONG>(std::lround(u*(candidate.rect.right-candidate.rect.left-1))),
        candidate.rect.top+static_cast<LONG>(std::lround(v*(candidate.rect.bottom-candidate.rect.top-1)))};
    Com<IUIAutomationElement> element,root;
    Com<IUIAutomationTreeWalker> walker;
    if(FAILED(automation->ElementFromHandle(candidate.window,&root))||!root||
       FAILED(automation->ElementFromPoint(point,&element))||!element||
       FAILED(automation->get_RawViewWalker(&walker))||!walker)return ImeAction::Unavailable;
    // Prove ancestry before invoking anything. Never invoke an overlapping app
    // or a host-wide action in place of the small candidate under the ray.
    std::vector<Com<IUIAutomationElement>> chain;bool inside=false;
    for(unsigned i=0;i<16&&std::chrono::steady_clock::now()<deadline;++i){
        BOOL same=FALSE;if(FAILED(automation->CompareElements(element.Get(),root.Get(),&same)))return ImeAction::Unavailable;
        if(same){inside=true;break;}
        chain.push_back(element);Com<IUIAutomationElement> parent;
        if(FAILED(walker->GetParentElement(element.Get(),&parent))||!parent)break;element=parent;
    }
    if(!inside)return ImeAction::Unavailable;
    for(const auto& item:chain){
        if(std::chrono::steady_clock::now()>=deadline)break;
        RECT rect{};BOOL offscreen=TRUE,enabled=FALSE;
        if(FAILED(item->get_CurrentBoundingRectangle(&rect))||IsRectEmpty(&rect)||!PtInRect(&rect,point)||
           rect.left<candidate.rect.left||rect.top<candidate.rect.top||rect.right>candidate.rect.right||rect.bottom>candidate.rect.bottom||
           FAILED(item->get_CurrentIsOffscreen(&offscreen))||offscreen||FAILED(item->get_CurrentIsEnabled(&enabled))||!enabled)continue;
        Com<IUIAutomationInvokePattern> invoke;
        if(SUCCEEDED(item->GetCurrentPatternAs(UIA_InvokePatternId,IID_PPV_ARGS(&invoke)))&&invoke){
            if(std::chrono::steady_clock::now()>=deadline||!candidate.contains(u,v))return ImeAction::Failed;
            return SUCCEEDED(invoke->Invoke())?ImeAction::Invoked:ImeAction::Failed;
        }
        Com<IUIAutomationLegacyIAccessiblePattern> legacy;
        if(SUCCEEDED(item->GetCurrentPatternAs(UIA_LegacyIAccessiblePatternId,IID_PPV_ARGS(&legacy)))&&legacy){
            BSTR action=nullptr;const auto result=legacy->get_CurrentDefaultAction(&action);
            const bool available=SUCCEEDED(result)&&action&&SysStringLen(action);SysFreeString(action);
            if(available){
                if(std::chrono::steady_clock::now()>=deadline||!candidate.contains(u,v))return ImeAction::Failed;
                return SUCCEEDED(legacy->DoDefaultAction())?ImeAction::Invoked:ImeAction::Failed;
            }
        }
    }
    return ImeAction::Unavailable;
}
}
