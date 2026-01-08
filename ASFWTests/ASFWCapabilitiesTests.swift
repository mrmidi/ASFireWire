
import Testing
@testable import ASFW
import Foundation

struct ASFWCapabilitiesTests {

    /// Build header (18 bytes per SharedDataModels.hpp)
    private func buildHeader(flags: UInt8, currentRate: UInt8, 
                            audioIn: UInt8, audioOut: UInt8,
                            midiIn: UInt8, midiOut: UInt8,
                            numPlugs: UInt8) -> Data {
        var data = Data()
        data.append(flags)       // 0: flags
        data.append(currentRate) // 1: currentRate
        data.append(contentsOf: [0x04, 0x00, 0x00, 0x00]) // 2-5: supportedRatesMask
        data.append(contentsOf: [0x00, 0x00])             // 6-7: padding
        data.append(audioIn)     // 8
        data.append(audioOut)    // 9
        data.append(midiIn)      // 10
        data.append(midiOut)     // 11
        data.append(0)           // 12: smpteIn
        data.append(0)           // 13: smpteOut
        data.append(numPlugs)    // 14
        data.append(0)           // 15: reserved
        data.append(contentsOf: [0x00, 0x00]) // 16-17: padding2
        return data
    }
    
    /// Build PlugInfoWire (40 bytes)
    private func buildPlug(id: UInt8, isInput: Bool, type: UInt8, 
                          name: String, numBlocks: UInt8, numSupportedFormats: UInt8 = 0) -> Data {
        var data = Data()
        data.append(id)               // 0
        data.append(isInput ? 1 : 0)  // 1
        data.append(type)             // 2
        data.append(numBlocks)        // 3
        data.append(UInt8(name.count)) // 4
        var nameBytes = [UInt8](repeating: 0, count: 32)
        name.utf8.prefix(32).enumerated().forEach { nameBytes[$0] = $1 }
        data.append(contentsOf: nameBytes) // 5-36
        data.append(numSupportedFormats)    // 37: numSupportedFormats
        data.append(contentsOf: [0x00, 0x00]) // 38-39: padding
        return data
    }
    
    /// Build SignalBlockWire (4 bytes)
    private func buildSignalBlock(formatCode: UInt8, channelCount: UInt8, 
                                  numChannelDetails: UInt8) -> Data {
        var data = Data()
        data.append(formatCode)
        data.append(channelCount)
        data.append(numChannelDetails)
        data.append(0) // padding
        return data
    }
    
    /// Build ChannelDetailWire (36 bytes)
    private func buildChannelDetail(musicPlugID: UInt16, position: UInt8, name: String) -> Data {
        var data = Data()
        data.append(UInt8(musicPlugID & 0xFF))        // LE
        data.append(UInt8((musicPlugID >> 8) & 0xFF))
        data.append(position)
        data.append(UInt8(name.count))
        var nameBytes = [UInt8](repeating: 0, count: 32)
        name.utf8.prefix(32).enumerated().forEach { nameBytes[$0] = $1 }
        data.append(contentsOf: nameBytes)
        return data
    }

    @Test func testMusicCapabilitiesParsing() async throws {
        var data = Data()

        // Header (18 bytes): Audio + MIDI, rate=0x02
        data.append(contentsOf: buildHeader(
            flags: 0x03,  // Audio + MIDI
            currentRate: 0x02,
            audioIn: 2, audioOut: 2,
            midiIn: 1, midiOut: 1,
            numPlugs: 2
        ))
        #expect(data.count == 18)

        // Plug 0 (Audio Input) with 1 signal block
        data.append(contentsOf: buildPlug(id: 0, isInput: true, type: 0x00, name: "Analog In 1", numBlocks: 1))
        data.append(contentsOf: buildSignalBlock(formatCode: 0x06, channelCount: 1, numChannelDetails: 0))
        
        // Plug 1 (MIDI Output) with 0 signal blocks
        data.append(contentsOf: buildPlug(id: 1, isInput: false, type: 0x01, name: "MIDI Out", numBlocks: 0))

        // Parse
        let caps = ASFWDriverConnector.AVCMusicCapabilities(data: data)
        #expect(caps != nil, "Parser returned nil")
        
        guard let c = caps else { return }
        
        // Verify Header
        #expect(c.hasAudioCapability == true)
        #expect(c.hasMidiCapability == true)
        #expect(c.currentRate == 0x02)
        #expect(c.audioInputPorts == 2)
        
        // Verify Plugs
        #expect(c.plugs.count == 2)
        
        // Plug 0
        let p0 = c.plugs[0]
        #expect(p0.plugID == 0)
        #expect(p0.isInput == true)
        #expect(p0.type == 0x00) // Audio
        #expect(p0.name == "Analog In 1")
        #expect(p0.signalBlocks.count == 1)
        #expect(p0.signalBlocks[0].formatCode == 0x06)
        
        // Plug 1
        let p1 = c.plugs[1]
        #expect(p1.plugID == 1)
        #expect(p1.isInput == false)
        #expect(p1.type == 0x01) // MIDI
        #expect(p1.name == "MIDI Out")
        #expect(p1.signalBlocks.count == 0)
    }
}
