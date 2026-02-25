//
// MusicSubunit.cpp
// ASFWDriver - AV/C Protocol Layer
//
// Music Subunit implementation (Audio/MIDI interfaces)
//

#include "MusicSubunit.hpp"
#include "../AVCUnit.hpp"
#include "../../../Logging/Logging.hpp"
#include "../StreamFormats/AVCStreamFormatCommands.hpp"
#include "../StreamFormats/AVCSignalSourceCommand.hpp"
#include "../AudioFunctionBlockCommand.hpp"
#include "../StreamFormats/StreamFormatParser.hpp"
#include "../Descriptors/AVCDescriptorCommands.hpp"
#include "../Descriptors/DescriptorAccessor.hpp"
#include <cctype>
#include <algorithm>
#include <cstdio>

namespace ASFW::Protocols::AVC::Music {

//==============================================================================
// Helper Functions for Big-Endian Reads
//==============================================================================

namespace {
    inline uint16_t ReadBE16(const uint8_t* data) {
        return (static_cast<uint16_t>(data[0]) << 8) | data[1];
    }
    
    inline uint32_t ReadBE32(const uint8_t* data) {
        return (static_cast<uint32_t>(data[0]) << 24) |
               (static_cast<uint32_t>(data[1]) << 16) |
               (static_cast<uint32_t>(data[2]) << 8) |
                data[3];
    }
} // namespace

MusicSubunit::MusicSubunit(AVCSubunitType type, uint8_t id)
    : Subunit(type, id) {
    ASFW_LOG_V3(MusicSubunit, "MusicSubunit created: type=0x%02x id=%d",
                   static_cast<uint8_t>(type), id);
}

// ...

void MusicSubunit::ParseCapabilities(AVCUnit& unit, std::function<void(bool)> completion) {
    ASFW_LOG_V1(MusicSubunit, "MusicSubunit: Parsing capabilities...");

    statusDescriptorReadOk_ = false;
    statusDescriptorParsedOk_ = false;
    statusDescriptorHasRouting_ = false;
    statusDescriptorHasClusterInfo_ = false;
    statusDescriptorHasPlugs_ = false;
    statusDescriptorExpectedPlugCount_ = 0;
    musicChannels_.clear();
    plugs_.clear();

    // CRITICAL: Capture shared_ptr to AVCUnit to keep FCPTransport alive during async operations.
    // The DescriptorAccessor stores FCPTransport& as a reference, so the AVCUnit (which owns
    // the FCPTransport via OSSharedPtr) must stay alive until all callbacks complete.
    // Without this, the FCPTransport reference becomes dangling after OPEN completes but
    // before READ is issued, causing a null pointer crash in FCPTransport::SubmitCommand.
    auto unitPtr = unit.shared_from_this();
    auto accessor = std::make_shared<DescriptorAccessor>(unit.GetFCPTransport(), GetAddress());

    // Define specifier for Music Subunit Status Descriptor (0x80)
    // Note: Apple driver uses 0x80 (Status Descriptor) for Music Subunit discovery, not 0x00 (Identifier)
    DescriptorSpecifier specifier;
    specifier.type = static_cast<DescriptorSpecifierType>(0x80); // Status Descriptor
    specifier.typeSpecificFields = {};

    // 1. Try Standard Sequence (OPEN -> READ -> CLOSE)
    accessor->readWithOpenCloseSequence(specifier, [this, unitPtr, accessor, specifier, completion](const DescriptorAccessor::ReadDescriptorResult& result) {
        if (result.success && !result.data.empty()) {
            ASFW_LOG_V1(MusicSubunit, "MusicSubunit: Standard OPEN-READ-CLOSE succeeded (%zu bytes)", result.data.size());
            statusDescriptorReadOk_ = true;
            statusDescriptorData_ = result.data; // Store raw data
            ParseDescriptorBlock(result.data.data(), result.data.size());
            ParseSignalFormats(*unitPtr, completion);
        } else {
            // 2. Fallback: Non-Standard Direct Read (Skip OPEN)
            ASFW_LOG_V1(MusicSubunit, "MusicSubunit: Standard descriptor access failed (result=%d). Trying Non-Standard Direct Read...", 
                           static_cast<int>(result.avcResult));
            
            accessor->readComplete(specifier, [this, unitPtr, completion](const DescriptorAccessor::ReadDescriptorResult& fallbackResult) {
                if (fallbackResult.success && !fallbackResult.data.empty()) {
                    ASFW_LOG_V1(MusicSubunit, "MusicSubunit: Non-Standard Direct Read SUCCEEDED (%zu bytes)", fallbackResult.data.size());
                    statusDescriptorReadOk_ = true;
                    statusDescriptorData_ = fallbackResult.data; // Store raw data
                    ParseDescriptorBlock(fallbackResult.data.data(), fallbackResult.data.size());
                } else {
                    ASFW_LOG_V0(MusicSubunit, "MusicSubunit: Non-Standard Direct Read also failed (result=%d). Capabilities may be incomplete.", 
                                 static_cast<int>(fallbackResult.avcResult));
                }
                
                // Proceed to signal formats regardless of descriptor success
                ParseSignalFormats(*unitPtr, completion);
            });
        }
    });
}

void MusicSubunit::ParseSignalFormats(AVCUnit& unit, std::function<void(bool)> completion) {
    // Use comprehensive Stream Format Support command (0xBF) instead of legacy Signal Format (0xA0/0xA1).
    // The legacy commands are often not implemented or are unit-level only.
    ASFW_LOG_V1(MusicSubunit, "MusicSubunit: Querying stream formats (using 0xBF/0x2F)...");
    QueryPlugFormats(unit, 0, completion);
}

void MusicSubunit::QueryPlugFormats(AVCUnit& unit, size_t plugIndex, std::function<void(bool)> completion) {
    using namespace StreamFormats;

    // Done with all plugs?
    if (plugIndex >= plugs_.size()) {
        // Next step: Query supported formats
        QuerySupportedFormats(unit, [this, &unit, completion](bool success) {
            // Next step: Query connections
            QueryConnections(unit, [this, &unit, completion](bool success) {
                // Final step: Parse names (already done via descriptors, just logging)
                ParsePlugNames(unit, completion);
            });
        });
        return;
    }

    auto& plug = plugs_[plugIndex];

    // Query current stream format for this plug (subfunction 0xC0)
    auto cmd = std::make_shared<AVCStreamFormatCommand>(
        unit,
        GetAddress(),
        plug.plugID,
        plug.IsInput()
    );

    cmd->Submit([this, &unit, plugIndex, completion](AVCResult result, const std::optional<AudioStreamFormat>& format) {
        if (IsSuccess(result) && format) {
            // Successfully parsed format with StreamFormatParser
            // IMPORTANT: Preserve channel details from descriptor parsing (ClusterInfo)
            // The descriptor provides musicPlugID->name mappings via ClusterInfo blocks
            std::vector<ChannelFormatInfo> preservedChannelFormats;
            if (plugs_[plugIndex].currentFormat) {
                preservedChannelFormats = plugs_[plugIndex].currentFormat->channelFormats;
            }
            
            // Update with new format from AV/C query
            plugs_[plugIndex].currentFormat = *format;
            
            // Merge preserved channel details into the new format
            // If descriptor had channel info, use it; otherwise keep what AV/C query provided
            if (!preservedChannelFormats.empty()) {
                // Copy channel details (musicPlugID, position, name) from preserved formats
                for (size_t i = 0; i < std::min(preservedChannelFormats.size(), 
                                                  plugs_[plugIndex].currentFormat->channelFormats.size()); ++i) {
                    plugs_[plugIndex].currentFormat->channelFormats[i].channels = 
                        std::move(preservedChannelFormats[i].channels);
                }
                
                // If AV/C query returned fewer channelFormats, append the rest from preserved
                for (size_t i = plugs_[plugIndex].currentFormat->channelFormats.size(); 
                     i < preservedChannelFormats.size(); ++i) {
                    plugs_[plugIndex].currentFormat->channelFormats.push_back(
                        std::move(preservedChannelFormats[i]));
                }
            }
            
            // Calculate actual channel count:
            // - For compound formats, sum up channelFormats[i].channelCount
            // - For simple formats, use totalChannels (defaults to 2)
            uint32_t channelCount = 0;
            if (format->IsCompound() && !format->channelFormats.empty()) {
                for (const auto& cf : format->channelFormats) {
                    channelCount += cf.channelCount;
                }
            } else {
                channelCount = format->totalChannels;
            }
            
            ASFW_LOG_V1(MusicSubunit, "MusicSubunit: Plug %u (%{public}s) current format: rate=%u Hz, channels=%u",
                         plugs_[plugIndex].plugID,
                         plugs_[plugIndex].IsInput() ? "in" : "out",
                         format->GetSampleRateHz(),
                         channelCount);

            // SYNTHESIZE CHANNELS if missing
            // Many devices don't provide explicit MusicPlugInfo (0x810B) blocks for channel names.
            // We must create them based on the format to ensure the GUI displays them.
            bool hasChannels = false;
            for (const auto& ch : musicChannels_) {
                if (ch.musicPlugID == plugs_[plugIndex].plugID) {
                    hasChannels = true;
                    break;
                }
            }

            if (!hasChannels) {
                size_t totalCh = format->totalChannels;
                ASFW_LOG_V1(MusicSubunit, "Synthesizing %zu channels for Plug %u", totalCh, plugs_[plugIndex].plugID);
                
                uint8_t portType = 0x00; // Default to Audio?
                if (plugs_[plugIndex].type == MusicPlugType::kMIDI) portType = 0x01;
                else if (plugs_[plugIndex].type == MusicPlugType::kSync) portType = 0x80;
                
                for (size_t i = 0; i < totalCh; ++i) {
                    MusicPlugChannel ch;
                    ch.musicPlugID = plugs_[plugIndex].plugID;
                    ch.portType = portType;
                    // Generate nice name: "Channel 1", "Channel 2"
                    char nameBuf[32];
                    snprintf(nameBuf, sizeof(nameBuf), "Channel %zu", i + 1);
                    ch.name = nameBuf;
                    musicChannels_.push_back(ch);
                }
            }

        } else {
            ASFW_LOG_V3(MusicSubunit, "MusicSubunit: Plug %u format query failed or not implemented",
                          plugs_[plugIndex].plugID);
        }

        // Continue to next plug
        QueryPlugFormats(unit, plugIndex + 1, completion);
    });
}

void MusicSubunit::QuerySupportedFormats(ASFW::Protocols::AVC::IAVCCommandSubmitter& submitter, std::function<void(bool)> completion) {
    using namespace StreamFormats;

    // Helper to recursively query supported formats for each plug
    struct QueryState {
        size_t plugIndex{0};
        std::function<void(bool)> completion;
    };

    auto state = std::make_shared<QueryState>();
    state->completion = completion;

    // Lambda to query next plug
    // Use shared_ptr to allow capturing itself
    auto queryNextPlug = std::make_shared<std::function<void()>>();
    
    *queryNextPlug = [this, &submitter, state, queryNextPlug]() {
        // Done with all plugs?
        if (state->plugIndex >= plugs_.size()) {
            ASFW_LOG_V1(MusicSubunit, "MusicSubunit: Supported format enumeration complete");
            state->completion(true);
            return;
        }

        auto& plug = plugs_[state->plugIndex];
        size_t currentPlugIndex = state->plugIndex;

        ASFW_LOG_V3(MusicSubunit, "MusicSubunit: Querying supported formats for plug %u (%{public}s)",
                      plug.plugID, plug.IsInput() ? "in" : "out");

        // Use QueryAllSupportedFormats helper to enumerate all supported formats
        QueryAllSupportedFormats(
            submitter,
            GetAddress(),
            plug.plugID,
            plug.IsInput(),
            [this, currentPlugIndex, state, queryNextPlug](std::vector<AudioStreamFormat> formats) {
                if (!formats.empty()) {
                    plugs_[currentPlugIndex].supportedFormats = std::move(formats);
                    ASFW_LOG_V1(MusicSubunit, "MusicSubunit: Plug %u supports %zu formats",
                                 plugs_[currentPlugIndex].plugID,
                                 plugs_[currentPlugIndex].supportedFormats.size());
                } else {
                    ASFW_LOG_V3(MusicSubunit, "MusicSubunit: Plug %u has no supported formats or command not implemented",
                                  plugs_[currentPlugIndex].plugID);
                }

                // Move to next plug
                state->plugIndex++;
                (*queryNextPlug)();
            },
            16  // Max 16 format iterations per plug
        );
    };

    // Start querying
    (*queryNextPlug)();
}

void MusicSubunit::QueryConnections(ASFW::Protocols::AVC::IAVCCommandSubmitter& submitter, std::function<void(bool)> completion) {
    using namespace StreamFormats;

    // Helper to recursively query connections for each destination (input) plug
    struct QueryState {
        size_t plugIndex{0};
        std::function<void(bool)> completion;
        std::function<void()> queryNext; // Recursive function

        void Advance() {
            plugIndex++;
            if (queryNext) {
                queryNext();
            }
        }
    };

    auto state = std::make_shared<QueryState>();
    state->completion = completion;

    // Define the recursive function
    state->queryNext = [this, &submitter, state]() {
        // Done with all plugs?
        if (state->plugIndex >= plugs_.size()) {
            ASFW_LOG_V1(MusicSubunit, "MusicSubunit: Connection topology query complete");
            auto completion = state->completion;
            state->queryNext = nullptr; // Break reference cycle
            completion(true);
            return;
        }

        auto& plug = plugs_[state->plugIndex];
        size_t currentPlugIndex = state->plugIndex;



        // Only query connection topology for destination (input) plugs
        // Source plugs don't have connections TO them, they have connections FROM them
        if (!plug.IsInput()) {
            state->plugIndex++;
            state->queryNext();
            return;
        }

        ASFW_LOG_V3(MusicSubunit, "MusicSubunit: Querying connection for destination plug %u",
                      plug.plugID);

        // Query SIGNAL SOURCE for this destination plug
        auto cmd = std::make_shared<AVCSignalSourceCommand>(
            submitter,
            GetAddress(),
            plug.plugID,
            true  // isSubunitPlug
        );

        cmd->Submit([this, currentPlugIndex, state, &submitter](AVCResult result, const ConnectionInfo& connInfo) {
            if (IsSuccess(result)) {
                plugs_[currentPlugIndex].connectionInfo = connInfo;
                LogConnection(currentPlugIndex, connInfo);
                state->Advance();
            } else if (result == AVCResult::kNotImplemented) {
                // Device might support SIGNAL SOURCE at the Unit level instead of Subunit level
                // (e.g., Apogee Duet). Retry targeting the Unit.
                ASFW_LOG_V3(MusicSubunit, "MusicSubunit: Subunit SIGNAL SOURCE not implemented, retrying with Unit address");

                auto unitCmd = std::make_shared<AVCSignalSourceCommand>(
                    submitter,
                    kAVCSubunitUnit, // Target the Unit (0xFF)
                    plugs_[currentPlugIndex].plugID,
                    true  // Still asking about a Subunit Plug
                );

                unitCmd->Submit([this, currentPlugIndex, state](AVCResult unitResult, const ConnectionInfo& unitConnInfo) {
                    if (IsSuccess(unitResult)) {
                        plugs_[currentPlugIndex].connectionInfo = unitConnInfo;
                        LogConnection(currentPlugIndex, unitConnInfo);
                    } else {
                        ASFW_LOG_V3(MusicSubunit, "MusicSubunit: Connection query failed for plug %u (Unit retry result: %d)",
                                      plugs_[currentPlugIndex].plugID, static_cast<int>(unitResult));
                    }
                    state->Advance();
                });
            } else {
                ASFW_LOG_V3(MusicSubunit, "MusicSubunit: Connection query failed for plug %u (Result: %d)",
                              plugs_[currentPlugIndex].plugID, static_cast<int>(result));
                state->Advance();
            }
        });
    };

    // Start querying
    state->queryNext();
}

void MusicSubunit::ParsePlugNames(AVCUnit& unit, std::function<void(bool)> completion) {
    // Plug names are parsed from the descriptor in ParseDescriptorBlock.
    // No additional commands needed if the descriptor was successfully read.

    ASFW_LOG_V1(MusicSubunit, "MusicSubunit: Parsing complete - %zu plugs, "
                "audio=%d midi=%d smpte=%d",
                plugs_.size(),
                capabilities_.hasAudioCapability,
                capabilities_.hasMidiCapability,
                capabilities_.hasSmpteTimeCodeCapability);

    completion(true);
}

bool MusicSubunit::HasCompleteDescriptorParse() const noexcept {
    if (!statusDescriptorReadOk_ || !statusDescriptorParsedOk_) {
        return false;
    }

    if (!statusDescriptorHasRouting_ || !statusDescriptorHasPlugs_) {
        return false;
    }

    if (statusDescriptorExpectedPlugCount_ > 0 &&
        plugs_.size() < statusDescriptorExpectedPlugCount_) {
        return false;
    }

    return true;
}

// Helper to extract name from a block (looks in nested blocks recursively)
static std::string ExtractPlugName(const ASFW::Protocols::AVC::Descriptors::AVCInfoBlock& block) {
    // Look for Name (0x000B) or RawText (0x000A) blocks
    auto nameBlock = block.FindNestedRecursive(0x000B); // Name
    if (!nameBlock) {
        nameBlock = block.FindNestedRecursive(0x000A); // Raw Text
    }
    
    if (nameBlock) {
        const auto& nameData = nameBlock->GetPrimaryData();
        if (!nameData.empty()) {
            std::string name;
            name.assign(reinterpret_cast<const char*>(nameData.data()), nameData.size());
            
            // Remove non-printables
            name.erase(std::remove_if(name.begin(), name.end(), [](unsigned char c){ 
                return !std::isprint(c); 
            }), name.end());
            
            return name;
        }
    }
    return "";
}

// Extract plugs from RoutingStatus block (0x8108) using position-based direction
// Apple's VirtualMusicSubunit.cpp shows: first numDestPlugs blocks are Input, next numSourcePlugs are Output
static void ExtractPlugsFromRoutingStatus(
    const ASFW::Protocols::AVC::Descriptors::AVCInfoBlock& routingBlock,
    std::vector<MusicSubunit::PlugInfo>& plugs)
{
    using namespace ASFW::Protocols::AVC::Descriptors;
    using namespace ASFW::Protocols::AVC::StreamFormats;
    
    const auto& primaryData = routingBlock.GetPrimaryData();
    
    // RoutingStatus primary fields: [0]=numDestPlugs, [1]=numSourcePlugs, [2-3]=musicPlugCount
    if (primaryData.size() < 2) {
        ASFW_LOG_V0(MusicSubunit, "RoutingStatus: Primary fields too short (%zu bytes)", primaryData.size());
        return;
    }
    
    uint8_t numDestPlugs = primaryData[0];
    uint8_t numSourcePlugs = primaryData[1];
    
    ASFW_LOG_V1(MusicSubunit, "RoutingStatus: numDestPlugs=%u, numSourcePlugs=%u", numDestPlugs, numSourcePlugs);
    
    // Find all SubunitPlugInfo (0x8109) blocks in order (recursive to handle nested structures)
    auto subunitPlugInfoBlocks = routingBlock.FindAllNestedRecursive(0x8109);
    
    ASFW_LOG_V3(MusicSubunit, "RoutingStatus: Found %zu SubunitPlugInfo blocks", subunitPlugInfoBlocks.size());
    
    // Process each SubunitPlugInfo block
    size_t blockIndex = 0;
    for (const auto& plugInfoBlock : subunitPlugInfoBlocks) {
        const auto& plugPrimaryData = plugInfoBlock.GetPrimaryData();
        
        // SubunitPlugInfo primary fields: [0]=subunit_plug_id, [1-2]=fdf_fmt, [3]=usage, [4-5]=numClusters, [6-7]=numChannels
        if (plugPrimaryData.size() < 4) {
            ASFW_LOG_V3(MusicSubunit, "SubunitPlugInfo: Primary fields too short (%zu bytes)", plugPrimaryData.size());
            blockIndex++;
            continue;
        }
        
        PlugInfo plug;
        plug.plugID = plugPrimaryData[0];  // Byte 0 is the actual subunit_plug_id
        
        // Map Usage (byte 3) to MusicPlugType
        uint8_t usage = plugPrimaryData[3];
        if (usage == 0x04 || usage == 0x05 || usage == 0x0B) {
            plug.type = MusicPlugType::kAudio;
        } else {
            plug.type = static_cast<MusicPlugType>(usage);
        }
        
        // Determine direction from position:
        // - Blocks 0 to (numDestPlugs-1) are Destination (Input)
        // - Blocks numDestPlugs to (numDestPlugs+numSourcePlugs-1) are Source (Output)
        plug.direction = PlugDirection::kInput;  // default: dest plugs and out-of-range
        if (blockIndex >= numDestPlugs) {
            if (blockIndex < static_cast<size_t>(numDestPlugs + numSourcePlugs)) {
                plug.direction = PlugDirection::kOutput;
            } else {
                ASFW_LOG_V1(MusicSubunit, "SubunitPlugInfo: Block %zu beyond expected count (dest=%u, src=%u)",
                            blockIndex, numDestPlugs, numSourcePlugs);
            }
        }
        
        // Extract name from nested blocks
        plug.name = ExtractPlugName(plugInfoBlock);
        
        if (!plug.name.empty()) {
            ASFW_LOG_V1(MusicSubunit, "MusicSubunit: Found Plug ID %u (%{public}s): '%{public}s'", 
                        plug.plugID, 
                        plug.direction == PlugDirection::kInput ? "Input" : "Output",
                        plug.name.c_str());
        } else {
            ASFW_LOG_V1(MusicSubunit, "MusicSubunit: Plug ID %u (%{public}s) has no name", 
                        plug.plugID,
                        plug.direction == PlugDirection::kInput ? "Input" : "Output");
        }
        // Extract ClusterInfo (0x810A) blocks and their signals
        // ClusterInfo contains: formatCode, portType, numSignals, then signal entries
        auto clusterBlocks = plugInfoBlock.FindAllNestedRecursive(0x810A);
        
        ASFW_LOG_V1(MusicSubunit, "Plug %u: Found %zu ClusterInfo blocks", plug.plugID, clusterBlocks.size());
        
        for (const auto& clusterBlock : clusterBlocks) {
            const auto& clusterData = clusterBlock.GetPrimaryData();
            
            // ClusterInfo primary fields: [0]=formatCode, [1]=portType, [2]=numSignals
            // Followed by 4 bytes per signal: musicPlugID(2), channel(1), location(1)
            if (clusterData.size() < 3) {
                ASFW_LOG_V1(MusicSubunit, "ClusterInfo: Too short (%zu bytes)", clusterData.size());
                continue;
            }
            
            ChannelFormatInfo channelFormat;
            channelFormat.formatCode = static_cast<StreamFormatCode>(clusterData[0]);
            uint8_t numSignals = clusterData[2];
            channelFormat.channelCount = numSignals;
            
            ASFW_LOG_V1(MusicSubunit, "ClusterInfo: formatCode=0x%02X, numSignals=%u, dataSize=%zu",
                        clusterData[0], numSignals, clusterData.size());
            
            // Parse signal entries (4 bytes each)
            for (uint8_t sig = 0; sig < numSignals; ++sig) {
                size_t sigOffset = 3 + sig * 4;
                if (sigOffset + 4 > clusterData.size()) break;
                
                ChannelFormatInfo::ChannelDetail detail;
                detail.musicPlugID = (static_cast<uint16_t>(clusterData[sigOffset]) << 8) | 
                                     clusterData[sigOffset + 1];
                detail.position = sig;  // Position within cluster
                // clusterData[sigOffset + 2] = channel index within stream
                // clusterData[sigOffset + 3] = location code (ignored for now)
                
                // Name will be populated later from MusicPlugChannel lookup
                channelFormat.channels.push_back(detail);
                
                ASFW_LOG_V1(MusicSubunit, "  Signal %u: musicPlugID=0x%04X, position=%u",
                            sig, detail.musicPlugID, detail.position);
            }
            
            ASFW_LOG_V1(MusicSubunit, "ClusterInfo: Added %zu channels to format", channelFormat.channels.size());
            
            // Add to currentFormat (will be populated later with rate info)
            if (!plug.currentFormat) {
                AudioStreamFormat fmt;
                fmt.formatHierarchy = FormatHierarchy::kAM824;
                fmt.subtype = AM824Subtype::kCompound;
                fmt.channelFormats.push_back(channelFormat);
                plug.currentFormat = fmt;
            } else {
                plug.currentFormat->channelFormats.push_back(channelFormat);
            }
        }
        
        plugs.push_back(plug);
        blockIndex++;
    }
    
    // Also extract MusicPlugInfo (0x810B) blocks for individual channel names
    // These provide more granular names like "Analog Out 1", "Analog In 2"
    // Accessible via ExtractMusicPlugChannels helper below
}

// Extract individual channel names from MusicPlugInfo (0x810B) blocks
// These blocks contain per-channel information with music_plug_id and name
static void ExtractMusicPlugChannels(
    const ASFW::Protocols::AVC::Descriptors::AVCInfoBlock& block,
    std::vector<MusicSubunit::MusicPlugChannel>& channels)
{
    using namespace ASFW::Protocols::AVC::Descriptors;
    
    // Look for MusicPlugInfo (0x810B) blocks recursively
    auto musicPlugBlocks = block.FindAllNestedRecursive(0x810B);
    
    for (const auto& musicPlugBlock : musicPlugBlocks) {
        const auto& primaryData = musicPlugBlock.GetPrimaryData();
        
        // MusicPlugInfo primary fields: Port Type + Music Plug ID (at least 3-4 bytes needed)
        // Based on Python parser:
        //   primary_len=14 with music_plug_id at bytes [1-2] and port_type at byte [0]
        if (primaryData.size() < 3) {
            continue;  // Too short to parse
        }
        
        MusicSubunit::MusicPlugChannel channel;
        channel.portType = primaryData[0];
        // Music Plug ID is at bytes 1-2 (big-endian)
        channel.musicPlugID = (static_cast<uint16_t>(primaryData[1]) << 8) | primaryData[2];
        
        // Extract name from nested RawText (0x000A) or Name (0x000B) block
        channel.name = ExtractPlugName(musicPlugBlock);
        
        if (!channel.name.empty()) {
            ASFW_LOG_V1(MusicSubunit, "MusicSubunit: Music Channel ID %u: '%{public}s' (plugType=0x%02x)",
                        channel.musicPlugID, channel.name.c_str(), channel.portType);
        }
        
        channels.push_back(channel);
    }
}

// Helper to extract plug info from a block (recursive) - used as fallback when no RoutingStatus found
static void ExtractPlugsFromBlock(
    const ASFW::Protocols::AVC::Descriptors::AVCInfoBlock& block,
    std::vector<MusicSubunit::PlugInfo>& plugs)
{
    using namespace ASFW::Protocols::AVC::Descriptors;
    using namespace ASFW::Protocols::AVC::StreamFormats;

    // Check for RoutingStatus (0x8108) - this contains the plug direction info
    if (block.GetType() == 0x8108) {
        ExtractPlugsFromRoutingStatus(block, plugs);
        return;  // Don't recurse further - RoutingStatus handles its children
    }
    
    // Recurse into children to find RoutingStatus blocks
    for (const auto& child : block.GetNestedBlocks()) {
        ExtractPlugsFromBlock(child, plugs);
    }
}


//==============================================================================
// Music Subunit Identifier Descriptor Parser
// Spec: TA Document 2001007, Section 5.2
//==============================================================================

size_t MusicSubunit::ParseMusicSubunitIdentifier(const uint8_t* data, size_t length) {
    ASFW_LOG_V3(MusicSubunit, "Parsing Music Subunit Identifier Descriptor (%zu bytes)", length);

    // Declare variables at top to avoid goto bypassing initialization errors
    size_t infoBlockOffset = 0;

    // Minimum required: descriptor header + some basic fields
    if (length < 16) {
        ASFW_LOG_V0(MusicSubunit, "Descriptor too short (%zu bytes) for header", length);
        return 0;  // Error - return 0
    }

    // Parse descriptor header
    // uint16_t descriptorLength = ReadBE16(data);  // Usually matches 'length' parameter
    uint8_t generationID = data[2];
    size_t sizeOfListID = data[3];
    size_t sizeOfObjectID = data[4];  // Note: FWA shows this is 1 byte, not 2!
    size_t sizeOfEntryPos = data[5];
    uint16_t numRootLists = ReadBE16(data + 6);
    
    ASFW_LOG_V3(MusicSubunit, "Header: GenID=0x%02x, ListIDSize=%zu, ObjIDSize=%zu, EntryPosSize=%zu, NumRootLists=%u",
                  generationID, sizeOfListID, sizeOfObjectID, sizeOfEntryPos, numRootLists);
    
    // Validate generation ID
    // 0x00: Music Subunit 1.0 (Standard)
    // 0x02: Observed in some devices
    if (generationID != 0x00 && generationID != 0x02) {
        ASFW_LOG_V1(MusicSubunit, "Unexpected generation_ID=0x%02x (expected 0x00 or 0x02)", generationID);
    }
    
    // Calculate offset to subunit_type_dependent_information_length
    size_t rootListArraySize = numRootLists * sizeOfListID;
    size_t subunitDepInfoLenOffset = 8 + rootListArraySize;
    
    if (length < subunitDepInfoLenOffset + 2) {
        ASFW_LOG_V0(MusicSubunit, "Descriptor too short for subunit_type_dependent_information_length at offset %zu (0x%zx)", subunitDepInfoLenOffset, subunitDepInfoLenOffset);
        return 0;  // Error - return 0
    }
    
    uint16_t subunitDepInfoLen = ReadBE16(data + subunitDepInfoLenOffset);
    size_t subunitDepInfoOffset = subunitDepInfoLenOffset + 2;
    
    ASFW_LOG_V3(MusicSubunit, "Subunit dependent info: length=%u, offset=%zu", subunitDepInfoLen, subunitDepInfoOffset);
    
    if (length < subunitDepInfoOffset + subunitDepInfoLen) {
        ASFW_LOG_V0(MusicSubunit, "Descriptor too short for claimed dependent info (len=%u) at offset %zu", subunitDepInfoLen, subunitDepInfoOffset);
        return 0;  // Error - return 0
    }
    
    // Parse Music Subunit specific header within subunit_type_dependent_information
    const uint8_t* musicInfoPtr = data + subunitDepInfoOffset;
    size_t musicInfoAvailableLen = subunitDepInfoLen;
    
    if (musicInfoAvailableLen < 6) {
        ASFW_LOG_V0(MusicSubunit, "Music subunit dependent info too short (%zu bytes)", musicInfoAvailableLen);
        return 0;  // Error - return 0
    }
    
    // Music subunit header: [0-1]=length, [2]=genID, [3]=version, [4-5]=specific_info_length
    capabilities_.musicSubunitVersion = musicInfoPtr[3];
    uint16_t musicSpecificInfoLen = ReadBE16(musicInfoPtr + 4);
    size_t musicSpecificInfoOffset = 6;
    
    ASFW_LOG_V1(MusicSubunit, "Music Subunit Version: 0x%02x, Specific Info Length: %u",
                 capabilities_.musicSubunitVersion, musicSpecificInfoLen);
    
    if (musicInfoAvailableLen < musicSpecificInfoOffset + musicSpecificInfoLen) {
        ASFW_LOG_V0(MusicSubunit, "Music info too short for claimed specific_information length (%u)", musicSpecificInfoLen);
        return 0;  // Error - return 0
    }
    
    // Parse music_subunit_specific_information (capabilities)
    const uint8_t* specificPtr = musicInfoPtr + musicSpecificInfoOffset;
    size_t specificAvailableLen = musicSpecificInfoLen;
    size_t currentOffset = 0;
    
    if (specificAvailableLen < 1) {
        ASFW_LOG_V1(MusicSubunit, "Music specific info area is empty");
        return 0;  // Error - return 0
    }
    
    // Parse capability presence flags (CORRECTED: LSB-first, not MSB-first!)
    uint8_t capAttribs = specificPtr[currentOffset++];
    capabilities_.hasGeneralCapability        = (capAttribs & 0x01) != 0;  // Bit 0
    capabilities_.hasAudioCapability          = (capAttribs & 0x02) != 0;  // Bit 1
    capabilities_.hasMidiCapability           = (capAttribs & 0x04) != 0;  // Bit 2
    capabilities_.hasSmpteTimeCodeCapability  = (capAttribs & 0x08) != 0;  // Bit 3
    capabilities_.hasSampleCountCapability    = (capAttribs & 0x10) != 0;  // Bit 4
    capabilities_.hasAudioSyncCapability      = (capAttribs & 0x20) != 0;  // Bit 5
    
    ASFW_LOG_V3(MusicSubunit, "Capability Flags: 0x%02x [Gen=%d, Aud=%d, MIDI=%d, SMPTE=%d, Samp=%d, Sync=%d]",
                  capAttribs, capabilities_.hasGeneralCapability, capabilities_.hasAudioCapability,
                  capabilities_.hasMidiCapability, capabilities_.hasSmpteTimeCodeCapability,
                  capabilities_.hasSampleCountCapability, capabilities_.hasAudioSyncCapability);
    
    //==========================================================================
    // Parse capability blocks (each is length-prefixed: [lenByte][data...])
    //==========================================================================
    
    // General Capability
    if (capabilities_.hasGeneralCapability) {
        if (specificAvailableLen < currentOffset + 1) goto parse_error;
        uint8_t genCapLen = specificPtr[currentOffset];
        size_t genCapBlockSize = genCapLen + 1;  // +1 for length byte itself
        
        if (specificAvailableLen < currentOffset + genCapBlockSize || genCapLen < 6) {
            ASFW_LOG_V0(MusicSubunit, "General Capability block invalid (len=%u)", genCapLen);
            goto parse_error;
        }
        
        const uint8_t* genCapPtr = specificPtr + currentOffset + 1;
        capabilities_.transmitCapabilityFlags = genCapPtr[0];
        capabilities_.receiveCapabilityFlags = genCapPtr[1];
        capabilities_.latencyCapability = ReadBE32(genCapPtr + 2);
        
        ASFW_LOG_V1(MusicSubunit, "General Capability: TxFlags=0x%02x, RxFlags=0x%02x, Latency=%u",
                     capabilities_.transmitCapabilityFlags.value(),
                     capabilities_.receiveCapabilityFlags.value(),
                     capabilities_.latencyCapability.value());
        
        currentOffset += genCapBlockSize;
    }
    
    // Audio Capability
    if (capabilities_.hasAudioCapability) {
        if (specificAvailableLen < currentOffset + 1) goto parse_error;
        uint8_t audioCapLen = specificPtr[currentOffset];
        size_t audioCapBlockSize = audioCapLen + 1;
        
        if (specificAvailableLen < currentOffset + audioCapBlockSize || audioCapLen < 5) {
            ASFW_LOG_V0(MusicSubunit, "Audio Capability block invalid (len=%u)", audioCapLen);
            goto parse_error;
        }
        
        const uint8_t* audioCapPtr = specificPtr + currentOffset + 1;
        uint8_t numFormats = audioCapPtr[0];
        size_t minRequired = 1 + 4 + (numFormats * 6);  // NumFormats + MaxIn/Out + (Formats × 6)
        
        if (audioCapLen < minRequired) {
            ASFW_LOG_V0(MusicSubunit, "Audio Capability data too short for %u formats", numFormats);
            goto parse_error;
        }
        
        capabilities_.maxAudioInputChannels = ReadBE16(audioCapPtr + 1);
        capabilities_.maxAudioOutputChannels = ReadBE16(audioCapPtr + 3);
        
        // Parse available formats array (6 bytes each)
        std::vector<AudioSampleFormat> formats;
        size_t formatOffset = 5;
        for (uint8_t i = 0; i < numFormats; ++i) {
            if (audioCapLen < formatOffset + 6) {
                ASFW_LOG_V0(MusicSubunit, "Audio format list truncated at index %u", i);
                goto parse_error;
            }
            AudioSampleFormat fmt;
            fmt.raw[0] = audioCapPtr[formatOffset];
            fmt.raw[1] = audioCapPtr[formatOffset + 1];
            fmt.raw[2] = audioCapPtr[formatOffset + 2];
            formats.push_back(fmt);
            formatOffset += 6;
        }
        capabilities_.availableAudioFormats = std::move(formats);
        
        ASFW_LOG_V1(MusicSubunit, "Audio Capability: MaxIn=%u, MaxOut=%u, NumFormats=%u",
                     capabilities_.maxAudioInputChannels.value(),
                     capabilities_.maxAudioOutputChannels.value(),
                     numFormats);
        
        currentOffset += audioCapBlockSize;
    }
    
    // MIDI Capability
    if (capabilities_.hasMidiCapability) {
        if (specificAvailableLen < currentOffset + 1) goto parse_error;
        uint8_t midiCapLen = specificPtr[currentOffset];
        size_t midiCapBlockSize = midiCapLen + 1;
        
        if (specificAvailableLen < currentOffset + midiCapBlockSize || midiCapLen < 6) {
            ASFW_LOG_V0(MusicSubunit, "MIDI Capability block invalid (len=%u)", midiCapLen);
            goto parse_error;
        }
        
        const uint8_t* midiCapPtr = specificPtr + currentOffset + 1;
        capabilities_.midiVersionMajor = midiCapPtr[0] >> 4;       // High nibble
        capabilities_.midiVersionMinor = midiCapPtr[0] & 0x0F;     // Low nibble
        capabilities_.midiAdaptationLayerVersion = midiCapPtr[1];
        capabilities_.maxMidiInputPorts = ReadBE16(midiCapPtr + 2);
        capabilities_.maxMidiOutputPorts = ReadBE16(midiCapPtr + 4);
        
        ASFW_LOG_V1(MusicSubunit, "MIDI Capability: Ver=%u.%u, Adapt=0x%02x, MaxIn=%u, MaxOut=%u",
                     capabilities_.midiVersionMajor.value(),
                     capabilities_.midiVersionMinor.value(),
                     capabilities_.midiAdaptationLayerVersion.value(),
                     capabilities_.maxMidiInputPorts.value(),
                     capabilities_.maxMidiOutputPorts.value());
        
        currentOffset += midiCapBlockSize;
    }
    
    // SMPTE Time Code Capability
    if (capabilities_.hasSmpteTimeCodeCapability) {
        if (specificAvailableLen < currentOffset + 1) goto parse_error;
        uint8_t smpteCapLen = specificPtr[currentOffset];
        size_t smpteCapBlockSize = smpteCapLen + 1;
        
        if (specificAvailableLen < currentOffset + smpteCapBlockSize || smpteCapLen < 1) {
            ASFW_LOG_V0(MusicSubunit, "SMPTE Capability block invalid (len=%u)", smpteCapLen);
            goto parse_error;
        }
        
        capabilities_.smpteTimeCodeCapabilityFlags = specificPtr[currentOffset + 1];
        ASFW_LOG_V1(MusicSubunit, "SMPTE Capability: Flags=0x%02x",
                     capabilities_.smpteTimeCodeCapabilityFlags.value());
        
        currentOffset += smpteCapBlockSize;
    }
    
    // Sample Count Capability
    if (capabilities_.hasSampleCountCapability) {
        if (specificAvailableLen < currentOffset + 1) goto parse_error;
        uint8_t sampleCapLen = specificPtr[currentOffset];
        size_t sampleCapBlockSize = sampleCapLen + 1;
        
        if (specificAvailableLen < currentOffset + sampleCapBlockSize || sampleCapLen < 1) {
            ASFW_LOG_V0(MusicSubunit, "Sample Count Capability block invalid (len=%u)", sampleCapLen);
            goto parse_error;
        }
        
        capabilities_.sampleCountCapabilityFlags = specificPtr[currentOffset + 1];
        ASFW_LOG_V1(MusicSubunit, "Sample Count Capability: Flags=0x%02x",
                     capabilities_.sampleCountCapabilityFlags.value());
        
        currentOffset += sampleCapBlockSize;
    }
    
    // Audio SYNC Capability
    if (capabilities_.hasAudioSyncCapability) {
        if (specificAvailableLen < currentOffset + 1) goto parse_error;
        uint8_t syncCapLen = specificPtr[currentOffset];
        size_t syncCapBlockSize = syncCapLen + 1;
        
        if (specificAvailableLen < currentOffset + syncCapBlockSize || syncCapLen < 1) {
            ASFW_LOG_V0(MusicSubunit, "Audio SYNC Capability block invalid (len=%u)", syncCapLen);
            goto parse_error;
        }
        
        capabilities_.audioSyncCapabilityFlags = specificPtr[currentOffset + 1];
        ASFW_LOG_V1(MusicSubunit, "Audio SYNC Capability: Flags=0x%02x",
                     capabilities_.audioSyncCapabilityFlags.value());

        currentOffset += syncCapBlockSize;
    }

    // Calculate absolute offset where info blocks start
    // Formula: subunitDepInfoOffset + musicSpecificInfoOffset + currentOffset
    infoBlockOffset = subunitDepInfoOffset + musicSpecificInfoOffset + currentOffset;

    ASFW_LOG_V3(MusicSubunit, "Successfully parsed Music Subunit Identifier Descriptor, info blocks start at offset %zu", infoBlockOffset);
    return infoBlockOffset;

parse_error:
    ASFW_LOG_V0(MusicSubunit, "Parse error at offset %zu in music_subunit_specific_information", currentOffset);
    return 0;  // Error - return 0
}

void MusicSubunit::ParseDescriptorBlock(const uint8_t* data, size_t length) {
    statusDescriptorParsedOk_ = false;
    statusDescriptorHasRouting_ = false;
    statusDescriptorHasClusterInfo_ = false;
    statusDescriptorHasPlugs_ = false;
    statusDescriptorExpectedPlugCount_ = 0;
    musicChannels_.clear();
    plugs_.clear();

    if (length < 4) {
        ASFW_LOG_V0(MusicSubunit, "Descriptor too short (%zu bytes)", length);
        return;
    }

    // We are reading the Status Descriptor (0x80), which consists of a 2-byte length
    // followed immediately by Info Blocks.
    // Reference: TA Document 2001007, Figure 6.1
    
    uint16_t descriptorLength = ReadBE16(data);
    ASFW_LOG_V1(MusicSubunit, "Parsing Status Descriptor: Declared Length=%u, Actual=%zu", 
                descriptorLength, length);
    // Per spec, info blocks immediately follow the 2-byte length.
    // Clamp parsing to the advertised descriptor length to avoid reading
    // appended data from buggy captures.
    const size_t advertisedEnd = 2 + static_cast<size_t>(descriptorLength);
    const size_t parseEnd = std::min(length, advertisedEnd);
    size_t infoBlockOffset = 2; // Standard offset

    // Context for parsing plugs and routing
    struct ParsingContext {
        std::vector<PlugInfo> discoveredPlugs;
        int numDest = 0;
        int numSrc = 0;
        bool foundRouting = false;
    };
    
    ParsingContext ctx;
    size_t parsedBlockCount = 0;

    // Helper to process a block
    std::function<void(const ASFW::Protocols::AVC::Descriptors::AVCInfoBlock&)> processBlock;
    processBlock = [&](const ASFW::Protocols::AVC::Descriptors::AVCInfoBlock& block) {
        using namespace ASFW::Protocols::AVC::Descriptors;
        using namespace ASFW::Protocols::AVC::StreamFormats; // Fix namespace lookup
        
        // Capability Blocks (0x8100 - 0x8105)
        uint16_t type = block.GetType();
        const auto& pData = block.GetPrimaryData();

        if (type == 0x8100) { // General Music Subunit Status Area
            if (pData.size() >= 6) {
                capabilities_.hasGeneralCapability = true;
                capabilities_.transmitCapabilityFlags = pData[0];
                capabilities_.receiveCapabilityFlags = pData[1];
                capabilities_.latencyCapability = ReadBE32(pData.data() + 2);
                ASFW_LOG_V1(MusicSubunit, "GMSSA: Tx=0x%02x Rx=0x%02x Latency=%u",
                    pData[0], pData[1], capabilities_.latencyCapability.value());
            }
        }
        else if (type == 0x8101) { // Audio Subunit Status Area
            if (pData.size() >= 5) {
                capabilities_.hasAudioCapability = true;
                uint8_t numFormats = pData[0];
                capabilities_.maxAudioInputChannels = ReadBE16(pData.data() + 1);
                capabilities_.maxAudioOutputChannels = ReadBE16(pData.data() + 3);
                
                ASFW_LOG_V1(MusicSubunit, "Audio Caps: In=%u Out=%u Formats=%u",
                    capabilities_.maxAudioInputChannels.value(),
                    capabilities_.maxAudioOutputChannels.value(), numFormats);
            }
        }
        else if (type == 0x8102) { // MIDI Subunit Status Area
            if (pData.size() >= 6) {
                capabilities_.hasMidiCapability = true;
                capabilities_.midiVersionMajor = pData[0] >> 4;
                capabilities_.midiVersionMinor = pData[0] & 0x0F;
                capabilities_.midiAdaptationLayerVersion = pData[1];
                capabilities_.maxMidiInputPorts = ReadBE16(pData.data() + 2);
                capabilities_.maxMidiOutputPorts = ReadBE16(pData.data() + 4);
                ASFW_LOG_V1(MusicSubunit, "MIDI Caps: Ports In=%u Out=%u",
                     capabilities_.maxMidiInputPorts.value(), capabilities_.maxMidiOutputPorts.value());
            }
        }
        else if (type == 0x8103) { // SMPTE Status Area
             if (!pData.empty()) {
                 capabilities_.hasSmpteTimeCodeCapability = true;
                 capabilities_.smpteTimeCodeCapabilityFlags = pData[0];
             }
        }
        else if (type == 0x8104) { // Sample Count Status Area
             if (!pData.empty()) {
                 capabilities_.hasSampleCountCapability = true;
                 capabilities_.sampleCountCapabilityFlags = pData[0];
             }
        }
        else if (type == 0x8105) { // Audio Sync Status Area
             if (!pData.empty()) {
                 capabilities_.hasAudioSyncCapability = true;
                 capabilities_.audioSyncCapabilityFlags = pData[0];
                 ASFW_LOG_V1(MusicSubunit, "Audio Sync Caps: Flags=0x%02x", pData[0]);
             }
        }

        // 1. RoutingStatus (0x8108)
        if (block.GetType() == 0x8108) {
            const auto& pData = block.GetPrimaryData();
            if (pData.size() >= 2) {
                ctx.numDest = pData[0];
                ctx.numSrc = pData[1];
                ctx.foundRouting = true;
                statusDescriptorHasRouting_ = true;
                statusDescriptorExpectedPlugCount_ = static_cast<uint16_t>(ctx.numDest + ctx.numSrc);
                ASFW_LOG_V1(MusicSubunit, "RoutingStatus found: dest=%d src=%d", ctx.numDest, ctx.numSrc);
            }
            // Recurse to find nested 0x8109 (standard behavior)
            for (const auto& child : block.GetNestedBlocks()) {
                processBlock(child);
            }
            return;
        }
        
        // 2. SubunitPlugInfo (0x8109)
        if (block.GetType() == 0x8109) {
            const auto& pData = block.GetPrimaryData();
            if (pData.size() >= 4) {
                PlugInfo plug;
                plug.plugID = pData[0];
                uint8_t usage = pData[3];
                if (usage == 0x04 || usage == 0x05) plug.type = MusicPlugType::kAudio;
                else plug.type = static_cast<MusicPlugType>(usage);
                
                plug.name = ExtractPlugName(block);
                
                // Extract ClusterInfo (0x810A) blocks to populate channel info
                auto clusterBlocks = block.FindAllNestedRecursive(0x810A);
                ASFW_LOG_V1(MusicSubunit, "Plug %u: Found %zu ClusterInfo blocks", plug.plugID, clusterBlocks.size());
                
                for (const auto& clusterBlock : clusterBlocks) {
                    const auto& clusterData = clusterBlock.GetPrimaryData();
                    // ClusterInfo: [0]=formatCode, [1]=portType, [2]=numSignals
                    // Then 4 bytes per signal: musicPlugID(2), channel(1), location(1)
                    if (clusterData.size() >= 3) {
                        ChannelFormatInfo channelFormat;
                        channelFormat.formatCode = static_cast<StreamFormatCode>(clusterData[0]);
                        uint8_t numSignals = clusterData[2];
                        channelFormat.channelCount = numSignals;
                        
                        ASFW_LOG_V1(MusicSubunit, "ClusterInfo: formatCode=0x%02X, numSignals=%u",
                                    clusterData[0], numSignals);
                        
                        // Parse signal entries (4 bytes each after the 3-byte header)
                        for (uint8_t s = 0; s < numSignals && (3 + (s + 1) * 4) <= clusterData.size(); s++) {
                            size_t signalOffset = 3 + s * 4;
                            uint16_t musicPlugID = (static_cast<uint16_t>(clusterData[signalOffset]) << 8) 
                                                 | clusterData[signalOffset + 1];
                            uint8_t position = clusterData[signalOffset + 2];
                            
                            ChannelFormatInfo::ChannelDetail detail;
                            detail.musicPlugID = musicPlugID;
                            detail.position = position;
                            // Name will be populated later from MusicPlugInfo
                            channelFormat.channels.push_back(detail);
                            
                            ASFW_LOG_V1(MusicSubunit, "  Signal %u: musicPlugID=0x%04X, position=%u",
                                        s, musicPlugID, position);
                        }
                        
                        if (!channelFormat.channels.empty()) {
                            statusDescriptorHasClusterInfo_ = true;
                            // Initialize currentFormat if not already set
                            if (!plug.currentFormat.has_value()) {
                                plug.currentFormat = AudioStreamFormat{};
                            }
                            plug.currentFormat->channelFormats.push_back(channelFormat);
                        }
                    }
                }
                
                ctx.discoveredPlugs.push_back(plug);
                statusDescriptorHasPlugs_ = true;
            }
            // 8109 shouldn't have nested plugs usually, but we check children anyway? No.
            return;
        }
        
        // Recurse for container blocks (e.g. Root Lists, Compound blocks)
        for (const auto& child : block.GetNestedBlocks()) {
            processBlock(child);
        }
    };

    if (infoBlockOffset < parseEnd) {
        ASFW_LOG_V3(MusicSubunit, "Parsing info blocks at offset %zu (length=%zu)",
                      infoBlockOffset, parseEnd - infoBlockOffset);
        
        // Robust parsing using AVCInfoBlock class
        using namespace ASFW::Protocols::AVC::Descriptors;
        
        size_t offset = infoBlockOffset;
        while (offset < parseEnd) {
            // FWA Fallback Scan: Check for valid block header
            if (parseEnd - offset < 4) {
                 ASFW_LOG_V1(MusicSubunit, "End of descriptor cleanup: %zu bytes remaining (too small for header)", parseEnd - offset);
                 break;
            }

            // Peek at compound length to validate block size before parsing
            uint16_t compoundLength = (static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1];
            size_t blockSize = compoundLength + 2;

            if (blockSize < 4 || compoundLength == 0xFFFF) {
                ASFW_LOG_V1(MusicSubunit, "Garbage/Invalid block at offset %zu (size=%zu). Scanning... (skipping 4 bytes)", offset, blockSize);
                offset += 4;
                continue;
            }

            size_t consumed = 0;
            size_t remaining = parseEnd - offset;
            
            auto blockResult = AVCInfoBlock::Parse(data + offset, remaining, consumed);
            
            if (blockResult) {
                parsedBlockCount++;
                // Process this top-level block
                processBlock(*blockResult);
                
                // Also extract individual channel names from MusicPlugInfo (0x810B) blocks
                ExtractMusicPlugChannels(*blockResult, musicChannels_);
                
                offset += consumed;
            } else {
                ASFW_LOG_V1(MusicSubunit, "Failed to parse info block at offset %zu, attempting scan (skipping 4 bytes)", offset);
                offset += 4; // Try skipping ahead instead of hard break
            }
            
            if (consumed == 0 && !blockResult) {
                 // If Parse returned null and consumed 0, handled above by skipping. 
                 // If Parse succeeded but consumed 0 (shouldn't happen), break loop to avoid infinite.
                 // Double check logic above: if blockResult is valid, consumed is used.
            }
        }
        
    } else {
        ASFW_LOG_V1(MusicSubunit, "No info blocks present");
    }

    // Post-process plugs to assign direction
    if (!ctx.discoveredPlugs.empty()) {
        if (!ctx.foundRouting) {
            // No routing info? checking device behavior
             ASFW_LOG_V1(MusicSubunit, "Warning: Plugs found but no RoutingStatus. Defaulting to Input.");
        }
        
        // Assign directions based on index and counts
        // Standard: First numDest are Inputs, then numSrc are Outputs
        
        // Note: Discovered plugs might include duplicates if multiple blocks describe same plug?
        // Assuming strictly ordered appearance in descriptor matches routing order.
        
        using namespace ASFW::Protocols::AVC::StreamFormats;

        size_t index = 0;
        for (auto& plug : ctx.discoveredPlugs) {
             if (ctx.foundRouting) {
                 if (index < static_cast<size_t>(ctx.numDest)) {
                     plug.direction = PlugDirection::kInput;
                 } else if (index < static_cast<size_t>(ctx.numDest + ctx.numSrc)) {
                     plug.direction = PlugDirection::kOutput;
                 } else {
                     plug.direction = PlugDirection::kInput; // Fallback
                     ASFW_LOG_V1(MusicSubunit, "Plug index %zu beyond declared counts (dest=%d src=%d)", index, ctx.numDest, ctx.numSrc);
                 }
             } else {
                 plug.direction = PlugDirection::kInput;
             }
             
             if (!plug.name.empty()) {
                 ASFW_LOG_V1(MusicSubunit, "Parsed Plug %u (%{public}s): %{public}s", 
                     plug.plugID, plug.direction == PlugDirection::kInput ? "In" : "Out", plug.name.c_str());
             }
             index++;
        }
        
        plugs_ = std::move(ctx.discoveredPlugs);
        statusDescriptorHasPlugs_ = !plugs_.empty();
        
        // Associate channel names from musicChannels_ (MusicPlugInfo blocks)
        // Build a musicPlugID → name lookup map
        std::unordered_map<uint16_t, std::string> channelNameMap;
        for (const auto& ch : musicChannels_) {
            if (!ch.name.empty()) {
                channelNameMap[ch.musicPlugID] = ch.name;
            }
        }
        
        // Populate names in ChannelDetail entries
        for (auto& plug : plugs_) {
            if (plug.currentFormat) {
                for (auto& cf : plug.currentFormat->channelFormats) {
                    for (auto& detail : cf.channels) {
                        auto it = channelNameMap.find(detail.musicPlugID);
                        if (it != channelNameMap.end()) {
                            detail.name = it->second;
                            ASFW_LOG_V1(MusicSubunit, "Plug %u: Channel 0x%04X -> '%{public}s'",
                                        plug.plugID, detail.musicPlugID, detail.name.c_str());
                        }
                    }
                }
            }
        }
    }

    statusDescriptorParsedOk_ = (parsedBlockCount > 0);

    if (!plugs_.empty()) {
        // Note: hasAudioCapability might already be set from descriptor parsing above
        // If plugs are found, ensure the corresponding capability flags are set.
        // This handles cases where the Music Subunit Identifier Descriptor might not
        // explicitly list these capabilities, but plugs are present.
        for (const auto& plug : plugs_) {
            if (plug.type == ASFW::Protocols::AVC::StreamFormats::MusicPlugType::kAudio) {
                capabilities_.hasAudioCapability = true;
            } else if (plug.type == ASFW::Protocols::AVC::StreamFormats::MusicPlugType::kMIDI) {
                capabilities_.hasMidiCapability = true;
            }
            // Add other plug types if they imply capabilities
        }

        // Update capability counts based on discovered plugs
        uint16_t audioInputPlugs = 0;
        uint16_t audioOutputPlugs = 0;
        uint16_t audioInputMaxChannels = capabilities_.maxAudioInputChannels.value_or(0);
        uint16_t audioOutputMaxChannels = capabilities_.maxAudioOutputChannels.value_or(0);
        uint16_t midiIns = 0;
        uint16_t midiOuts = 0;

        for (const auto& plug : plugs_) {
            if (plug.type == ASFW::Protocols::AVC::StreamFormats::MusicPlugType::kAudio) {
                uint16_t channels = 0;
                if (plug.currentFormat.has_value()) {
                    const auto& fmt = *plug.currentFormat;
                    if (fmt.totalChannels > 0) {
                        channels = fmt.totalChannels;
                    } else {
                        uint32_t sum = 0;
                        for (const auto& block : fmt.channelFormats) {
                            sum += block.channelCount;
                        }
                        if (sum > 0) {
                            channels = static_cast<uint16_t>(std::min<uint32_t>(sum, 0xFFFFu));
                        }
                    }
                }

                if (plug.IsInput()) {
                    ++audioInputPlugs;
                    audioInputMaxChannels = std::max(audioInputMaxChannels, channels);
                } else {
                    ++audioOutputPlugs;
                    audioOutputMaxChannels = std::max(audioOutputMaxChannels, channels);
                }
            } else if (plug.type == ASFW::Protocols::AVC::StreamFormats::MusicPlugType::kMIDI) {
                if (plug.IsInput()) midiIns++;
                else midiOuts++;
            }
        }

        if (audioInputMaxChannels > 0) {
            capabilities_.maxAudioInputChannels = audioInputMaxChannels;
        }
        if (audioOutputMaxChannels > 0) {
            capabilities_.maxAudioOutputChannels = audioOutputMaxChannels;
        }
        capabilities_.maxMidiInputPorts = midiIns;
        capabilities_.maxMidiOutputPorts = midiOuts;
        
        ASFW_LOG_V1(MusicSubunit,
                    "Updated Capabilities from Plugs: Audio In maxCh=%u (plugs=%u) Out maxCh=%u (plugs=%u), MIDI In=%u Out=%u",
                    capabilities_.maxAudioInputChannels.value_or(0), audioInputPlugs,
                    capabilities_.maxAudioOutputChannels.value_or(0), audioOutputPlugs,
                    midiIns, midiOuts);
    }
}

// ... (ReadStatusDescriptor) ...
void MusicSubunit::ReadStatusDescriptor(AVCUnit& unit, std::function<void(bool)> completion) {
    ASFW_LOG_V1(MusicSubunit, "Reading Music Subunit Status Descriptor (type 0x80)");

    // Keep AVCUnit alive during async operations
    auto unitPtr = unit.shared_from_this();

    auto accessor = std::make_shared<DescriptorAccessor>(
        unit.GetFCPTransport(), GetAddress()
    );

    // Define specifier for Status Descriptor (0x80)
    DescriptorSpecifier specifier;
    specifier.type = static_cast<DescriptorSpecifierType>(0x80);
    specifier.typeSpecificFields = {};

    // Common parsing logic
    auto parseHandler = [this, completion](const DescriptorAccessor::ReadDescriptorResult& result) {
        if (!result.success) {
            ASFW_LOG_V0(MusicSubunit, "Failed to read Status Descriptor: %d",
                          static_cast<int>(result.avcResult));
            completion(false);
            return;
        }

        const auto& data = result.data;
        ASFW_LOG_V3(MusicSubunit, "Received Status Descriptor (%zu bytes)", data.size());

        // Store raw data
        statusDescriptorData_ = data;

        // Parse total_info_block_length from header (2 bytes)
        if (data.size() < 2) {
            ASFW_LOG_V0(MusicSubunit, "Status Descriptor too short (need >=2 bytes for header)");
            completion(false);
            return;
        }

        uint16_t totalInfoBlockLength = ReadBE16(data.data());
        ASFW_LOG_V3(MusicSubunit, "Total info block length: %u bytes", totalInfoBlockLength);

        // Validate length
        if (data.size() < 2 + totalInfoBlockLength) {
            ASFW_LOG_V1(MusicSubunit,
                "Status Descriptor shorter than claimed (have %zu, need %u)",
                data.size(), 2 + totalInfoBlockLength);
        }

        // Parse info blocks using AVCInfoBlock::Parse
        dynamicStatus_.clear();
        const size_t advertisedEnd = 2 + static_cast<size_t>(totalInfoBlockLength);
        const size_t parseEnd = std::min(data.size(), advertisedEnd);
        size_t offset = 2;  // Skip total_info_block_length field

        while (offset < parseEnd) {
            size_t consumed = 0;
            auto block = ASFW::Protocols::AVC::Descriptors::AVCInfoBlock::Parse(
                data.data() + offset,
                parseEnd - offset,
                consumed
            );

            if (!block) {
                ASFW_LOG_V1(MusicSubunit,
                    "Failed to parse info block at offset %zu (error: %d), stopping",
                    offset, static_cast<int>(block.error()));
                break;
            }

            ASFW_LOG_V1(MusicSubunit, "Parsed status info block: type=0x%04x, %zu nested blocks",
                         block->GetType(), block->GetNestedBlocks().size());

            dynamicStatus_.push_back(std::move(*block));
            offset += consumed;
        }

            ASFW_LOG_V1(MusicSubunit, "Successfully parsed %zu status info blocks",
                         dynamicStatus_.size());

            completion(true);
    };

    // 1. Try Standard Sequence
    accessor->readWithOpenCloseSequence(specifier, [this, unitPtr, accessor, specifier, completion, parseHandler](const DescriptorAccessor::ReadDescriptorResult& result) {
        if (result.success) {
            parseHandler(result);
        } else {
            // 2. Fallback: Non-Standard Direct Read
            ASFW_LOG_V1(MusicSubunit, "MusicSubunit: Standard Status Read failed. Trying Non-Standard Direct Read...");
            accessor->readComplete(specifier, [parseHandler](const DescriptorAccessor::ReadDescriptorResult& fallbackResult) {
                parseHandler(fallbackResult);
            });
        }
    });
}

void MusicSubunit::SetSampleRate(ASFW::Protocols::AVC::IAVCCommandSubmitter& submitter, uint32_t sampleRate, std::function<void(bool)> completion) {
    using namespace StreamFormats;

    // Convert Hz to AM824 rate code
    SampleRate rateCode = SampleRate::k48000Hz;
    if (sampleRate == 44100) rateCode = SampleRate::k44100Hz;
    else if (sampleRate == 48000) rateCode = SampleRate::k48000Hz;
    else if (sampleRate == 88200) rateCode = SampleRate::k88200Hz;
    else if (sampleRate == 96000) rateCode = SampleRate::k96000Hz;
    else if (sampleRate == 176400) rateCode = SampleRate::k176400Hz;
    else if (sampleRate == 192000) rateCode = SampleRate::k192000Hz;
    else {
        ASFW_LOG_V1(MusicSubunit, "MusicSubunit: Unsupported sample rate %u Hz", sampleRate);
        completion(false);
        return;
    }

    // Create format structure
    AudioStreamFormat format;
    format.formatHierarchy = FormatHierarchy::kAM824; // AM824
    format.subtype = AM824Subtype::kCompound; // Compound
    format.sampleRate = rateCode;
    format.channelFormats.resize(0); // Don't care about channels for rate set? Or maybe we do?

    // Iterate all plugs and set format?
    // Or just the first one?
    // Usually setting one plug sets the device rate.
    // Let's try setting plug 0 (or the first available plug).
    
    if (plugs_.empty()) {
        ASFW_LOG_V1(MusicSubunit, "MusicSubunit: No plugs to set sample rate on");
        completion(false);
        return;
    }

    // Use the first plug
    uint8_t plugID = plugs_[0].plugID;
    bool isInput = plugs_[0].IsInput();

    ASFW_LOG_V1(MusicSubunit, "MusicSubunit: Setting sample rate to %u Hz (code 0x%02x) on plug %u", 
                 sampleRate, static_cast<uint8_t>(rateCode), plugID);

    auto cmd = std::make_shared<AVCStreamFormatCommand>(
        submitter,
        GetAddress(),
        plugID,
        isInput,
        format
    );

    cmd->Submit([completion](AVCResult result, const std::optional<AudioStreamFormat>& format) {
        if (IsSuccess(result)) {
            ASFW_LOG_V1(MusicSubunit, "MusicSubunit: SetSampleRate succeeded");
            completion(true);
        } else {
            ASFW_LOG_V1(MusicSubunit, "MusicSubunit: SetSampleRate failed (result=%d)", static_cast<int>(result));
            completion(false);
        }
    });
}

void MusicSubunit::LogConnection(size_t index, const StreamFormats::ConnectionInfo& info) {
    using namespace StreamFormats;
    if (info.sourceSubunitType == SourceSubunitType::kNotConnected) {
        ASFW_LOG_V3(MusicSubunit, "MusicSubunit: Plug %u is not connected",
                      plugs_[index].plugID);
    } else {
        ASFW_LOG_V1(MusicSubunit, "MusicSubunit: Plug %u connected to source plug %u (subunit type 0x%02x, id %u)",
                     plugs_[index].plugID,
                     info.sourcePlugNumber,
                     static_cast<unsigned>(info.sourceSubunitType),
                     info.sourceSubunitID);
    }
}

void MusicSubunit::SetAudioVolume(ASFW::Protocols::AVC::IAVCCommandSubmitter& submitter, uint8_t plugId, int16_t volume, std::function<void(bool)> completion) {
    // Target Audio Subunit 0 (0x01 << 3 | 0 = 0x08)
    uint8_t subunitAddr = (static_cast<uint8_t>(AVCSubunitType::kAudio) << 3) | 0;
    
    // Volume data: 2 bytes, big endian
    std::vector<uint8_t> data;
    data.push_back(static_cast<uint8_t>((volume >> 8) & 0xFF));
    data.push_back(static_cast<uint8_t>(volume & 0xFF));
    
    auto cmd = std::make_shared<AudioFunctionBlockCommand>(
        submitter,
        subunitAddr,
        AudioFunctionBlockCommand::CommandType::kControl,
        plugId,
        AudioFunctionBlockCommand::ControlSelector::kVolume,
        data
    );
    
    cmd->Submit([completion, plugId](AVCResult result, const std::vector<uint8_t>&) {
        if (IsSuccess(result)) {
            ASFW_LOG_V1(MusicSubunit, "MusicSubunit: Set Audio Volume success (plug %d)", plugId);
            completion(true);
        } else {
            ASFW_LOG_V1(MusicSubunit, "MusicSubunit: Set Audio Volume failed: result=%d", static_cast<int>(result));
            completion(false);
        }
    });
}

void MusicSubunit::SetAudioMute(ASFW::Protocols::AVC::IAVCCommandSubmitter& submitter, uint8_t plugId, bool mute, std::function<void(bool)> completion) {
    // Target Audio Subunit 0
    uint8_t subunitAddr = (static_cast<uint8_t>(AVCSubunitType::kAudio) << 3) | 0;
    
    // Mute: 0x70 (On), 0x60 (Off)
    uint8_t muteVal = mute ? 0x70 : 0x60;
    
    auto cmd = std::make_shared<AudioFunctionBlockCommand>(
        submitter,
        subunitAddr,
        AudioFunctionBlockCommand::CommandType::kControl,
        plugId,
        AudioFunctionBlockCommand::ControlSelector::kMute,
        std::vector<uint8_t>{muteVal}
    );
    
    cmd->Submit([completion, cmd](AVCResult result, const std::vector<uint8_t>&) {
        if (IsSuccess(result)) {
            ASFW_LOG_V1(MusicSubunit, "MusicSubunit: Set Audio Mute success");
            completion(true);
        } else {
            ASFW_LOG_V1(MusicSubunit, "MusicSubunit: Set Audio Mute failed: result=%d", static_cast<int>(result));
            completion(false);
        }
    });
}

} // namespace ASFW::Protocols::AVC::Music
