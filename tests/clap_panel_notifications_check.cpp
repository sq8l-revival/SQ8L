#include "ClapPanelNotifications.h"
#include "SoundParameterService.h"
#include "automation_adapter_check.h"
#include <array>
#include <iostream>
#include <vector>
using namespace sq8l::sound;
using automation_check::require;
struct Event { enum Kind {Begin,Value,End} kind; EchoToken token{}; };
int main() try {
    SoundParameterService service(sq8l::makeInitProgram());
    auto edit=service.accept(0,2,Origin::Panel);
    std::array<Event,3> events{{{Event::Begin,{}},{Event::Value,edit.token},{Event::End,{}}}};
    std::vector<Event::Kind> delivered;
    auto current=[&](const Event& e){return e.kind!=Event::Value || service.currentPanelEcho(e.token);};
    auto revision=service.snapshot().revision;
    auto drain=[&](unsigned start,unsigned capacity,bool available=true){
        return sq8l::drainClapPanelNotifications(events.data()+start,3-start,current,[&](const Event& e){
            if(!available || capacity==0)return false;--capacity;delivered.push_back(e.kind);return true;
        });
    };
    require(drain(0,3,false)==0,"missing output retains complete queue");
    require(drain(0,1)==1,"rejected value retains value/end suffix");
    service.accept(0,2,Origin::Host); // equal value remains a new semantic Host edit
    require(drain(1,0)==1,"stale value consumed while rejected end retained");
    service.replace(sq8l::makeInitProgram());
    require(drain(2,1)==1,"replacement preserves end for published begin");
    require(delivered==std::vector<Event::Kind>{Event::Begin,Event::End},"balanced gesture after replacement");
    edit=service.accept(0,2,Origin::Panel);events[1].token=edit.token;delivered.clear();
    revision=service.snapshot().revision;
    require(drain(0,3)==3,"current exact token publishes in order");
    require(service.snapshot().revision==revision,"drain never replays setter");
    require(delivered==std::vector<Event::Kind>{Event::Begin,Event::Value,Event::End},"ordered complete gesture");
    delivered.clear();service.replace(sq8l::makeInitProgram());
    require(drain(0,3)==3 && delivered.size()==2,"replacement drops stale queued value");
    std::cout<<"CLAP shared queue: missing/rejected outputs, suffix retry, exact token, equal Host edit, replacement, gesture balance, no semantic replay passed\n";
} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
