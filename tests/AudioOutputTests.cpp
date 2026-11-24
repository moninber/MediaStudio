#include "AudioOutput.h"

#include <audiopolicy.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

using namespace std::chrono_literals;
struct AudioOutputTestAccess {
    static void factory(AudioOutput& output, std::function<HRESULT(IMMDevice*,IAudioClient**)> factory) {
        output.client_factory_=std::move(factory);
    }
};
namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
struct Probe {
    int created=0,destroyed=0;
    bool fail_first=false,fail_all=false,fail_format=false,cancel_after_failure=false;
    bool allowed=true;
    std::vector<REFERENCE_TIME> requested;
    UINT32 actual_frames=0;
};

// 只代理当前测试创建的真实 IAudioClient；可拒绝 Initialize，其他操作仍使用实际 WASAPI。
class ClientProxy final:public IAudioClient {
public:
    ClientProxy(IAudioClient* client,Probe& probe):client_(client),probe_(probe){++probe_.created;}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** result)override{
        if(!result)return E_POINTER;
        if(IsEqualIID(iid,__uuidof(IUnknown))||IsEqualIID(iid,__uuidof(IAudioClient))){*result=this;AddRef();return S_OK;}
        *result=nullptr;return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef()override{return ++refs_;}
    ULONG STDMETHODCALLTYPE Release()override{const auto count=--refs_;if(!count)delete this;return count;}
    HRESULT STDMETHODCALLTYPE Initialize(AUDCLNT_SHAREMODE mode,DWORD flags,REFERENCE_TIME duration,REFERENCE_TIME period,
        const WAVEFORMATEX* format,LPCGUID session)override{
        probe_.requested.push_back(duration);
        if(probe_.fail_all||(probe_.fail_first&&probe_.requested.size()==1)){
            if(probe_.cancel_after_failure)probe_.allowed=false;
            return E_FAIL; // 显式注入首次缓冲请求失败，不宣称真实设备自然拒绝了 100 ms。
        }
        return client_->Initialize(mode,flags,duration,period,format,session);
    }
    HRESULT STDMETHODCALLTYPE GetBufferSize(UINT32* frames)override{const auto hr=client_->GetBufferSize(frames);if(SUCCEEDED(hr))probe_.actual_frames=*frames;return hr;}
    HRESULT STDMETHODCALLTYPE GetStreamLatency(REFERENCE_TIME* latency)override{return client_->GetStreamLatency(latency);}
    HRESULT STDMETHODCALLTYPE GetCurrentPadding(UINT32* padding)override{return client_->GetCurrentPadding(padding);}
    HRESULT STDMETHODCALLTYPE IsFormatSupported(AUDCLNT_SHAREMODE mode,const WAVEFORMATEX* format,WAVEFORMATEX** closest)override{
        if(probe_.fail_format){if(closest)*closest=nullptr;return AUDCLNT_E_UNSUPPORTED_FORMAT;}
        return client_->IsFormatSupported(mode,format,closest);
    }
    HRESULT STDMETHODCALLTYPE GetMixFormat(WAVEFORMATEX** format)override{return client_->GetMixFormat(format);}
    HRESULT STDMETHODCALLTYPE GetDevicePeriod(REFERENCE_TIME* normal,REFERENCE_TIME* minimum)override{return client_->GetDevicePeriod(normal,minimum);}
    HRESULT STDMETHODCALLTYPE Start()override{return client_->Start();}
    HRESULT STDMETHODCALLTYPE Stop()override{return client_->Stop();}
    HRESULT STDMETHODCALLTYPE Reset()override{return client_->Reset();}
    HRESULT STDMETHODCALLTYPE SetEventHandle(HANDLE event)override{return client_->SetEventHandle(event);}
    HRESULT STDMETHODCALLTYPE GetService(REFIID iid,void** result)override{return client_->GetService(iid,result);}
private:
    ~ClientProxy(){client_->Release();++probe_.destroyed;}
    std::atomic<ULONG> refs_{1};IAudioClient* client_;Probe& probe_;
};

