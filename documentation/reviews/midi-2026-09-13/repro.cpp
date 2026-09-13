#include <array>
#include <cstdio>
#include "ASFWDriver/Midi/Ump/MidiByteStreamToUmp.hpp"
#include "ASFWDriver/Midi/Ump/UmpToMidiByteStream.hpp"
#include "ASFWDriver/Midi/Transport/MidiTransportBlock.hpp"
using namespace ASFW::Midi;
using namespace ASFW::Midi::Ump;
int main(int argc, char**) {
  if (argc > 1) {
    MidiByteStreamToUmp parser;
    uint32_t setup[8]{}; uint8_t start[]{0xf0,1};
    (void)parser.Push(start, setup);
    uint32_t out[2]{}; uint8_t tune[]{0xf6};
    auto r=parser.Push(tune,out);
    std::printf("two-word span: wrote %u words\n",r.wordsWritten);
    return 0;
  }
  UmpToMidiByteStream converter;
  std::array<uint32_t,256> messages{}; messages.fill(0x20903c40);
  uint8_t bytes[512]{};
  auto r=converter.Pull(messages,bytes);
  std::printf("callback-sized scratch: consumed %u / 256 words; %u bytes\n",r.wordsConsumed,r.bytesWritten);
  UmpToMidiByteStream sysex;
  MidiByteRing fullRing; std::array<uint8_t,1024> full{};
  (void)fullRing.TryWrite(full);
  uint32_t sysexStart[]{0x30110100,0};
  auto startResult=sysex.Pull(sysexStart,bytes);
  bool accepted=fullRing.TryWrite({bytes,startResult.bytesWritten});
  fullRing.Consume(1024);
  uint32_t sysexEnd[]{0x30310200,0};
  auto endResult=sysex.Pull(sysexEnd,bytes);
  std::printf("rejected SysEx start=%d; next end emits %u bytes: %02x %02x (no F0)\n",!accepted,endResult.bytesWritten,bytes[0],bytes[1]);
  MidiByteRing ring; MidiByteStreamToUmp parser;
  uint8_t prefix[]{0x90,0x3c}; (void)ring.TryWrite(prefix);
  ring.MarkDiscontinuity(); // e.g. dropped velocity byte before next message
  parser.Reset(); // DrainReceiveRings resets BEFORE draining pre-gap bytes
  uint8_t data[1024]{}; uint32_t words[2048]{};
  auto n=ring.Peek(data); auto p=parser.Push({data,n},words); ring.Consume(p.bytesConsumed);
  uint8_t postGap[]{0x40}; (void)ring.TryWrite(postGap);
  n=ring.Peek(data); p=parser.Push({data,n},words);
  std::printf("after gap: emitted %u word(s), first=0x%08x\n",p.wordsWritten,words[0]);
}
