#!/usr/sbin/dtrace -qs
/*
 * sbp2-trace.d — capture the SCSI CDBs + data-out payloads VueScan (or Nikon Scan)
 * sends to a FireWire/SBP-2 scanner, by tracing IOFireWireSBP2Lib's command setters.
 *
 * WHY trace the lib (not IOConnectCallMethod): IOFireWireSBP2Lib writes the CDB into
 * a mapped ORB and describes the data buffer as IOVirtualRange[]; neither travels
 * inline through an IOKit struct, so interposing IOConnectCallMethod won't see them.
 * The lib's setCommandBlock()/setCommandBuffers() receive the CDB/data as plain args
 * BEFORE they are DMA'd — that's what we capture.
 *
 * RUN (on the pre-Tahoe Mac, 9000 powered + FH 35mm holder in, SIP off):
 *   1) Launch VueScan, set Source = the FireWire 9000, Media = the 35mm strip holder.
 *   2) sudo dtrace -qs sbp2-trace.d -p "$(pgrep -x VueScan)" -o capture.txt
 *   3) In VueScan: press Preview (triggers frame detect + SET BOUNDARY), then Scan
 *      ONE frame at a LOW resolution (keeps READ small; geometry is what matters).
 *   4) Ctrl-C dtrace. Inspect capture.txt.
 *
 * IF NO PROBES FIRE (VueScan uses different symbols / its own SBP-2 path):
 *   sudo dtrace -ln 'pid$target::*ommand*:entry' -p "$(pgrep -x VueScan)"
 *   sudo dtrace -ln 'pid$target::*ORB*:entry'     -p "$(pgrep -x VueScan)"
 *   sudo dtrace -ln 'pid$target::*FireWire*:entry' -p "$(pgrep -x VueScan)"
 *   ...then point the probes below at the real function names. The exact
 *   IOFireWireSBP2Lib signatures are in the ASFireWire repo: docs/IOFireWireFamily/.
 *
 * REFINE: arg positions/struct layout below are best-effort (COM-style fn(self,...)).
 * Verify against the first hits + the lib source before trusting the dumps.
 */
#pragma D option quiet
#pragma D option bufsize=32m
#pragma D option dynvarsize=16m
/* IOFireWireSBP2Lib is a CFPlugin loaded lazily when the app opens the device.
 * zdefs lets this script compile with zero matching probes; they arm when the
 * plugin loads. Symbols verified on Sequoia 15.7.4 (nm on the plugin binary):
 *   IOFireWireSBP2LibORB::setCommandBlock(void*, UInt32)
 *   IOFireWireSBP2LibORB::setCommandBuffersAsRanges(FWSBP2VirtualRange*, count, dir, off, len)
 * plus static* wrappers — each call may print twice (wrapper + instance). */
#pragma D option zdefs

/* CDB: setCommandBlock(self, void *buf, UInt32 len)  →  arg1=buf, arg2=len. */
pid$target::*etCommandBlock*:entry
{
    printf("\n[CDB %s] len=%d\n", probefunc, (int)arg2);
    tracemem(copyin(arg1, 16), 16);          /* a SCSI CDB is <= 16 bytes */
}

/* DATA-OUT: setCommandBuffers(self, IOVirtualRange *ranges, UInt32 count, ...).
 * IOVirtualRange (64-bit) = { uint64 address; uint64 length } (16 bytes). Dump
 * range[0] — that is the SET WINDOW / SET BOUNDARY payload we need. */
pid$target::*etCommandBuffers*:entry
{
    self->rng = (uint64_t *)copyin(arg1, 16);
    self->n   = self->rng[1] > 256 ? 256 : self->rng[1];
    /* arg3 = direction (distinguishes data-out from data-in buffers) */
    printf("\n[DATA %s] dir=%d addr=0x%llx len=%d\n",
           probefunc, (int)arg3, (unsigned long long)self->rng[0], (int)self->rng[1]);
    tracemem(copyin(self->rng[0], self->n), 256, self->n);
}

/* Submission markers, so CDB+data can be paired in issue order. */
pid$target::*ubmitORB*:entry  { printf("[SUBMIT %s]\n", probefunc); }
pid$target::*ppendORB*:entry  { printf("[APPEND %s]\n", probefunc); }
