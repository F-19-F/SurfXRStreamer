#pragma once
#include "windows.hpp"
#include <sddl.h>
namespace surfxr::transport::desktop {
class MediaLease {
public:
    explicit MediaLease(const wchar_t* name=L"Global\\SURFXRFrameMedia-v2",DWORD timeout=8000,std::function<bool()> cancelled={},HANDLE cancellation=nullptr){
        PSECURITY_DESCRIPTOR descriptor=nullptr;
        require(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x00100002;;;AU)",SDDL_REVISION_1,&descriptor,nullptr),"desktop_media_lease_acl");
        SECURITY_ATTRIBUTES attrs{sizeof(attrs),descriptor,FALSE};
        semaphore_.value=CreateSemaphoreExW(&attrs,1,1,name,0,SYNCHRONIZE|SEMAPHORE_MODIFY_STATE);LocalFree(descriptor);
        require(semaphore_.value,"desktop_media_lease_create");
        DWORD result=WAIT_TIMEOUT;
        if(cancellation){
            if(!cancelled||!cancelled()){
                const HANDLE events[]{semaphore_.value,cancellation};result=WaitForMultipleObjects(2,events,FALSE,timeout);
                if(result==WAIT_OBJECT_0&&cancelled&&cancelled()){ReleaseSemaphore(semaphore_.value,1,nullptr);result=WAIT_OBJECT_0+1;}
            }
        }else if(!cancelled)result=WaitForSingleObject(semaphore_.value,timeout);
        else {
            const auto deadline=GetTickCount64()+timeout;
            while(GetTickCount64()<deadline&&!cancelled()){
                result=WaitForSingleObject(semaphore_.value,2);if(result!=WAIT_TIMEOUT)break;
            }
        }
        require(result==WAIT_OBJECT_0,"desktop_other_media_still_running");
    }
    ~MediaLease(){ReleaseSemaphore(semaphore_.value,1,nullptr);}
private:
    Handle semaphore_;
};
}
