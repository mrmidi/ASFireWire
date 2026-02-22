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

    @Test func duetSidecarStateTransitionsPerGuid() {
        let connector = ASFWDriverConnector()
        let guid: UInt64 = 0x0003_DB00_01DD_DD11

        var first = DuetStateSnapshot()
        first.inputParams = DuetInputParams(gains: [20, 21],
                                            polarities: [false, true],
                                            xlrNominalLevels: [.microphone, .professional],
                                            phantomPowerings: [true, false],
                                            sources: [.xlr, .phone],
                                            clickless: false)
        connector.setDuetCachedState(guid: guid, snapshot: first)

        let cached1 = connector.getDuetCachedState(guid: guid)
        #expect(cached1?.inputParams?.gains == [20, 21])
        #expect(cached1?.inputParams?.clickless == false)

        var second = cached1 ?? DuetStateSnapshot()
        second.inputParams?.clickless = true
        second.mixerParams = DuetMixerParams(outputs: [
            DuetMixerCoefficients(analogInputs: [100, 200], streamInputs: [300, 400]),
            DuetMixerCoefficients(analogInputs: [500, 600], streamInputs: [700, 800])
        ])
        connector.setDuetCachedState(guid: guid, snapshot: second)

        let cached2 = connector.getDuetCachedState(guid: guid)
        #expect(cached2?.inputParams?.clickless == true)
        #expect(cached2?.mixerParams?.gain(destination: 1, source: 3) == 800)

        connector.clearDuetCachedState(guid: guid)
        #expect(connector.getDuetCachedState(guid: guid) == nil)
    }
}
