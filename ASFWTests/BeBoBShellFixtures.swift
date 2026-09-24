// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// BeBoBShellFixtures.swift — VERBATIM captures from the BridgeCo Virtual UART
// shell on an M-Audio FireWire 1814 (node 2, 48 kHz, S/PDIF, audio running),
// 2026-08-18. Do not hand-edit these to make a parser pass: they are the
// device's own output and they are the specification.
//
// The previous fixtures for this code were invented ("rxPackets:  12450"), and
// the tests passed against a format the firmware never emits.

enum BeBoBShellFixtures {
    static let sysStat = #"""
/cfg/dev/isodrv/ClockDividers/syt/48000>sys stat
global statistic:
rxIsr        9691179    rxStreamInvalid     0    MDBAliveErrors      0
txIsr        6116574    txStreamInvalid     0    distortErrors       0
rxIsrH0            0    dmaBusyWaitCnt      0    rxLLCAlarm          0
rxIsrH1            0           rxIsrH2      0    rxLLCAlarmIntrv     0

Output Stream statistic:
    iso channel      58        1       60
           dest      AV     1394       AV
          speed       0        2        0
        txWrite 4515638  3880170  4515650
        txReady 4515654  3880188  4515662
      txDelayed 1580165  1293405  1580169
         txSend       0        0        0
          txDma 4515722  3880254  4515728
   noInterption       0        0        0
   txQFillLevel    33 %     33 %     41 %
W      txQEmpty   10257     3108      355
E       txQFull       0        0        0
E     MdbPtrFull      0        0        0
E     MdbPktFull      0        0        0
E      txDmaBusy      0        0        0
E       pkt Past      0        0        0
E     pkt Future      4        0        0
E     pktSytDiff      0        0        0
S      SytOffset   9216    12288     4608
S        SytCorr      0        0        0
S        FFLimit  53448    56520    48840

Input Stream statistic:
    iso channel       0       61       62
         source    1394       AV       AV
    linStartSyt 9668096        0        0
    outStartSyt       0        0    44731
      rxPackets 3882332        0  4516419
   blockingConv       0        0        0
   rxQFillLevel    80 %      0 %      0 %
  PoolFillLevel    90 %      0 %     19 %
    onlyHeaders       0        0        0
    rxEMIEtyPkt       0        0        0
W    rxEmptyPkt 1294172        0        0
E       rxNoMem    1652        0      246
E      rxToLong       0        0        0
E   rxPktToLong       0        0        0
E  rxPktToSmall       0        0        0
E     rxDmaBusy       0        0        0
E       rxQFull       0        0        0
E    CtrDiffErr       1        0        2
E    SytDiffErr       2        0        0
E    SumDiffErr       0        0        0
E     BCOHdrErr       0        0        0
      counter1        0        0        0
      counter2        0        0        0
/cfg>
"""#

    static let sysAvStatAll = #"""

======================================================================
$ sys avstat all
======================================================================
sys avstat all
TGEN                  : FE700000
TgPhaseErr(FE700800)      : 00000001, 00000000, 00000000
TgInt(FE700C00)           : 00000003
    SetPhaseErrUpdate : 00000001
    SetTgInLock       : 00000001
    SetTgSytMiss      : 00000000
    SetTgCmapTrig     : 00000000
    IntMask           : 00000000

AV1                   : FE900000
AV1FifoFull(FE900800)     : 00000096
AV1FifoEmpty(FE900804)    : 00000069
AV1DtaFlowErr(FE900808)   : 00000000
AV1TSErr(FE90080C)        : 00000000
AV1InSync(FE900810)       : 00000096
AV1AESErr(FE900814)       : 00000000
AV1Int(FE900C00)  : 00000006
    DataErr   : 00000000
    SetTSErr  : 00000001
    SyncWarn  : 00000001
    AESErr    : 00000000
    IntMask   : 00000000

AV2                   : FEA00000
AV2FifoFull(FEA00800)     : 00000000
AV2FifoEmpty(FEA00804)    : 000000FF
AV2DtaFlowErr(FEA00808)   : 00000000
AV2TSErr(FEA0080C)        : 00000000
AV2InSync(FEA00810)       : 0000002D
AV2AESErr(FEA00814)       : 00000000
AV2Int(FEA00C00)  : 00000004
    DataErr   : 00000000
    SetTSErr  : 00000000
    SyncWarn  : 00000001
    AESErr    : 00000000
    IntMask   : 00000000

FRAMER/DEFRAMER    : FED00000
FrmStatus0(FED00800)   : 00000003
FrmStatus1(FED00804)   : 00000000
FramerInt(FED00C00)    : 00000004
    DBCMismatch    : 00000000
    FMTMismatch    : 00000000
    SIDMismatch    : 00000001
    HeaderMismatch : 00000000
    CIPMismatch    : 00000000
    AVOutOfSync    : 00000000
    OutOfSync      : 00000000
    PayloadMismatch: 00000000
    FrmIntMask     : 00000000

MDB                  : FEE00000
IsoTxAlarmID(FEE00800)   : 0000003E
IsoTxAlarmType(FEE00800) : 00000003
IsoRxAlarmID(FEE00804)   : 00000000
IsoRxAlarmType(FEE00804) : 00000000
ArmTxAlarmID(FEE00808)   : 00000000
ArmTxAlarmType(FEE00808) : 00000000
LlcRxAlarmID(FEE0080C)   : 00000000
LlcRxAlarmType(FEE0080C) : 00000002
TxDiscard(FEE00810)      : 00000000
TxRetry(FEE00810)        : 00000000
RxAsyFlush(FEE00810)     : 00000000
MDBInt(FEE00C00)    : 00000040
    TxDiscard   : 00000000
    TxRetry     : 00000000
    RXAsyFlush  : 00000000
    LlcRXalarm  : 00000000
    Armalarm    : 00000000
    IsoRXalarm  : 00000000
    IsoTXalarm  : 00000001
    IntMask     : 00000008

LLC             : FEF00000
LLC Status(FEF00800): 00000008
LLCInt(FEF00C00)    : D018053C
    IntMask     : 00000000

/cfg>
"""#

    static let fwShow = #"""

======================================================================
$ fw show
======================================================================
fw show
Selected Iso Channels:
  LineIn .....     = 62
  SpdifAdatIn .... = 61
  SpdifAdatOut ... = 58
  MixerOut ...     = 60
  SwReturn ...     = 0
  SwSend .....     = 1
  MidiIn .....     = -1
  MidiOut ....     = -1
  DataOut ....     = -1
  AC3 ........     = -1
Sampling Frequency = 48kHz
Sync Source        = Internal Digital Input Sync
Audio State        = Running
Timer Action       = CSP Check Mixer
Spdif Source       = RCA
Input Source       = SPDIF
Output Source      = SPDIF
Sync Source        = INTDIG
/cfg>
"""#
}
