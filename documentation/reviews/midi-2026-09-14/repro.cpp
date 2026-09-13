#include <array>
#include <cstdio>
#include "ASFWDriver/Midi/Transport/MidiTransportBlock.hpp"
#include "ASFWDriver/Midi/Ump/MidiByteStreamToUmp.hpp"
#include "ASFWDriver/Midi/Ump/UmpToMidiByteStream.hpp"
#include "ASFWDriver/Audio/Engine/Direct/Rx/RxAudioPacketProcessor.hpp"
using namespace ASFW::Midi;
using namespace ASFW::Midi::Ump;
struct Sink : ASFW::Audio::Ports::IMidiByteSink {
 unsigned delivered=0;
 void DeliverMidiBytes(uint8_t,const uint8_t*,uint8_t n) noexcept override {delivered+=n;}
 void MarkMidiDiscontinuity(uint8_t) noexcept override {}
};
void be(uint8_t* p,uint32_t w){p[0]=w>>24;p[1]=w>>16;p[2]=w>>8;p[3]=w;}
int main(){
 MidiByteRing ring; MidiByteStreamToUmp parser;
 uint8_t prefix[]{0x90,0x3c}; (void)ring.TryWrite(prefix);
 uint64_t seen=0; auto gaps=ring.discontinuities.load(std::memory_order_relaxed);
 if(gaps!=seen){ seen=gaps; ring.Consume(ring.Available()); parser.Reset(); }
 // Producer runs after the service's one gap check, before its next Peek.
 ring.MarkDiscontinuity(); uint8_t suffix[]{0x40}; (void)ring.TryWrite(suffix);
 uint8_t raw[1024]{}; uint32_t words[3072]{};
 auto n=ring.Peek(raw); auto r=parser.Push({raw,n},words);
 std::printf("gap during drain: %u UMP word(s), 0x%08x\n",r.wordsWritten,words[0]);
 using namespace ASFW::AudioEngine::Direct;
 DirectInputWriter writer; Rx::RxAudioPacketProcessor processor(writer); Sink sink;
 std::array<uint8_t,24> packet{};
 be(packet.data()+8,2u<<16); // DBS2, DBC0
 be(packet.data()+12,0x90ffffff); // FMT10, FDFff, SYTffff
 be(packet.data()+20,0x81900000); // last slot has a MIDI-looking byte
 Rx::RxMidiExtraction extraction{.sink=&sink,.geometry={.dbs=2,.midiSlotIndex=1,.portCount=1,.dbcAligned=true}};
 auto result=processor.ProcessPacket(packet.data(),packet.size(),0,1,2,ASFW::Encoding::AudioWireFormat::kAM824,0,false,{},false,extraction);
 std::printf("FMT10/FDFff NO-DATA: MIDI bytes delivered=%u\n",result.midiBytesDelivered);
 UmpToMidiByteStream converter; std::array<uint32_t,256> input{}; input.fill(0x20903c40); uint8_t tx[512]{};
 unsigned consumed=0, produced=0;
 while(consumed<input.size()){auto p=converter.Pull({input.data()+consumed,input.size()-consumed},tx); if(!p.wordsConsumed)break; consumed+=p.wordsConsumed; produced+=p.bytesWritten;}
 std::printf("updated TX loop: %u/256 words, %u bytes\n",consumed,produced);
}
