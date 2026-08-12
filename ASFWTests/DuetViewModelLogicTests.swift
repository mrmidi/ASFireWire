import Foundation
import Testing
@testable import ASFW

struct DuetViewModelLogicTests {
    @Test func faderBankSelectionKeepsDestinationsIndependent() {
        let connector = ASFWDriverConnector()
        let viewModel = DuetControlViewModel(connector: connector)

        viewModel.selectedOutputBank = .output1
        viewModel.setMixerGain(source: 0, gain: 1200)

        #expect(viewModel.mixerParams.gain(destination: 0, source: 0) == 1200)
        #expect(viewModel.mixerParams.gain(destination: 1, source: 0) == 0)

        viewModel.selectedOutputBank = .output2
        viewModel.setMixerGain(source: 0, gain: 2500)

        #expect(viewModel.mixerParams.gain(destination: 0, source: 0) == 1200)
        #expect(viewModel.mixerParams.gain(destination: 1, source: 0) == 2500)
    }

    @Test func duetSidecarStateTransitionsPerUnitInstance() {
        let connector = ASFWDriverConnector()
        let unitID = UnitInstanceID(
            device: DeviceInstanceID(41),
            unitDirectoryOffset: 0x28
        )

        var first = DuetStateSnapshot()
        first.inputParams = DuetInputParams(gains: [20, 21],
                                            polarities: [false, true],
                                            xlrNominalLevels: [.microphone, .professional],
                                            phantomPowerings: [true, false],
                                            sources: [.xlr, .phone],
                                            clickless: false)
        connector.setDuetCachedState(unitID: unitID, snapshot: first)

        let cached1 = connector.getDuetCachedState(unitID: unitID)
        #expect(cached1?.inputParams?.gains == [20, 21])
        #expect(cached1?.inputParams?.clickless == false)

        var second = cached1 ?? DuetStateSnapshot()
        second.inputParams?.clickless = true
        second.mixerParams = DuetMixerParams(outputs: [
            DuetMixerCoefficients(analogInputs: [100, 200], streamInputs: [300, 400]),
            DuetMixerCoefficients(analogInputs: [500, 600], streamInputs: [700, 800])
        ])
        connector.setDuetCachedState(unitID: unitID, snapshot: second)

        let cached2 = connector.getDuetCachedState(unitID: unitID)
        #expect(cached2?.inputParams?.clickless == true)
        #expect(cached2?.mixerParams?.gain(destination: 1, source: 3) == 800)

        connector.clearDuetCachedState(unitID: unitID)
        #expect(connector.getDuetCachedState(unitID: unitID) == nil)
    }

    @Test func duetCacheIsReplacedAcrossGenerationAndReplug() {
        let connector = ASFWDriverConnector()
        let original = UnitInstanceID(
            device: DeviceInstanceID(41),
            unitDirectoryOffset: 0x28
        )
        connector.reconcileDuetCachedStateRoutes([original.device: 17])
        connector.setDuetCachedState(unitID: original, snapshot: DuetStateSnapshot())
        #expect(connector.getDuetCachedState(unitID: original) != nil)

        connector.reconcileDuetCachedStateRoutes([original.device: 18])
        #expect(connector.getDuetCachedState(unitID: original) == nil)

        let replugged = UnitInstanceID(
            device: DeviceInstanceID(42),
            unitDirectoryOffset: original.unitDirectoryOffset
        )
        connector.setDuetCachedState(unitID: original, snapshot: DuetStateSnapshot())
        connector.reconcileDuetCachedStateRoutes([replugged.device: 19])
        #expect(connector.getDuetCachedState(unitID: original) == nil)
        #expect(connector.getDuetCachedState(unitID: replugged) == nil)
    }
}