void attach(AudioOutput& output,Probe& probe){
    AudioOutputTestAccess::factory(output,[&](IMMDevice* device,IAudioClient** result){
        // 进入第二次激活前，前一次客户端必须已经 Release；地址可能复用，因此用生命周期计数核对。
        require(probe.created==probe.destroyed,"failed client remained alive before fallback/reopen activation");
        IAudioClient* client=nullptr;
        const auto hr=device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(&client));
        if(FAILED(hr))return hr;
        *result=new ClientProxy(client,probe);return S_OK;
    });
}

bool hasEndpoint(){
    IMMDeviceEnumerator* enumerator=nullptr;IMMDevice* device=nullptr;
    const auto hr=CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator));
    require(SUCCEEDED(hr),"cannot enumerate audio devices");
    const auto endpoint=enumerator->GetDefaultAudioEndpoint(eRender,eConsole,&device);
    if(device)device->Release();enumerator->Release();
    if(endpoint==HRESULT_FROM_WIN32(ERROR_NOT_FOUND))return false;
    require(SUCCEEDED(endpoint),"audio endpoint lookup failed");return true;
}

int activeSessions(){
    IMMDeviceEnumerator* enumerator=nullptr;IMMDevice* device=nullptr;IAudioSessionManager2* manager=nullptr;IAudioSessionEnumerator* sessions=nullptr;
    require(SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator))),"session enumerator unavailable");
    require(SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender,eConsole,&device)),"default endpoint unavailable");
    require(SUCCEEDED(device->Activate(__uuidof(IAudioSessionManager2),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(&manager))),"audio session manager unavailable");
    require(SUCCEEDED(manager->GetSessionEnumerator(&sessions)),"cannot enumerate sessions");
    int count=0,active=0;sessions->GetCount(&count);
    for(int i=0;i<count;++i){
        IAudioSessionControl* control=nullptr;IAudioSessionControl2* extra=nullptr;DWORD process=0;AudioSessionState state=AudioSessionStateExpired;
        if(SUCCEEDED(sessions->GetSession(i,&control))&&SUCCEEDED(control->QueryInterface(IID_PPV_ARGS(&extra)))&&
            SUCCEEDED(extra->GetProcessId(&process))&&process==GetCurrentProcessId()&&SUCCEEDED(control->GetState(&state))&&state==AudioSessionStateActive)++active;
        if(extra)extra->Release();if(control)control->Release();
    }
    sessions->Release();manager->Release();device->Release();enumerator->Release();return active;
}

void submitSilence(AudioOutput& output){
    std::vector<std::uint8_t> pcm(4410*4,0);std::size_t offset=0;
    const std::size_t frames=pcm.size()/4; // write 的偏移单位是 PCM 帧，不能与字节数比较。
    const auto deadline=std::chrono::steady_clock::now()+2s;
    while(offset<frames&&std::chrono::steady_clock::now()<deadline){
        const auto result=output.write(pcm,offset,[]{return true;});
        require(result!=AudioWriteResult::Error,"actual PCM write failed");
        if(result==AudioWriteResult::WouldBlock)std::this_thread::sleep_for(2ms);
    }
    require(offset==frames&&output.sample().valid&&activeSessions()==1,"actual PCM/session was not established");
}

// 实际请求、100 ns 单位转换、GetBufferSize 容量以及 PCM 输出必须彼此一致。
void testRealBuffer(bool low){
    Probe probe;AudioOutput output;attach(output,probe);
    const AudioBufferRequest request=low?AudioBufferRequest{100ms,350ms}:AudioBufferRequest{};
    require(output.init(44100,2,request),"real buffer initialization failed");
    const auto info=output.bufferReport();
    require(info.initialized&&info.requested==request.duration&&info.device_frames==probe.actual_frames&&info.sample_rate==44100&&
        info.capacity_seconds&&std::abs(*info.capacity_seconds-static_cast<double>(probe.actual_frames)/44100.0)<1e-12&&
        probe.requested.front()==request.duration.count()*10000&&info.attempts==probe.requested.size(),"requested/actual/units mismatch");
    submitSilence(output);output.reset();
    require(output.bufferReport().device_frames==info.device_frames,"clock reset erased measured device capacity");
    output.cleanup();require(probe.created==probe.destroyed&&activeSessions()==0,"actual audio client/session leaked");
    std::cout<<"real_buffer_requested_ms="<<request.duration.count()<<" actual_frames="<<info.device_frames
        <<" capacity_ms="<<*info.capacity_seconds*1000<<" PCM=1 client_released=1\n";
}

