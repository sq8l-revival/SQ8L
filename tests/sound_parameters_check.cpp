// This checker reads the frozen identity/appearance fixture independently of the registry.
#include "SoundParameters.h"
#include "EditBuffer.h"
#include "LcdControl.h"
#include <cmath>
#include <cctype>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <cstring>
using namespace sq8l;
namespace {
struct Json {
    std::map<std::string,Json> object;
    std::vector<Json> array;
    std::string value;
    const Json& at(const std::string& key) const { return object.at(key); }
    int number() const { return std::stoi(value); }
};
class Parser {
    std::string text; size_t pos=0;
    void ws(){while(pos<text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;}
    char take(){if(pos>=text.size())throw std::runtime_error("truncated fixture");return text[pos++];}
    std::string string(){if(take()!='"')throw std::runtime_error("expected string");std::string s;for(char c=take();c!='"';c=take()){if(c=='\\'){c=take();if(c=='n')c='\n';else if(c=='t')c='\t';else if(c!='\\'&&c!='"')throw std::runtime_error("unsupported escape");}s+=c;}return s;}
    Json item(){ws();Json j;if(text.at(pos)=='{'){++pos;ws();while(text.at(pos)!='}'){auto key=string();ws();if(take()!=':')throw std::runtime_error("expected colon");j.object.emplace(key,item());ws();if(text.at(pos)=='}')break;if(take()!=',')throw std::runtime_error("expected comma");ws();}++pos;}else if(text.at(pos)=='['){++pos;ws();while(text.at(pos)!=']'){j.array.push_back(item());ws();if(text.at(pos)==']')break;if(take()!=',')throw std::runtime_error("expected comma");}++pos;}else if(text.at(pos)=='"')j.value=string();else{while(pos<text.size() && (text[pos]=='-'||std::isdigit(static_cast<unsigned char>(text[pos]))))j.value+=take();if(j.value.empty())throw std::runtime_error("expected integer");}return j;}
public:
    explicit Parser(std::string s):text(std::move(s)){}
    Json parse(){auto j=item();ws();if(pos!=text.size())throw std::runtime_error("trailing fixture bytes");return j;}
};
int checks=0;
void require(bool b,const std::string& what){++checks;if(!b)throw std::runtime_error(what);}
int n(const Json& j,const char* key){return j.at(key).number();}
bool packed(const sound::Parameter& p){using L=sound::LabelKind;return p.label==L::LfoPhase||p.label==L::EnvVelLevel||p.label==L::EnvT4||p.codec==sound::Codec::SyncT4;}
}
int main(int argc,char** argv)try {
    require(argc==2,"fixture path required");std::ifstream file(argv[1]);require(bool(file),"open fixture");std::stringstream buf;buf<<file.rdbuf();auto fixture=Parser(buf.str()).parse();
    const auto& expected=fixture.at("parameters").array;
    require(expected.size()==sound::kParameterCount,"frozen identity count");
    require(sound::parameter(sound::ParameterId::oscillator1_semi)==sound::parameter(1),"typed oscillator lookup");
    require(sound::parameter(sound::ParameterId::env4_t4_sync)==sound::parameter(255),"typed sync lookup");
    require(!sound::parameter(static_cast<sound::ParameterId>(256)),"invalid typed lookup");
    std::set<std::string> symbols;std::set<int> fields;std::map<int,std::set<unsigned>> pairs;
    const Program init=makeInitProgram();
    for(size_t i=0;i<expected.size();++i){const auto& f=expected[i];const auto& p=sound::parameters()[i];
        require(p.index==unsigned(n(f,"index"))&&p.index==i,"explicit public index");
        require(p.symbol==f.at("symbol").value&&p.name==f.at("name").value&&p.group==f.at("group").value,"frozen identity strings");
        require(p.field==n(f,"field")&&p.minimum==n(f,"min")&&p.maximum==n(f,"max")&&p.defaultValue==n(f,"default")&&int(p.label)==n(f,"label")&&p.pair==n(f,"pair")&&p.modeField==n(f,"mode"),"frozen binding/domain/default/label/pair");
        static const char* codecs[]={"Direct","Voices","Free","Sync","SyncT4"};require(f.at("codec").value==codecs[int(p.codec)],"frozen codec");
        require(symbols.insert(p.symbol).second,"unique symbol");fields.insert(p.field);
        if(p.pair>=0)pairs[p.pair].insert(p.index);
        auto untouched=init;require(sound::read(p,init)==p.defaultValue,"INIT default "+std::string(p.symbol));require(std::memcmp(&untouched,&init,sizeof init)==0,"read purity");
        require(sound::normalize(p,p.minimum)==0&&sound::normalize(p,p.maximum)==1,"normalized endpoints");
        require(std::abs((sound::normalize(p,p.minimum+1)-sound::normalize(p,p.minimum))-1.0/(p.maximum-p.minimum))<1e-14,"normalized step");
        int v=999;require(!sound::canonicalize(p,std::numeric_limits<double>::infinity(),v)&&v==999,"reject infinity");require(!sound::canonicalize(p,std::numeric_limits<double>::quiet_NaN(),v)&&v==999,"reject NaN");
        require(sound::canonicalize(p,-1e20,v)&&v==p.minimum,"lower clamp");require(sound::canonicalize(p,1e20,v)&&v==p.maximum,"upper clamp");
        for(int x=p.minimum;x<p.maximum;++x){require(sound::canonicalize(p,x+0.5,v),"finite tie");int want=p.kind==sound::Kind::Boolean?p.minimum:(x<0?x:x+1);require(v==want,"ties away zero / boolean midpoint");}
        if(p.kind==sound::Kind::Boolean){require(sound::canonicalize(p,std::nextafter((p.minimum+p.maximum)*0.5,double(p.maximum)),v)&&v==p.maximum,"strict above midpoint");}
        for(int x=p.minimum;x<=p.maximum;++x){require(!sound::choiceLabel(p,x).empty(),"label exists");for(int fill:{0,255,165,90}){Program actual;std::memset(actual.bytes,fill,sizeof actual);Program want=actual;int raw=x;if(p.codec==sound::Codec::SyncT4 && x>=36)raw=(x-36)|64;
            const auto& binding=paramTable().at(size_t(p.field));size_t offset=ofs::Body+binding.offset;
            if(packed(p))want.bytes[offset]=uint8_t((want.bytes[offset]&128)|(raw&127));else if(binding.type < 8) {
                const unsigned mask=((1u<<binding.type)-1u)<<binding.shift;
                want.bytes[offset]=uint8_t((want.bytes[offset]&~mask)|((unsigned(raw)<<binding.shift)&mask));
            } else {
                const unsigned bytes=(binding.type&0x7f)/8;
                for(unsigned byte=0;byte<bytes;++byte)want.bytes[offset+byte]=uint8_t(unsigned(raw)>>(8*byte));
            }
            require(sound::write(p,actual,x),"legal write");require(std::memcmp(&actual,&want,sizeof actual)==0,"unrelated bit/byte preservation "+std::string(p.symbol));require(sound::read(p,actual)==x,"legal round trip "+std::string(p.symbol));
            auto snapshot=actual;require(!sound::write(p,actual,std::numeric_limits<double>::quiet_NaN())&&std::memcmp(&snapshot,&actual,sizeof actual)==0,"nonfinite write purity");
        }}
    }
    require(fields.size()==236&&pairs.size()==20,"audited fields and permanent pairs");for(auto& pair:pairs)require(pair.second.size()==2,"two identities per pair");
    require(!sound::parameter(256)&&!sound::parameterForField(-1),"invalid lookup");
    gui::LcdDisplay display("registry audit");EditBuffer eb(nullptr);gui::LcdController lcd(display,&eb,nullptr);lcd.buildPages();size_t appearance=0;std::set<int> covered;
    for(size_t pg=0;pg<lcd.pages().size();++pg){const auto& page=lcd.pages()[pg];for(size_t sub=0;sub<page.subs.size();++sub){const auto& sp=page.subs[sub];for(const auto& p:sp.params){if(p.ordinal<0)continue;
        require(appearance<fixture.at("appearances").array.size(),"unexpected appearance");const auto& f=fixture.at("appearances").array[appearance++];int field=sp.editBufferIndex(p);
        require(int(pg)==n(f,"page")&&int(sub)==n(f,"sub")&&p.index==n(f,"param")&&field==n(f,"field"),"actual page resolution/alias");require(p.min==n(f,"min")&&p.max==n(f,"max")&&int(p.fmt)==n(f,"label"),"actual panel native domain/formatter");
        require(p.name==f.at("name").value&&p.hint==f.at("hint").value&&sp.title==f.at("group").value,"actual appearance identity");require(getParam(init,field)==n(f,"default"),"raw INIT fixture");
        auto* free=sound::parameterForField(field);auto* sync=sound::parameterForField(field,true);require(free&&sync,"panel covered");covered.insert(field);
        if(free->pair>=0)require(free!=sync&&sync->modeField==free->modeField,"permanent mode resolution");else require(free==sync,"alias resolution independent of mode");
    }}}
    require(appearance==260&&covered==fields,"complete editable coverage; navigation/static excluded");
    for(const auto& b:fixture.at("buttons").object){int field=b.second.number();require(sound::parameterForField(field)!=nullptr,"button alias covered");}
    require(findParam(ofs::Sync-ofs::Body)==n(fixture.at("buttons"),"sync")&&findParam(ofs::AmMod-ofs::Body)==n(fixture.at("buttons"),"am")&&findParam(ofs::Mono-ofs::Body)==n(fixture.at("buttons"),"mono"),"actual direct button byte bindings");
    Program legacy=init;setParam(legacy,375,0);auto saved=legacy;require(sound::read(*sound::parameterForField(375),legacy)==8&&std::memcmp(&legacy,&saved,sizeof legacy)==0,"VOICES0 read without rewrite");
    for(const auto& p:sound::parameters()) if(p.codec==sound::Codec::Sync) for(int raw=0;raw<128;++raw) {
        Program a=init;setParam(a,p.field,raw);auto snapshot=a;
        require(sound::read(p,a)==std::min(raw,35),"sync legacy projection");
        require(std::memcmp(&a,&snapshot,sizeof a)==0,"sync projection never rewrites legacy raw");
    }
    require(sound::normalize(*sound::parameterForField(274),0)==0.5,"pan center");require(sound::normalize(*sound::parameterForField(279),0)==1.0/16,"saturation EMU");
    for(int source=-1;source<=145;++source)require(sound::labels::format(sound::LabelKind::ModSource,source)==sound::labels::kModSourceNames[source+1],"147 source labels");
    for(const auto& p:sound::parameters())if(p.codec==sound::Codec::SyncT4)for(int raw=0;raw<128;++raw){Program a=init;setParam(a,p.field,raw);require(sound::read(p,a)==std::min(raw&63,35)+((raw&64)?36:0),"T4 packed projection");}
    std::cout<<"PASS "<<checks<<" checks; 260 appearances, 236 fields, 256 identities, 20 tempo pairs\n";
    return 0;
}catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<": "<<e.what()<<'\n';return 1;}
