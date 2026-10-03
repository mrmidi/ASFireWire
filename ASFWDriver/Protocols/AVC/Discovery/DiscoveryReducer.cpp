// SPDX-License-Identifier: Apache-2.0
#include "DiscoveryReducer.hpp"
#include "../Descriptors/DescriptorTypeCodes.hpp"
#include <algorithm>
#include <utility>
#include <type_traits>

namespace ASFW::AVC::DiscoveryEngine {

namespace {
/// Controls[] bit 0 is read as the most significant bit of the element: this matches the Phase 88
/// capture (`c0 00` = Mute + Volume), not a literal reading of TA 1999008 Table 8.3 ("Bit 0: Mute"),
/// and the Duet's bitmap fits neither order, so mute and volume are always probed and every other
/// control is probed only if this bit says so. Open question F8 in magic-numbers-audit.md.
constexpr uint16_t kControlBitmapFirstBit = 0x8000;
} // namespace
namespace {
template<class... T> struct Visit : T... { using T::operator()...; };
template<class... T> Visit(T...) -> Visit<T...>;
using D = ParsedDescriptors::AudioSubunitDescriptorParser;

// Route loss and a broken transport end discovery. A timeout or an allowlist
// refusal (nothing was sent) only fails that probe: optional discovery is
// independent, and a device that ignores one opcode must not lose the rest.
bool TransportFailure(const AvcError& e) {
    return e.kind == AvcErrorKind::kBusReset || e.kind == AvcErrorKind::kTransportError;
}
/// Count a probe's timeout; true when the unit has stopped answering.
bool UnitStoppedAnswering(State& s, const std::optional<AvcError>& error) {
    if (!error || error->kind != AvcErrorKind::kTimeout) { s.consecutiveTimeouts = 0; return false; }
    return ++s.consecutiveTimeouts >= kMaxConsecutiveTimeouts;
}
void Finish(Transition& t, std::optional<AvcError> error = {}, bool cancelled = false) {
    ++t.state.serial; // Invalidate an outstanding reply before freezing/releasing the builder.
    if (!error) error = t.state.builder.terminalError;
    t.state.builder.terminalError = error;
    t.state.builder.cancelled = cancelled;
    t.state.builder.complete = !error && !cancelled;
    for (auto& contents : t.state.builder.contents)
        if (contents.audio) D::ResolveNames(*contents.audio, contents.text);
    t.state.phase = Terminal{};
    t.actions.emplace_back(Commit{std::make_shared<const DiscoverySnapshot>(std::move(t.state.builder))});
}
SubunitContents& Contents(State& s, SubunitId id) {
    auto it = std::find_if(s.builder.contents.begin(), s.builder.contents.end(),
                          [id](const auto& c) { return c.id == id; });
    if (it != s.builder.contents.end()) return *it;
    s.builder.contents.push_back(SubunitContents{.id = id});
    return s.builder.contents.back();
}
PlugContents& Plug(State& s, SubunitAddress address, Cmd::PlugDirection direction, uint8_t id) {
    auto it = std::find_if(s.builder.plugs.begin(), s.builder.plugs.end(), [&](const auto& p) {
        return p.address == address && p.direction == direction && p.id.value == id;
    });
    if (it != s.builder.plugs.end()) return *it;
    s.builder.plugs.push_back(PlugContents{.address = address, .direction = direction, .id = {id}});
    return s.builder.plugs.back();
}
void Insert(State& s, size_t after, std::vector<Probe> probes) {
    s.probes.insert(s.probes.begin() + static_cast<ptrdiff_t>(after + 1),
                    std::make_move_iterator(probes.begin()), std::make_move_iterator(probes.end()));
}
void Formats(State& s, SubunitAddress address, uint8_t inputs, uint8_t outputs) {
    for (auto direction : {Cmd::PlugDirection::kInput, Cmd::PlugDirection::kOutput}) {
        const unsigned count = direction == Cmd::PlugDirection::kInput ? inputs : outputs;
        for (unsigned i = 0; i < count; ++i) {
            const auto id = static_cast<uint8_t>(i);
            (void)Plug(s, address, direction, id);
            if (address.IsUnit()) s.probes.emplace_back(Cmd::PlugSignalFormatCommand{
                .operands = {.direction = direction == Cmd::PlugDirection::kInput ?
                    Cmd::PlugSignalDirection::kInput : Cmd::PlugSignalDirection::kOutput, .plugId = id}});
            const auto plug = address.IsUnit() ? Cmd::PlugAddress::UnitPlug(direction, Cmd::UnitPlugType::kPcr, id) :
                                                Cmd::PlugAddress::SubunitPlug(direction, id);
            for (auto form : {Cmd::StreamFormatSubfunction::kSingle, Cmd::StreamFormatSubfunction::kList})
                s.probes.emplace_back(FormatProbe{Cmd::StreamFormatCommand{.address = address,
                    .operands = {.form = form, .plug = plug}}});
        }
    }
}
// Checkpoints append their work before the next checkpoint. All wire actions
// use the phase-2 codecs (GeneralCommands.hpp and StreamFormatCommand.hpp cite
// the corresponding ta1394/Linux/Apple layouts); this reducer adds no codec.
void Expand(State& s, Checkpoint point) {
    switch (point) {
    case Checkpoint::Formats:
        Formats(s, SubunitAddress::Unit(), s.builder.unit.unitPlugs.isochronousInputs,
                s.builder.unit.unitPlugs.isochronousOutputs);
        for (const auto& sub : s.builder.unit.subunits)
            if (sub.id.type == SubunitType::kMusic || sub.id.type == SubunitType::kAudio)
                Formats(s, sub.id.ToAddress(), sub.plugs.destinationPlugs, sub.plugs.sourcePlugs);
        s.probes.emplace_back(Checkpoint::Descriptors); break;
    case Checkpoint::Descriptors:
        s.probes.emplace_back(DescriptorProbe{SubunitId{}, Cmd::DescriptorSpecifier::SubunitStatus()});
        for (const auto& sub : s.builder.unit.subunits) {
            if (sub.id.type == SubunitType::kMusic)
                s.probes.emplace_back(DescriptorProbe{sub.id, Cmd::DescriptorSpecifier::SubunitStatus()});
            if (sub.id.type == SubunitType::kAudio)
                s.probes.emplace_back(DescriptorProbe{sub.id, Cmd::DescriptorSpecifier::SubunitIdentifier()});
        }
        s.probes.emplace_back(Checkpoint::Routes); break;
    case Checkpoint::Routes:
        for (const auto& plug : s.builder.plugs) {
            if (plug.address.IsUnit() && plug.direction == Cmd::PlugDirection::kOutput)
                s.probes.emplace_back(Cmd::QuerySignalSource(
                    Cmd::SignalAddress::UnitIsochronousPlug(plug.id.value)));
            else if (!plug.address.IsUnit() && plug.direction == Cmd::PlugDirection::kInput)
                s.probes.emplace_back(Cmd::QuerySignalSource(
                    Cmd::SignalAddress::SubunitPlug(plug.address, plug.id.value)));
        }
        s.probes.emplace_back(Checkpoint::Controls); break;
    case Checkpoint::Controls:
        for (const auto& c : s.builder.contents) {
            if (!c.audio) continue;
            for (const auto& block : c.audio->functionBlocks) {
                // Current selector input by STATUS (Linux bebob terratec/phase88.rs:74-81,232).
                // No SPECIFIC INQUIRY of inputs: no reference sends it.
                if (block.type == ParsedDescriptors::AudioFunctionBlockType::kSelector)
                    s.probes.emplace_back(SelectorProbe{Cmd::SelectorCommand{.address = c.id.ToAddress(),
                        .operands = {.functionBlockId = block.id}}});
                if (block.type != ParsedDescriptors::AudioFunctionBlockType::kFeature) continue;
                // Controls are confirmed by STATUS; the descriptor bitmap is only a hint.
                // The Duet's bitmap matches neither bit order while STATUS shows mute and
                // volume on channels 0-2 (documentation/avc-rebuild/fixtures/duet_descriptors.md),
                // so mute and volume are always asked on the master and every cluster
                // channel (Linux bebob lib.rs:312-321 reads volume this way). Other
                // controls only where the bitmap advertises them (Audio Subunit 1.0
                // Table 8.3: the first control occupies the most significant bit).
                const size_t channels = std::max<size_t>(block.channelControls.size(), block.clusterChannels);
                for (size_t ch = 0; ch <= channels; ++ch) {
                    const uint16_t bits = ch == 0 ? block.masterControls :
                        (ch - 1 < block.channelControls.size() ? block.channelControls[ch - 1] : 0);
                    for (size_t bit = 0; bit < Cmd::kFeatureControlWidths.size(); ++bit) {
                        const auto control = Cmd::kFeatureControlWidths[bit].control;
                        const bool always = control == Cmd::FeatureControl::kMute || control == Cmd::FeatureControl::kVolume;
                        if ((always || (bits & (kControlBitmapFirstBit >> bit))) && Cmd::kFeatureControlWidths[bit].width)
                            s.probes.emplace_back(Cmd::FeatureCommand{.address = c.id.ToAddress(),
                                .operands = {.functionBlockId = block.id, .channel = static_cast<uint8_t>(ch),
                                    .control = control}});
                    }
                }
            }
        }
        for (const auto& c : s.builder.contents) {
            if (!c.music) continue;
            for (const auto& plug : c.music->plugs) {
                if (!plug.isDestination || plug.usage != ParsedDescriptors::kMusicPlugUsageSync) continue;
                const auto destination = Cmd::SignalAddress::SubunitPlug(c.id.ToAddress(), plug.plugId);
                std::vector<Cmd::SignalAddress> candidates;
                for (unsigned i = 0; i < s.builder.unit.unitPlugs.isochronousInputs; ++i)
                    candidates.push_back(Cmd::SignalAddress::UnitIsochronousPlug(static_cast<uint8_t>(i)));
                for (unsigned i = 0; i < s.builder.unit.unitPlugs.externalInputs; ++i)
                    candidates.push_back(Cmd::SignalAddress::UnitExternalPlug(static_cast<uint8_t>(i)));
                for (const auto& source : c.music->plugs) if (!source.isDestination)
                    candidates.push_back(Cmd::SignalAddress::SubunitPlug(c.id.ToAddress(), source.plugId));
                for (auto source : candidates)
                    s.probes.emplace_back(ClockProbe{Cmd::CanConnectSignalSource(source, destination)});
            }
        }
        s.probes.emplace_back(Checkpoint::Extension); break;
    case Checkpoint::Extension: s.probes.emplace_back(Checkpoint::Commit); break;
    case Checkpoint::Commit: break;
    }
}
Expected<CommandFrame> Encode(State& s, Probe& p) {
    return std::visit(Visit{
        [&](FormatProbe& f) {
            auto& op = f.command.operands;
            op.opcode = (s.opcodePolicy == IAvcUnit::StreamFormatOpcodePolicy::kLearn && (s.usesSupportOpcode || f.fallback)) || s.opcodePolicy == IAvcUnit::StreamFormatOpcodePolicy::kSupportOnly ?
                Cmd::StreamFormatOpcode::kStreamFormatSupport : Cmd::StreamFormatOpcode::kExtendedStreamFormat;
            return f.command.Encode(CommandType::kStatus);
        },
        [](SelectorProbe& f) { return f.command.Encode(CommandType::kStatus); },
        // Sync-connection candidates by SPECIFIC INQUIRY, as FFADO
        // avc_plug.cpp:670-690 (inquireConnnection) from avc_unit.cpp:823-845.
        [](ClockProbe& f) { return f.command.Encode(CommandType::kSpecificInquiry); },
        [](DescriptorProbe&) -> Expected<CommandFrame> { return Fail(AvcErrorKind::kInvalidArgument); },
        [](Checkpoint&) -> Expected<CommandFrame> { return Fail(AvcErrorKind::kInvalidArgument); },
        [](auto& command) { return command.Encode(CommandType::kStatus); }
    }, p);
}
void Advance(Transition& t, size_t index) {
    auto& s = t.state;
    while (index < s.probes.size()) {
        if (s.probes.size() > kMaxProbeOperations || s.serial >= kMaxProbeOperations * 2) {
            Finish(t, AvcError::Of(AvcErrorKind::kUnsupported)); return;
        }
        if (auto* checkpoint = std::get_if<Checkpoint>(&s.probes[index])) {
            const auto point = *checkpoint; // Expand may reallocate probes.
            if (point == Checkpoint::Commit) { Finish(t); return; }
            Expand(s, point);
            if (point != Checkpoint::Extension) { ++index; continue; }
            const OperationIdentity identity{s.builder.session, {++s.serial}, s.builder.route};
            s.phase = Probing{index, identity};
            t.actions.emplace_back(RunExtension{identity}); return;
        }
        const OperationIdentity identity{s.builder.session, {++s.serial}, s.builder.route};
        s.phase = Probing{index, identity};
        if (const auto* descriptor = std::get_if<DescriptorProbe>(&s.probes[index])) {
            t.actions.emplace_back(ReadDescriptor{identity, *descriptor}); return;
        }
        auto frame = Encode(s, s.probes[index]);
        if (!frame) { Finish(t, frame.error()); return; }
        t.actions.emplace_back(Send{identity, *frame}); return;
    }
    Finish(t);
}
Expected<std::span<const uint8_t>> Validate(State& s, Probe& probe, const Expected<OwnedResponse>& response) {
    if (!response) return std::unexpected(response.error());
    auto frame = Encode(s, probe);
    if (!frame) return std::unexpected(frame.error());
    if (response->address != frame->Address()) return Fail(AvcErrorKind::kAddressMismatch);
    if (response->opcode != frame->OpcodeValue()) return Fail(AvcErrorKind::kOpcodeMismatch);
    if (!IsResponseCodeAccepted(response->code, frame->Type())) return std::unexpected(AvcError::Unexpected(response->code));
    return std::span<const uint8_t>{response->operands};
}
template<class Command> auto Decode(const Command& c, const Expected<std::span<const uint8_t>>& bytes, std::optional<AvcError>& error) {
    if (!bytes) return Expected<typename Command::Reply>{std::unexpected(bytes.error())};
    auto result = c.Decode(*bytes);
    if (!result) error = result.error();
    return result;
}
void Handle(Transition& t, size_t index, const Reply& reply) {
    auto& s = t.state;
    Probe probe = s.probes[index]; // Processing may insert/reallocate the queue.
    auto bytes = Validate(s, probe, reply.response);
    if (!bytes && TransportFailure(bytes.error())) { Finish(t, bytes.error()); return; }
    if (UnitStoppedAnswering(s, bytes ? std::nullopt : std::optional{bytes.error()})) { Finish(t, bytes.error()); return; }
    auto frame = Encode(s, probe);
    if (frame) s.builder.outcomes.push_back({frame->Address(), frame->OpcodeValue(), bytes ? std::nullopt : std::optional{bytes.error()}});
    bool retry = false;
    std::optional<AvcError> decodeError;
    std::visit(Visit{
        [&](const Cmd::UnitInfoCommand& c) { if (auto r = Decode(c, bytes, decodeError)) s.builder.unit.info = *r; },
        [&](const Cmd::SubunitInfoCommand& c) {
            auto r = Decode(c, bytes, decodeError);
            if (!r) return;
            if (r->page != c.operands.page || r->extensionCode != c.operands.extensionCode) return;
            std::vector<Probe> next;
            if (r->entryCount == 4 && r->page < 7) next.emplace_back(Cmd::SubunitInfoCommand{
                .operands = {.page = static_cast<uint8_t>(r->page + 1)}});
            for (size_t e = 0; e < r->entryCount; ++e)
                for (unsigned id = 0; id <= r->entries[e].maximumId; ++id) {
                    const SubunitId identity{r->entries[e].type, static_cast<uint8_t>(id)};
                    if (s.builder.unit.FindSubunit(identity.type, identity.id)) continue;
                    s.builder.unit.subunits.push_back({identity});
                    (void)Contents(s, identity);
                    next.emplace_back(Cmd::PlugInfoCommand{.address = identity.ToAddress(),
                        .operands = {.form = Cmd::PlugInfoForm::kSubunit}});
                }
            Insert(s, index, std::move(next));
        },
        [&](const Cmd::PlugInfoCommand& c) {
            auto r = Decode(c, bytes, decodeError); if (!r) return;
            if (r->form == Cmd::PlugInfoForm::kUnitIsoExternal) s.builder.unit.unitPlugs = r->unit;
            else if (r->form == Cmd::PlugInfoForm::kUnitAsync) s.builder.unit.unitAsyncPlugs = r->asynchronous;
            else for (auto& sub : s.builder.unit.subunits) if (sub.id.ToAddress() == c.address) {
                sub.plugs = r->subunit; sub.plugsDiscovered = true;
            }
        },
        [&](const Cmd::PlugSignalFormatCommand& c) {
            if (auto r = Decode(c, bytes, decodeError); r && r->plugId == c.operands.plugId)
                Plug(s, c.address, c.operands.direction == Cmd::PlugSignalDirection::kInput ?
                    Cmd::PlugDirection::kInput : Cmd::PlugDirection::kOutput, r->plugId).signalFormat = *r;
        },
        [&](FormatProbe f) {
            auto r = Decode(f.command, bytes, decodeError);
            if (!r) {
                // Learn only from a plug's first query: NOT IMPLEMENTED past the
                // end of a list is that list's terminator, not opcode evidence.
                const bool firstQuery = f.command.operands.form == Cmd::StreamFormatSubfunction::kSingle ||
                                        f.command.operands.index == 0;
                if (!f.fallback && firstQuery && s.opcodePolicy == IAvcUnit::StreamFormatOpcodePolicy::kLearn &&
                    f.command.operands.opcode == Cmd::StreamFormatOpcode::kExtendedStreamFormat &&
                    r.error().response == ResponseCode::kNotImplemented) {
                    f.fallback = true; s.probes[index] = f; retry = true;
                }
                return;
            }
            if (r->plug != f.command.operands.plug || r->form != f.command.operands.form ||
                (r->form == Cmd::StreamFormatSubfunction::kList && r->index != f.command.operands.index)) return;
            if (f.fallback) { s.usesSupportOpcode = true; t.actions.emplace_back(LearnSupportOpcode{}); }
            auto& plug = Plug(s, f.command.address, r->plug.direction, r->plug.plugId);
            if (r->form == Cmd::StreamFormatSubfunction::kSingle) plug.current = r->format;
            else {
                plug.formations.push_back(r->format);
                if (r->index + 1 < kMaxFormatEntries) {
                    ++f.command.operands.index; Insert(s, index, {f});
                }
            }
        },
        [&](const Cmd::SignalSourceCommand& c) {
            if (auto r = Decode(c, bytes, decodeError); r && r->destination == c.operands.destination)
                Plug(s, r->destination.Subunit(), r->destination.IsUnit() ? Cmd::PlugDirection::kOutput :
                    Cmd::PlugDirection::kInput, r->destination.PlugId()).route = *r;
        },
        [&](const Cmd::FeatureCommand& c) {
            auto r = Decode(c, bytes, decodeError);
            FeatureStatus status{{c.address.Type(), c.address.Id()}, c.operands.functionBlockId,
                c.operands.channel, c.operands.control};
            if (r && r->functionBlockId == c.operands.functionBlockId && r->channel == c.operands.channel && r->control == c.operands.control)
                status.value = *r;
            else status.error = r ? AvcError::Of(AvcErrorKind::kMalformedOperands) : r.error();
            s.builder.features.push_back(std::move(status));
        },
        [&](const SelectorProbe& probe) {
            const auto& c = probe.command;
            auto r = Decode(c, bytes, decodeError);
            const SubunitId id{c.address.Type(), c.address.Id()};
            auto it = std::find_if(s.builder.selectors.begin(), s.builder.selectors.end(), [&](const auto& status) {
                return status.subunit == id && status.blockId == c.operands.functionBlockId;
            });
            if (it == s.builder.selectors.end()) {
                s.builder.selectors.push_back({id, c.operands.functionBlockId}); it = std::prev(s.builder.selectors.end());
            }
            if (!r || r->functionBlockId != c.operands.functionBlockId) return;
            it->value = *r;
        },
        [&](const ClockProbe& c) {
            if (auto r = Decode(c.command, bytes, decodeError); r && r->destination == c.command.operands.destination &&
                c.command.operands.source == r->source) s.builder.confirmedClockRoutes.push_back(*r);
        },
        [](const auto&) {}
    }, probe);
    if (decodeError && !s.builder.outcomes.empty()) s.builder.outcomes.back().error = decodeError;
    const auto error = decodeError ? decodeError : (bytes ? std::optional<AvcError>{} : std::optional{bytes.error()});
    if (error && (std::holds_alternative<Cmd::SubunitInfoCommand>(probe) ||
        (std::holds_alternative<Cmd::PlugInfoCommand>(probe) &&
         std::get<Cmd::PlugInfoCommand>(probe).operands.form == Cmd::PlugInfoForm::kUnitIsoExternal)))
        s.builder.terminalError = error; // Independent optional discovery still runs.
    Advance(t, retry ? index : index + 1);
}
void Descriptor(Transition& t, size_t index, const DescriptorReply& reply) {
    auto& s = t.state;
    const auto probe = std::get<DescriptorProbe>(s.probes[index]);
    const auto& result = reply.result;
    DescriptorBlob blob{probe.subunit, probe.specifier, result.data, result.primaryError, result.cleanupError};
    if (result.cancelled || (result.primaryError && TransportFailure(*result.primaryError)) ||
        UnitStoppedAnswering(s, result.primaryError)) {
        s.builder.descriptors.push_back(std::move(blob));
        Finish(t, result.primaryError, result.cancelled); return;
    }
    if (result.success && probe.subunit.type != SubunitType::kUnit) {
        auto& content = Contents(s, probe.subunit);
        if (probe.subunit.type == SubunitType::kMusic) {
            if (probe.specifier == Cmd::DescriptorSpecifier::SubunitIdentifier()) {
                // The static capabilities (TA 2001007 §5). Captured on a Phase 88; a failure here costs the
                // capabilities only, never the discovery.
                auto parsed = ParsedDescriptors::MusicSubunitIdentifierParser::Parse(result.data);
                if (parsed) content.musicIdentifier = std::move(*parsed); else blob.parseError = parsed.error();
            } else {
                auto parsed = ParsedDescriptors::MusicSubunitDescriptorParser::ParseStatusDescriptor(result.data);
                if (parsed) content.music = std::move(*parsed); else blob.parseError = parsed.error();
                // Read the identifier only from a subunit that has just answered OPEN / READ DESCRIPTOR.
                Insert(s, index, {DescriptorProbe{probe.subunit, Cmd::DescriptorSpecifier::SubunitIdentifier()}});
            }
        } else if (probe.specifier == Cmd::DescriptorSpecifier::SubunitIdentifier()) {
            auto parsed = D::ParseIdentifierDescriptor(result.data);
            if (parsed) {
                content.audio = std::move(*parsed);
                std::vector<Probe> next;
                for (auto id : content.audio->rootListIds) {
                    ListAncestry root;
                    (void)root.push_back(id); // Capacity is the depth budget (>= 1).
                    next.emplace_back(DescriptorProbe{probe.subunit, Cmd::DescriptorSpecifier::ListById(id), 1, root});
                }
                Insert(s, index, std::move(next));
            } else blob.parseError = parsed.error();
        } else {
            auto text = D::ParseTextDatabaseListChecked(result.data);
            if (text) content.text.insert(text->begin(), text->end()); else blob.parseError = text.error();
            auto children = D::ParseChildListIds(result.data, content.audio->sizeOfListId, content.audio->sizeOfObjectId);
            if (children) {
                std::vector<Probe> next;
                for (auto id : *children) {
                    const auto specifier = Cmd::DescriptorSpecifier::ListById(id);
                    const bool seen = std::any_of(s.probes.begin(), s.probes.end(), [&](const auto& queued) {
                        auto* p = std::get_if<DescriptorProbe>(&queued);
                        return p && p->subunit == probe.subunit && p->specifier == specifier;
                    });
                    if (std::find(probe.ancestors.begin(), probe.ancestors.end(), id) != probe.ancestors.end()) {
                        s.builder.textReferences.push_back({probe.subunit, id, TextReferenceKind::Cycle}); continue;
                    }
                    if (seen) {
                        s.builder.textReferences.push_back({probe.subunit, id, TextReferenceKind::Duplicate}); continue;
                    }
                    if (probe.depth >= ParsedDescriptors::kMaxTextListDepth ||
                        std::count_if(s.probes.begin(), s.probes.end(), [&](const Probe& queued) {
                            auto* p = std::get_if<DescriptorProbe>(&queued);
                            return p && p->subunit == probe.subunit &&
                                   p->specifier.bytes[0] == static_cast<uint8_t>(Cmd::DescriptorSpecifierType::kListById);
                        }) + next.size() >= ParsedDescriptors::kMaxTextListNodes) {
                        blob.parseError = ParsedDescriptors::ParseError{0, ParsedDescriptors::ParseErrorKind::BudgetExceeded};
                        s.builder.textReferences.push_back({probe.subunit, id, TextReferenceKind::BudgetExceeded}); break;
                    }
                    auto ancestors = probe.ancestors;
                    if (!ancestors.push_back(id)) { // Deeper than the budget allows.
                        blob.parseError = ParsedDescriptors::ParseError{0, ParsedDescriptors::ParseErrorKind::BudgetExceeded};
                        s.builder.textReferences.push_back({probe.subunit, id, TextReferenceKind::BudgetExceeded}); break;
                    }
                    next.emplace_back(DescriptorProbe{probe.subunit, specifier, probe.depth + 1, ancestors});
                }
                Insert(s, index, std::move(next));
            } else if (!blob.parseError) blob.parseError = children.error();
        }
    }
    s.builder.descriptors.push_back(std::move(blob));
    Advance(t, index + 1);
}
} // namespace
Transition Step(State state, Event event) {
    Transition t{std::move(state), {}};
    if (auto* start = std::get_if<Start>(&event)) {
        if (!std::holds_alternative<Idle>(t.state.phase)) return t;
        t.state.builder.session = start->session; t.state.builder.route = start->route;
        t.state.builder.unit.identity = start->identity;
        t.state.opcodePolicy = start->opcodePolicy; t.state.usesSupportOpcode = start->usesSupportOpcode;
        t.state.probes = {Cmd::UnitInfoCommand{}, Cmd::SubunitInfoCommand{},
            Cmd::PlugInfoCommand{}, Cmd::PlugInfoCommand{.operands = {.form = Cmd::PlugInfoForm::kUnitAsync}}, Checkpoint::Formats};
        Advance(t, 0); return t;
    }
    const auto* running = std::get_if<Probing>(&t.state.phase);
    if (!running) return t;
    const auto operation = running->operation;
    const auto index = running->index;
    std::visit(Visit{
        [&](const Reply& reply) {
            if (reply.operation == operation && !std::holds_alternative<DescriptorProbe>(t.state.probes[index]) &&
                !std::holds_alternative<Checkpoint>(t.state.probes[index])) Handle(t, index, reply);
        },
        [&](const DescriptorReply& reply) {
            if (reply.operation == operation && std::holds_alternative<DescriptorProbe>(t.state.probes[index])) Descriptor(t, index, reply);
        },
        [&](const ExtensionComplete& reply) {
            if (reply.operation == operation && std::holds_alternative<Checkpoint>(t.state.probes[index])) {
                t.state.builder.extension = reply.facts; Advance(t, index + 1);
            }
        },
        [&](Cancel) { Finish(t, {}, true); },
        [&](RouteLost) { Finish(t, AvcError::Of(AvcErrorKind::kBusReset)); },
        [](Start) {}
    }, event);
    return t;
}
} // namespace ASFW::AVC::DiscoveryEngine