// 强制首次 Initialize 失败，再用新客户端真实初始化 350 ms，验证回退及旧资源释放。
void testFallback(std::string_view mode){
    Probe probe;probe.fail_first=true;probe.fail_all=mode=="fallback-fails";probe.cancel_after_failure=mode=="fallback-cancel";
    AudioOutput output;attach(output,probe);
    const bool opened=output.init(44100,2,{100ms,350ms},[&]{return probe.allowed;});
    const auto info=output.bufferReport();
    require(info.requested==100ms&&info.first_error==static_cast<std::int32_t>(E_FAIL),"initial injected HRESULT/request lost");
    if(mode=="fallback"){
        require(opened&&info.initialized&&info.attempted==350ms&&info.attempts==2&&info.usedFallback()&&
            info.capacity_seconds&&probe.requested==std::vector<REFERENCE_TIME>{1000000,3500000}&&probe.created==2&&probe.destroyed==1,
            "fallback failed, reused old client or advertised 100 ms");
        submitSilence(output);
    }else if(mode=="fallback-fails"){
        require(!opened&&!info.initialized&&!info.usedFallback()&&info.attempts==2&&info.last_error==static_cast<std::int32_t>(E_FAIL)&&
            !info.capacity_seconds&&probe.created==2&&probe.destroyed==2,"failed fallback leaked resources or invented actual capacity");
    }else{
        require(!opened&&info.cancelled&&info.attempts==1&&!info.usedFallback()&&probe.created==1&&probe.destroyed==1,
            "cancel did not override fallback creation");
    }
    output.cleanup();require(probe.created==probe.destroyed&&activeSessions()==0,"fallback/cancel left an audio client or session");
    std::cout<<"audio_case="<<mode<<" injected_first_failure=1 actual_350ms_fallback="<<opened<<" old_client_released_before_new=1 cancellation_observed=1\n";
}

void testInvalid(){
    AudioOutput output;
    for(const auto duration:{0ms,-1ms,std::chrono::milliseconds::max()}){
        require(!output.init(44100,2,{duration,{}})&&output.bufferReport().attempts==0&&
            output.bufferReport().first_error==static_cast<std::int32_t>(E_INVALIDARG),"invalid duration reached WASAPI");
    }
    require(!output.init(44100,2,{100ms,100ms})&&!output.init(0,2)&&!output.init(44100,0),"invalid format/fallback was accepted");
    std::cout<<"invalid_duration_and_format_before_COM=1 duration_overflow_guard=1 invalid_fallback=1\n";
}
void testFormatFailure(){
    Probe probe;probe.fail_format=true;AudioOutput output;attach(output,probe);
    require(!output.init(44100,2,{100ms,350ms}),"unsupported format accepted");
    const auto info=output.bufferReport();
    require(probe.created==1&&probe.destroyed==1&&probe.requested.empty()&&info.attempts==0&&!info.capacity_seconds,
        "format failure retried buffer requests or retained resources");
    std::cout<<"format_failure_not_buffer_fallback=1 partial_initialization_released=1\n";
}
}
int main(int argc,char** argv){
    const auto com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    try{
        require(SUCCEEDED(com),"test COM initialization failed");
        const std::string_view mode=argc==2?argv[1]:"";
        if(mode=="invalid")testInvalid();
        else{
            if(!hasEndpoint()){CoUninitialize();return 77;}
            if(mode=="default"||mode=="low")testRealBuffer(mode=="low");
            else if(mode=="fallback"||mode=="fallback-fails"||mode=="fallback-cancel")testFallback(mode);
            else if(mode=="format-failure")testFormatFailure();else throw std::runtime_error("unknown audio test");
        }
        CoUninitialize();return 0;
    }catch(const std::exception& error){std::cerr<<"Audio test failed: "<<error.what()<<'\n';if(SUCCEEDED(com))CoUninitialize();return 1;}
}
