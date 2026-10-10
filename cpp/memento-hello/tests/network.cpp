// Run the production tools and mux against a deterministic Ethernet queue.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>
#include <r2/net.hpp>
#include <r2/syscall.hpp>
#include "../nsk.h"
#include "../netmux.h"

using Frame = std::vector<uint8_t>;
static uint64_t clockMs;
static bool driverActive = true, rejectBind = false, rejectSend = false;
static bool configMissing = false;
static int bindings, releases;
static std::deque<Frame> incoming;
static std::vector<Frame> sent;
static const uint8_t local[4] = {10,3,4,2}, router[4] = {10,3,4,1}, remote[4] = {1,1,1,1};
static const uint8_t mac[6] = {0x52,0x54,0,0x12,0x34,0x56}, peerMac[6] = {0x52,0x54,0,0x12,0x34,0x57};
static void check(bool ok, const char *why) { if (!ok) { std::fprintf(stderr,"FAIL at %llu: %s\n", (unsigned long long)clockMs, why); std::exit(1); } }
static uint16_t get16(const uint8_t *p) { return (p[0]<<8)|p[1]; }
static uint32_t get32(const uint8_t *p) { return (uint32_t)get16(p)<<16|get16(p+2); }
static void put16(uint8_t *p,uint16_t v) { p[0]=v>>8;p[1]=v; }
static void put32(uint8_t *p,uint32_t v) { put16(p,v>>16);put16(p+2,v); }
static uint16_t checksum(const uint8_t *p, size_t n)
{
    uint32_t sum=0; for(size_t i=0;i+1<n;i+=2) sum+=get16(p+i);
    if(n&1) sum+=(uint32_t)p[n-1]<<8;
    while(sum>>16) sum=(sum&65535)+(sum>>16);
    return (uint16_t)~sum;
}
static Frame arpReply(const uint8_t ip[4])
{
    Frame f(42); std::memcpy(f.data(),mac,6);std::memcpy(f.data()+6,peerMac,6);
    put16(f.data()+12,0x806);put16(f.data()+14,1);put16(f.data()+16,0x800);f[18]=6;f[19]=4;put16(f.data()+20,2);
    std::memcpy(f.data()+22,peerMac,6);std::memcpy(f.data()+28,ip,4);
    std::memcpy(f.data()+32,mac,6);std::memcpy(f.data()+38,local,4);return f;
}
static Frame packet(const uint8_t src[4],uint8_t proto,size_t bytes)
{
    Frame f(34+bytes);std::memcpy(f.data(),mac,6);std::memcpy(f.data()+6,peerMac,6);put16(f.data()+12,0x800);
    f[14]=0x45;put16(f.data()+16,20+bytes);f[22]=52;f[23]=proto;
    std::memcpy(f.data()+26,src,4);std::memcpy(f.data()+30,local,4);put16(f.data()+24,checksum(f.data()+14,20));return f;
}
static Frame echoReply(const Frame &probe)
{
    Frame f=packet(probe.data()+30,1,probe.size()-34);
    std::memcpy(f.data()+34,probe.data()+34,probe.size()-34);f[34]=0;put16(f.data()+36,0);put16(f.data()+36,checksum(f.data()+34,f.size()-34));return f;
}
static Frame errorReply(const Frame &probe,const uint8_t src[4],uint8_t type=11)
{
    Frame f=packet(src,1,36);f[34]=type;
    std::memcpy(f.data()+42,probe.data()+14,28);put16(f.data()+36,checksum(f.data()+34,36));return f;
}
static Frame tcpReply(const Frame &probe,uint8_t flags)
{
    Frame f=packet(probe.data()+30,6,20);put16(f.data()+34,80);put16(f.data()+36,NetLink::PORT);
    put32(f.data()+38,100);put32(f.data()+42,get32(probe.data()+38)+1);f[46]=0x50;f[47]=flags;return f;
}
static void advance(NetTools &t,uint64_t ms) { clockMs+=ms; t.step(); }
static Frame lastProbe(uint16_t id)
{
    for(auto i=sent.rbegin();i!=sent.rend();++i)
        if(i->size()>=42&&get16(i->data()+12)==0x800&&(*i)[23]==1&&get16(i->data()+38)==id) return *i;
    check(false,"expected an ICMP probe");return {};
}
namespace r2 {
uint64_t ticks() noexcept { return clockMs; }
void sleep(uint64_t ms) { clockMs+=ms; }
int64_t raw_syscall(Sys sys,int64_t arg1,int64_t arg2,int64_t arg3) noexcept
{
    if(sys==Sys::ReceivePort)
    {
        if(incoming.empty()) return 0;
        Frame f=incoming.front();incoming.pop_front();std::memcpy((void*)arg2,f.data(),f.size());return f.size();
    }
    if(sys==Sys::SendPacket)
    {
        if(rejectSend) return 0xfc;
        check(arg1==4,"raw Ethernet send");sent.emplace_back((uint8_t*)arg2,(uint8_t*)arg2+arg3);return 0;
    }
    if(sys==Sys::NetRegister&&arg2==1)
    {
        check(arg1==NetLink::PORT&&bindings==1,"only the owned scan port released");bindings=0;releases++;return 0;
    }
    if(sys==Sys::SysInfo) { std::memcpy(((SysInfo*)arg2)->ip_addr,local,4);return 0; }
    return 0xfc;
}
namespace heap { void *allocate(size_t size) { return std::malloc(size); } }
namespace net {
std::optional<Status> status() { return Status{{{0x52,0x54,0,0x12,0x34,0x56}},{{10,3,4,2}},driverActive}; }
std::optional<Config> config()
{
    if(configMissing) return {};
    return Config{{{10,3,4,2}},{{255,255,255,0}},{{10,3,4,1}},{{1,1,1,1}},
                  {{0x52,0x54,0,0x12,0x34,0x56}},{{0x52,0x54,0,0x12,0x34,0x57}},true};
}
bool register_driver() { driverActive=true;return true; }
bool bind_port(uint16_t port) { check(port==NetLink::PORT,"tools port");if(rejectBind) return false;bindings=1;return true; }
}
}
extern "C" void net_set_frame_source(int64_t (*)(uint8_t*,uint32_t,uint8_t)) {}
extern "C" void net_set_frame_sink(void (*)(const uint8_t*,uint32_t)) {}

