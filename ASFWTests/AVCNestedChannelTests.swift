
import Testing
@testable import ASFW
import Foundation

/// Tests for the new nested channel wire format where channels are inside signal blocks
struct AVCNestedChannelTests {

    /// Helper to build header (18 bytes per SharedDataModels.hpp)
    private func buildHeader(audioIn: UInt8 = 2, audioOut: UInt8 = 2,
                            currentRate: UInt8 = 0x03, numPlugs: UInt8 = 1) -> Data {
        var data = Data()
        // Byte 0: flags (hasAudio=bit0, hasMIDI=bit1, hasSMPTE=bit2)
        data.append(0x01) // hasAudio only
        // Byte 1: currentRate
        data.append(currentRate)
        // Bytes 2-5: supportedRatesMask (little-endian UInt32)
        data.append(contentsOf: [0x38, 0x04, 0x00, 0x00]) // 0x438 = 44.1k, 48k, 88.2k, 96k
        // Bytes 6-7: padding
        data.append(contentsOf: [0x00, 0x00])
        // Bytes 8-13: port counts
        data.append(audioIn)     // 8
        data.append(audioOut)    // 9
        data.append(0)           // 10 midiIn
        data.append(0)           // 11 midiOut
        data.append(0)           // 12 smpteIn
        data.append(0)           // 13 smpteOut
        // Byte 14: numPlugs
        data.append(numPlugs)
        // Byte 15: reserved
        data.append(0)
        // Bytes 16-17: padding2
        data.append(contentsOf: [0x00, 0x00])
        return data
    }
    
    /// Build PlugInfoWire (40 bytes)
    private func buildPlug(id: UInt8, isInput: Bool, type: UInt8 = 0, 
                          name: String, numBlocks: UInt8, numSupportedFormats: UInt8 = 0) -> Data {
        var data = Data()
        // Byte 0: plugID
        data.append(id)
        // Byte 1: isInput
        data.append(isInput ? 1 : 0)
        // Byte 2: type
        data.append(type)
        // Byte 3: numSignalBlocks
        data.append(numBlocks)
        // Byte 4: nameLength
        data.append(UInt8(name.count))
        // Bytes 5-36: name[32]
        var nameBytes = [UInt8](repeating: 0, count: 32)
        let nameData = name.data(using: .utf8) ?? Data()
        for (i, byte) in nameData.prefix(32).enumerated() {
            nameBytes[i] = byte
        }
        data.append(contentsOf: nameBytes)
        // Byte 37: numSupportedFormats
        data.append(numSupportedFormats)
        // Bytes 38-39: padding[2]
        data.append(contentsOf: [0, 0])
        return data
    }
    
    /// Build SignalBlockWire (4 bytes)
    private func buildSignalBlock(formatCode: UInt8, channelCount: UInt8, 
                                  numChannelDetails: UInt8) -> Data {
        var data = Data()
        data.append(formatCode)         // 0
        data.append(channelCount)       // 1
        data.append(numChannelDetails)  // 2
        data.append(0)                  // 3: padding
        return data
    }
    
    /// Build ChannelDetailWire (36 bytes)
    /// Note: Swift parser expects musicPlugID as little-endian
    private func buildChannelDetail(musicPlugID: UInt16, position: UInt8, name: String) -> Data {
        var data = Data()
        // Bytes 0-1: musicPlugID (little-endian per Swift parser)
        data.append(UInt8(musicPlugID & 0xFF))
        data.append(UInt8((musicPlugID >> 8) & 0xFF))
        // Byte 2: position
        data.append(position)
        // Byte 3: nameLength
        data.append(UInt8(name.count))
        // Bytes 4-35: name[32]
        var nameBytes = [UInt8](repeating: 0, count: 32)
        let nameData = name.data(using: .utf8) ?? Data()
        for (i, byte) in nameData.prefix(32).enumerated() {
            nameBytes[i] = byte
        }
        data.append(contentsOf: nameBytes)
        return data
    }

    @Test func testNestedChannelParsing() async throws {
        var data = Data()
        
        // Header (18 bytes)
        data.append(contentsOf: buildHeader())
        #expect(data.count == 18, "Header should be 18 bytes, got \(data.count)")
        
        // Plug (40 bytes)
        data.append(contentsOf: buildPlug(id: 0, isInput: true, name: "Analog Out", numBlocks: 1))
        #expect(data.count == 58, "After plug should be 58 bytes, got \(data.count)")
        
        // Signal Block (4 bytes) with 2 channel details
        data.append(contentsOf: buildSignalBlock(formatCode: 0x06, channelCount: 2, numChannelDetails: 2))
        #expect(data.count == 62, "After signal block should be 62 bytes, got \(data.count)")
        
        // Channel Details (36 bytes each)
        data.append(contentsOf: buildChannelDetail(musicPlugID: 0, position: 0, name: "Analog Out 1"))
        data.append(contentsOf: buildChannelDetail(musicPlugID: 1, position: 1, name: "Analog Out 2"))
        #expect(data.count == 134, "Final size should be 134 bytes, got \(data.count)")
        
        // Parse
        let caps = ASFWDriverConnector.AVCMusicCapabilities(data: data)
        #expect(caps != nil, "Parser returned nil for \(data.count) bytes")
        
        guard let c = caps else { return }
        
        #expect(c.hasAudioCapability == true)
        #expect(c.currentRate == 0x03)
        #expect(c.plugs.count == 1, "Expected 1 plug, got \(c.plugs.count)")
        
        let p0 = c.plugs[0]
        #expect(p0.name == "Analog Out", "Plug name mismatch: '\(p0.name)'")
        #expect(p0.signalBlocks.count == 1, "Expected 1 block, got \(p0.signalBlocks.count)")
        
        let block = p0.signalBlocks[0]
        #expect(block.formatCode == 0x06)
        #expect(block.channelCount == 2)
        #expect(block.channels.count == 2, "Expected 2 channels, got \(block.channels.count)")
        
        #expect(block.channels[0].musicPlugID == 0)
        #expect(block.channels[0].name == "Analog Out 1")
        #expect(block.channels[1].musicPlugID == 1)
        #expect(block.channels[1].name == "Analog Out 2")
    }
    
    @Test func testEmptyChannelsInSignalBlock() async throws {
        var data = Data()
        
        // Header (18 bytes) + Plug (40 bytes) + Signal Block with 0 channel details (4 bytes)
        data.append(contentsOf: buildHeader())
        data.append(contentsOf: buildPlug(id: 0, isInput: true, name: "Test", numBlocks: 1))
        data.append(contentsOf: buildSignalBlock(formatCode: 0x06, channelCount: 2, numChannelDetails: 0))
        
        let caps = ASFWDriverConnector.AVCMusicCapabilities(data: data)
        #expect(caps != nil)
        
        guard let c = caps else { return }
        #expect(c.plugs.count == 1)
        #expect(c.plugs[0].signalBlocks.count == 1)
        #expect(c.plugs[0].signalBlocks[0].channels.isEmpty)
    }
}
