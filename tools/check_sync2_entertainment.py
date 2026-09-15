"""Compile the real entertainment, ILP parser, publisher and persistence code.
Run with Python in a Visual Studio developer prompt (cl on PATH). No ROM required.
Only host dependencies are replaced; this is not a guest execution test.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
STUBS = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <typeindex>
#include <utility>
#include <vector>
#define LOG(...) ((void)0)
#define REGISTER_SERVICE(...)
constexpr int CERF_FATAL_RUNTIME_ERROR = 1;
[[noreturn]] void CerfFatalExit(int) { throw std::runtime_error("fatal"); }
struct CerfEmulator {
    std::map<std::type_index, void*> services;
    template<class T> T& Get() { return *static_cast<T*>(services.at(typeid(T))); }
    template<class T> T* TryGet() { return &Get<T>(); }
    template<class T> void Add(T& s) { services[typeid(T)] = &s; }
};
struct StateWriter {
    std::vector<uint8_t> bytes;
    void WriteBytes(const void* p,size_t n) { auto b=static_cast<const uint8_t*>(p); if(n)bytes.insert(bytes.end(),b,b+n); }
    template<class T> void Write(T v) { WriteBytes(&v,sizeof(v)); }
    size_t BytesWritten() const { return bytes.size(); }
    void PatchAt(size_t offset,const void* p,size_t n) { assert(offset+n<=bytes.size()); memcpy(bytes.data()+offset,p,n); }
};
struct StateReader {
    const std::vector<uint8_t>& bytes; size_t offset=0; bool ok=true;
    template<class T> void Read(T& v) { ReadBytes(&v,sizeof(v)); }
    void ReadBytes(void* p,size_t n) { if(n>bytes.size()-offset){ok=false;return;} if(n)memcpy(p,bytes.data()+offset,n); offset+=n; }
    bool Ok() const{return ok;} size_t Position() const{return offset;}
    size_t FileSize() const{return bytes.size();}
    void SeekTo(size_t p){if(p>bytes.size())ok=false;else offset=p;}
};
struct Service { CerfEmulator& emu_; explicit Service(CerfEmulator& e):emu_(e){}
    virtual ~Service()=default; virtual bool ShouldRegister(){return true;} virtual void OnReady(){} };
enum class Board { FordSyncGen2 };
struct BoardContext { Board GetBoard() const{return Board::FordSyncGen2;} };
struct FordSync2VmcuPeer {
    std::vector<std::vector<uint8_t>> frames;
    void InjectReliable(uint8_t cid,uint8_t,const uint8_t* p,size_t n){assert(cid==28);frames.emplace_back(p,p+n);}
};
struct JitRunner {void Pause(){} void Resume(){}};
struct EmulationPause {bool IsPaused()const{return false;}};
struct EmulationFreeze {int WorkerSection(){return 0;}};
'''
STUBS += r'''
using HDC=void*;struct RECT{};
enum class WidgetGroup {Indicator};
struct WidgetMenuItem {std::wstring label;bool enabled=true;};
struct HostWidget {
 virtual ~HostWidget()=default;
 virtual std::wstring WidgetName()const=0;virtual WidgetGroup Group()const=0;
 virtual bool PrimaryActionOpensMenu()const{return false;}
 virtual void DrawIcon(HDC,const RECT&)const=0;virtual std::wstring Tooltip()const=0;
 virtual bool PollDirty(){return false;}
 virtual std::vector<WidgetMenuItem> BuildMenu(){return {};}
 void MarkRx(){}
};
struct HostWidgetRegistry {void Register(HostWidget*){}};
struct HostIconCache {std::wstring last;void DrawCentered(HDC,const RECT&,const wchar_t* name){last=name;}};
'''
TEST = r'''
int main() {
    CerfEmulator emu;
    BoardContext board; FordSync2VmcuPeer peer;
    FordSync2IlpSignals signals(emu); FordSync2IlpChannel channel(emu);
    FordSync2Entertainment entertainment(emu);
    emu.Add(board); emu.Add(peer); emu.Add(signals); emu.Add(channel);
    entertainment.OnReady();
    auto request=[&](unsigned op,unsigned source,unsigned priority,unsigned system=0){
        std::vector<uint8_t> b{2,0,0x34,0x12,4,0};
        const unsigned vals[]={priority,source,system,op};
        for(unsigned i=0;i<4;++i){unsigned id=0x0200007E+i;
            for(unsigned j=0;j<4;++j)b.push_back(static_cast<uint8_t>(id>>(8*j)));
            b.push_back(static_cast<uint8_t>(vals[i]));}
        peer.frames.clear();channel.HandleInbound(b.data(),b.size());
    };
    auto batch=[&](size_t frame,std::array<uint8_t,9> expected){
        const auto& f=peer.frames.at(frame);assert(f.size()==51&&f[0]==10&&f[4]==9&&f[5]==0);
        for(unsigned i=0;i<9;++i){size_t p=6+5*i;uint32_t id=0;
            for(unsigned j=0;j<4;++j)id|=uint32_t(f[p+j])<<(8*j);
            assert(id==0x02000092+i&&f[p+4]==expected[i]);}
    };
    auto subscribe=[&](){for(unsigned i=0;i<9;++i){
        std::array<uint8_t,26> b{5,0,0x55,0};uint32_t id=0x02000092+i;
        for(unsigned j=0;j<4;++j)b[4+j]=static_cast<uint8_t>(id>>(8*j));
        channel.HandleInbound(b.data(),b.size());}
    };
    // A query before filter registration must retain both events, including across a snapshot.
    request(5,12,14); assert(peer.frames.size()==1&&peer.frames[0][1]==0);
    StateWriter pending; channel.SaveState(pending);
    StateReader pendingReader{pending.bytes}; channel.RestoreState(pendingReader); assert(pendingReader.Ok());
    subscribe(); peer.frames.clear(); channel.OnWatchdogPet();
    assert(peer.frames.size()==2);
    batch(0,{14,12,0,0,14,12,0,5,1}); batch(1,{14,12,0,1,14,12,0,0,4});
    request(1,0,7); assert(peer.frames.size()==3);
    assert((peer.frames[0]==std::vector<uint8_t>{0x82,0,0x34,0x12}));
    batch(1,{14,12,0,0,7,0,0,1,1}); batch(2,{7,0,0,4,14,12,0,0,4});
    request(1,0,7); assert(peer.frames.size()==3); // repeated request stays granted
    request(1,8,11); assert(peer.frames.size()==4);
    batch(2,{7,0,0,1,14,12,0,0,4}); batch(3,{11,8,0,4,14,12,0,0,4});
    request(2,0,7); assert(peer.frames.size()==3); // release of absent radio preserves APIM
    request(5,12,14); batch(2,{11,8,0,4,14,12,0,0,4});
    request(4,0,7); batch(2,{7,0,0,1,14,12,0,0,4});
    request(1,6,2); assert(peer.frames.size()==2); batch(1,{14,12,0,0,2,6,0,1,3});
    request(1,0,7,1); assert(peer.frames.size()==2); batch(1,{14,12,0,0,7,0,1,1,3});
    StateWriter active; channel.SaveState(active);
    request(3,12,14); batch(2,{14,12,0,1,14,12,0,0,4});
    StateReader activeReader{active.bytes}; channel.RestoreState(activeReader); assert(activeReader.Ok());
    request(5,12,14); batch(2,{11,8,0,4,14,12,0,0,4});
    request(2,8,11); assert(peer.frames.size()==4);batch(3,{14,12,0,1,14,12,0,0,4});
    request(3,12,14); assert(peer.frames.size()==3); // repeated release-all is idempotent
    request(0,12,14); assert(peer.frames.size()==1);
    request(6,0,7);assert(peer.frames.size()==1&&peer.frames[0][1]==1);
    request(1,0,15);assert(peer.frames.size()==1&&peer.frames[0][1]==1);
    const uint8_t duplicate[]={2,0,1,0,4,0,0x7E,0,0,2,7,0x7E,0,0,2,7,0x80,0,0,2,0,0x81,0,0,2,1};
    peer.frames.clear(); channel.HandleInbound(duplicate,sizeof(duplicate));assert(peer.frames.size()==1&&peer.frames[0][1]==1);
    request(5,12,14);batch(2,{14,12,0,1,14,12,0,0,4});
    const uint8_t partial[]={2,0,1,0,1,0,0x81,0,0,2,1};
    peer.frames.clear();channel.HandleInbound(partial,sizeof(partial));assert(peer.frames.size()==1&&peer.frames[0][1]==1);
    peer.frames.clear();channel.OnWatchdogPet();assert(peer.frames.empty());
    signals.RepublishComposites();channel.PublishPending();assert(peer.frames.empty());

    // A rejected snapshot cannot install an invalid active resource.
    const std::string key="entertainment";
    auto keyPos=std::search(active.bytes.begin(),active.bytes.end(),key.begin(),key.end());
    assert(keyPos!=active.bytes.end());*(keyPos+key.size()+4+8)=255;
    bool rejected=false;try{StateReader corrupt{active.bytes};channel.RestoreState(corrupt);}catch(const std::runtime_error&){rejected=true;}
    assert(rejected);
    // Actual descriptor widths agree with the nine-byte guest payload.
    for(unsigned i=0;i<9;++i)assert(FordSync2IlpSignals::HeadWriteWidth(0x02000092+i)==1);
    {
        CerfEmulator e; BoardContext b; FordSync2VmcuPeer p;
        FordSync2IlpSignals s(e); FordSync2IlpChannel c(e); FordSync2Entertainment a(e);
        e.Add(b);e.Add(p);e.Add(s);e.Add(c);a.OnReady();
        const uint8_t query[]={2,0,1,0,4,0,0x7E,0,0,2,14,0x7F,0,0,2,12,0x80,0,0,2,0,0x81,0,0,2,5};
        for(unsigned i=0;i<24;++i)c.HandleInbound(query,sizeof(query));
        assert(p.frames.size()==24);c.HandleInbound(query,sizeof(query));assert(p.frames.back()[1]==1);
        StateWriter full;c.SaveState(full);StateReader fullReader{full.bytes};c.RestoreState(fullReader);assert(fullReader.Ok());
        for(unsigned i=0;i<9;++i)s.NoteFilterRegistration(0x02000092+i,0x55);
        p.frames.clear();c.OnWatchdogPet();assert(p.frames.size()==48);
    }
    {
        CerfEmulator emu; BoardContext board; FordSync2VmcuPeer peer;
        FordSync2IlpSignals signals(emu); FordSync2IlpChannel channel(emu);
        emu.Add(board); emu.Add(peer); emu.Add(signals); emu.Add(channel);
        HostWidgetRegistry widgets;HostIconCache icons;emu.Add(widgets);emu.Add(icons);
        FordSync2MediaStatus media(emu);media.OnReady();
        auto send=[&](std::initializer_list<std::pair<uint32_t,uint64_t>> values){
            std::vector<uint8_t> b{2,0,1,0,static_cast<uint8_t>(values.size()),0};
            for(auto [id,v]:values){for(unsigned j=0;j<4;++j)b.push_back(id>>(8*j));
                for(unsigned j=0;j<FordSync2IlpSignals::HeadWriteWidth(id);++j)b.push_back(v>>(8*j));}
            peer.frames.clear();channel.HandleInbound(b.data(),b.size());
            return peer.frames.at(0).at(1)==0;
        };
        auto label=[&](size_t i){return media.BuildMenu().at(i).label;};
        RECT rect;media.DrawIcon(nullptr,rect);assert(icons.last==L"ICON_MEDIA_WAITING");
        assert(label(0)==L"Waiting for status" && label(1).find(L"Not reported")!=std::wstring::npos);
        assert(send({{0x02000212,101}}));
        media.DrawIcon(nullptr,rect);assert(icons.last==L"ICON_MEDIA_PARTIAL");
        assert(label(3)==L"Track playtime (reported): 101");
        assert(!send({{0x02000212,200},{0x02000212,300}}));
        assert(label(3)==L"Track playtime (reported): 101");
        assert(!send({{0x02000212,200},{0x02000174,0}}));
        assert(label(3)==L"Track playtime (reported): 101");
        assert(send({{0x02000213,65535},{0x02000214,3},{0x02000215,0xFFFFFFFFull}}));
        media.DrawIcon(nullptr,rect);assert(icons.last==L"ICON_MEDIA_READY");
        assert(label(1)==L"Track number: 4294967295");
        assert(media.PollDirty()&&!media.PollDirty());
        StateWriter saved;channel.SaveState(saved);
        assert(send({{0x02000215,2},{0x02000212,0}}));
        StateReader reader{saved.bytes};channel.RestoreState(reader);
        assert(reader.Ok()&&label(1)==L"Track number: 4294967295"&&label(3)==L"Track playtime (reported): 101");
        assert(peer.frames.size()==1);
        const std::string key="media_status";
        auto pos=std::search(saved.bytes.begin(),saved.bytes.end(),key.begin(),key.end());
        assert(pos!=saved.bytes.end());*(pos+key.size()+4+8)=16;
        bool rejected=false;
        try { StateReader corrupt{saved.bytes}; channel.RestoreState(corrupt); }
        catch(const std::runtime_error&) { rejected=true; }
        assert(rejected);
    }

    puts("Entertainment ILP checks passed: ordering, grants, releases, denial, validation, subscriptions, snapshots, media-status values and icon states");
}
'''

def source(name):
    return '\n'.join(line for line in (ROOT/name).read_text(encoding='utf-8-sig').splitlines()
                     if not line.lstrip().startswith(('#include', '#pragma once'))) + '\n'

with tempfile.TemporaryDirectory(prefix='cerf-entertainment-check-') as temp:
    path=Path(temp)
    code=STUBS
    base='cerf/boards/ford_sync2/'
    for name in ('ford_sync2_ilp_signal_tables.h','ford_sync2_ilp_descriptors.h',
                 'ford_sync2_ilp_signals.h','ford_sync2_ilp_signals.cpp',
                 'ford_sync2_ilp_channel.h','ford_sync2_ilp_channel.cpp',
                 'ford_sync2_entertainment_state.h','ford_sync2_entertainment.cpp','ford_sync2_media_status.cpp'):
        code+=source(base+name)
    cpp=path/'check.cpp';cpp.write_text(code+TEST,encoding='utf-8')
    exe=path/'check.exe'
    subprocess.run(['cl','/nologo','/std:c++20','/EHsc','/utf-8','/Od',str(cpp),'/Fe:'+str(exe)],cwd=path,check=True)
    subprocess.run([str(exe)],cwd=path,check=True,timeout=30)