int main()
{
    uint8_t ip[4];
    check(NetLink::parseAddress(" 255.255.255.255 ",ip),"valid IPv4");
    for(const char *s:{"256.1.1.1","1.2.3","1.2.3.4x","1.2.3.4/24","999999999999.1.1.1"})
        check(!NetLink::parseAddress(s,ip),"invalid IPv4 rejected");
    {
        NetTools t;
        check(!t.scan.start(t.link,"0.0.0.0/0"),"oversized range rejected without overflow");t.step();check(!bindings,"parse failure does not bind");
        rejectBind=true;check(!t.scan.start(t.link,"10.3.4.0/30"),"full registry reported");rejectBind=false;t.step();
        check(!bindings,"failed bind not released as an owned port");
    }
    // ARP retries, delayed replies and completion without leaking a slot.
    for(uint64_t delay:{130u,450u,1400u})
    {
        NetTools t;sent.clear();
        check(t.scan.start(t.link,"10.3.4.0/30"),"local scan starts with external driver");
        check(t.scan.arpOnly()&&bindings==1,"ARP-only scan binds one port");
        NetTools second;check(!second.scan.start(second.link,"10.3.4.0/30"),"second window cannot steal tools");
        second.stopAll();check(bindings==1,"closing another window preserves first port");
        int asks=0;size_t seen=0;uint64_t replyAt=0;
        for(int step=0;step<10000&&t.busy();step++)
        {
            t.step();
            for(;seen<sent.size();seen++) if(get16(sent[seen].data()+12)==0x806&&get16(sent[seen].data()+20)==1)
            { asks++;if(asks==3) replyAt=clockMs+delay; }
            if(replyAt&&clockMs>=replyAt) { incoming.push_back(arpReply(router));replyAt=0; }
            clockMs+=10;
        }
        check(!t.busy()&&asks==3&&t.scan.up()==2,"third ARP attempt discovers slow host");
        check(t.scan.host(0).answeredOn==3&&t.scan.host(0).rttMs>=delay,"delayed RTT retained");
        check(!bindings&&!netmux_port_use(NetLink::PORT),"completed scan releases port and note");
    }
    {
        NetTools t;check(!t.scan.start(t.link,"1.1.1.0/24"),"off-link ARP scan rejected");t.step();check(!bindings,"off-link failure releases port");
        check(!t.trace.start(t.link,remote),"trace reports ICMP ownership limitation");t.step();check(!bindings,"trace failure releases port");
        check(t.scan.start(t.link,"10.3.4.0/24"),"scan for shutdown");t.step();
    }
    check(!bindings,"destroying active tools releases port");
    for(int i=0;i<24;i++) { NetTools t;check(t.scan.start(t.link,"10.3.4.0/30"),"window reopened");t.stopAll(); }
    check(!bindings,"repeated close/reopen preserves registry capacity");

    // ARP ping answers its outstanding request; TCP fallback tears down SYN/ACK.
    {
        NetTools t;check(t.ping.start(t.link,router)&&t.ping.mode()==Pinger::ARP,"on-link ARP ping");t.step();
        incoming.push_back(arpReply(router));advance(t,180);
        check(t.ping.lines()==1&&t.ping.line(0).ms==180,"ARP latency");t.stopAll();check(!bindings,"stop ping releases port");
        check(t.ping.start(t.link,remote)&&t.ping.mode()==Pinger::TCP,"off-link TCP ping");t.step();Frame syn=sent.back();
        check(syn[47]==2&&checksum(syn.data()+14,20)==0,"SYN and IPv4 checksum");
        Frame bad=tcpReply(syn,0x12);bad[45]^=1;incoming.push_back(bad);advance(t,130);check(t.ping.lines()==0,"unrelated ACK rejected");
        incoming.push_back(tcpReply(syn,0x12));advance(t,100);
        check(t.ping.lines()==1&&t.ping.line(0).kind==Pinger::L_OPEN&&sent.back()[47]==4,"open host answered and reset");
        advance(t,1000);syn=sent.back();incoming.push_back(tcpReply(syn,0x14));advance(t,150);
        check(t.ping.line(1).kind==Pinger::L_CLOSED,"closed port still proves host alive");
        t.stopAll();check(!bindings,"TCP ping releases port");
    }

    // The mux preserves another stack's traffic, including activation at tick 0.
    {
        uint8_t out[2048];clockMs=0;netmux_forget(NETMUX_WEB);netmux_pull(NETMUX_WEB,out,sizeof(out));
        netmux_claim(NETMUX_WEB,47000,47015);
        Frame tcp=packet(remote,6,20);tcp[46]=0x50;put16(tcp.data()+36,47001);incoming.push_back(tcp);
        check(netmux_pull(NETMUX_NSK,out,sizeof(out))==0,"tools do not consume browser TCP");
        check(netmux_pull(NETMUX_WEB,out,sizeof(out))==(int64_t)tcp.size(),"browser frame stashed even at tick zero");
        incoming.push_back(arpReply(router));netmux_pull(NETMUX_WEB,out,sizeof(out));
        netmux_forget(NETMUX_NSK);check(netmux_pull(NETMUX_NSK,out,sizeof(out))==0,"stop discards queued tool frames");
        NetmuxStats before=netmux_stats();rejectSend=true;check(!netmux_send(tcp.data(),tcp.size()),"failed send reported");rejectSend=false;
        check(netmux_stats().tx_bytes==before.tx_bytes,"failed send not counted");
        netmux_forget(NETMUX_NSK);
    }

    // When Memento is the driver, ICMP replies and router errors reach tools.
    driverActive=false;
    {
        NetTools t;check(t.ping.start(t.link,remote)&&t.ping.mode()==Pinger::ICMP,"ICMP ping with driver");t.step();Frame echo=lastProbe(NETMUX_PING_ICMP_ID);
        check(checksum(echo.data()+34,16)==0,"echo checksum");
        Frame reply=echoReply(echo), bad=reply;bad[41]^=1;incoming.push_back(bad);advance(t,130);check(t.ping.lines()==0,"wrong ICMP sequence ignored");
        incoming.push_back(reply);advance(t,200);check(t.ping.line(0).ms==330,"ICMP latency");
        incoming.push_back(reply);t.step();check(t.ping.lines()==1,"duplicate echo ignored");
        advance(t,1000);echo=lastProbe(NETMUX_PING_ICMP_ID);incoming.push_back(errorReply(echo,router,3));advance(t,130);
        check(t.ping.line(1).kind==Pinger::L_UNREACH,"router unreachable matched to ping");
        advance(t,1000);advance(t,2100);check(t.ping.lines()>=3,"lost echo times out");t.stopAll();check(!bindings,"driver tools need no bound port");
    }
    {
        NetTools t;check(t.scan.start(t.link,"1.1.1.1/32"),"routed ICMP scan");t.step();Frame echo=lastProbe(NETMUX_NSK_ICMP_ID);
        // A UI stall past the wait deadline still drains an already queued reply.
        incoming.push_back(echoReply(echo));advance(t,1700);
        check(t.scan.up()==1,"late scan reply accepted before deadline transition");
        for(int i=0;i<1000&&t.busy();i++) advance(t,10);
        check(!t.busy()&&!bindings,"routed scan completes");
    }
    {
        NetTools t;sent.clear();check(t.trace.start(t.link,remote),"traceroute starts");
        struct Due { uint64_t at;Frame frame; };std::vector<Due> due;size_t seen=0;
        for(int step=0;step<2000&&t.busy();step++)
        {
            t.step();
            for(;seen<sent.size();seen++)
            {
                Frame f=sent[seen];if(f.size()!=50||get16(f.data()+38)!=NETMUX_TRACE_ICMP_ID) continue;
                int ttl=f[22];if(ttl<=3)
                {
                    uint8_t hop[4]={10,0,0,(uint8_t)ttl};Frame reply=ttl==3?echoReply(f):errorReply(f,hop);
                    due.push_back({clockMs+130+(uint64_t)ttl*20,reply});
                }
            }
            for(auto i=due.begin();i!=due.end();) if(i->at<=clockMs) { incoming.push_back(i->frame);i=due.erase(i); } else ++i;
            clockMs+=10;
        }
        check(!t.busy()&&t.trace.hops()==3,"trace terminates at destination");
        check(t.trace.hop(2).flags&Tracer::H_DEST,"destination marked");
        for(int h=0;h<3;h++) check(t.trace.hop(h).answered==7&&t.trace.hop(h).ms[0]>=130,"all three delayed hop probes recorded");
    }
    {
        NetTools t;check(t.trace.start(t.link,remote),"silent traceroute starts");
        for(int i=0;i<10000&&t.busy();i++) advance(t,10);
        check(!t.busy()&&t.trace.hops()==Tracer::MAX_HOPS,"silent path has bounded completion");
    }
    {
        NetTools t;check(t.ping.start(t.link,remote),"long ping starts");
        for(int i=0;i<256;i++)
        {
            t.step();incoming.push_back(echoReply(lastProbe(NETMUX_PING_ICMP_ID)));t.step();advance(t,1000);
        }
        Frame delayed=echoReply(lastProbe(NETMUX_PING_ICMP_ID));t.stopAll();
        check(t.ping.start(t.link,remote),"long ping restarted");t.step();
        incoming.push_back(delayed);advance(t,130);
        check(t.ping.lines()==0,"late reply from previous ping run rejected");
        incoming.push_back(echoReply(lastProbe(NETMUX_PING_ICMP_ID)));advance(t,130);
        check(t.ping.lines()==1&&t.ping.line(0).seq==1,"restarted ping starts its display sequence at one");t.stopAll();
    }
    {
        configMissing=true;NetTools t;sent.clear();
        check(t.ping.start(t.link,remote),"ping on a legacy/unconfigured kernel");t.step();
        check(sent.back().size()==42&&!std::memcmp(sent.back().data()+38,router,4),"fallback route resolves local .1 gateway");
        incoming.push_back(arpReply(router));advance(t,450);
        Frame echo=lastProbe(NETMUX_PING_ICMP_ID);incoming.push_back(echoReply(echo));advance(t,130);
        check(t.ping.lines()==1&&t.ping.line(0).seq==1,"fallback gateway ping succeeds with sequence one");
        t.stopAll();configMissing=false;
    }
    std::printf("Network tools tests passed (slow replies, routing, ping, trace, %d releases).\n",releases);
}
