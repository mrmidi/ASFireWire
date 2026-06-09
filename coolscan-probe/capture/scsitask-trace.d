#!/usr/sbin/dtrace -qs
/*
 * scsitask-trace.d — capture the SCSI CDBs + data-out payloads VueScan sends to the
 * CoolScan 9000 via SCSITaskLib (Apple's generic SCSI user-client plugin).
 *
 * WHY SCSITaskLib (not IOFireWireSBP2Lib): on Sequoia the SBP-2 LUN is bridged
 * in-kernel by IOFireWireSerialBusProtocolTransport → IOSCSIPeripheralDeviceNub,
 * and VueScan opens a SCSITaskUserClient on the nub (verified via IORegistry:
 * "IOUserClientCreator" = pid of VueScan). No FireWire lib is ever mapped into
 * the process — sbp2-trace.d can never fire. SCSITaskLib symbols verified by nm
 * on Sequoia 15.7.4:
 *   SCSITaskClass::SetCommandDescriptorBlock(UInt8 *cdb, UInt8 len)
 *   SCSITaskClass::SetScatterGatherEntries(IOVirtualRange *r, UInt8 n, UInt64 xfer, UInt8 dir)
 *   SCSITaskClass::ExecuteTask[Sync|Async]()
 * plus s*-static wrappers (each call can print twice — dedupe when reading).
 *
 * STASH-AT-ENTRY / DUMP-AT-RETURN: dtrace copyin() cannot take page faults; dumping
 * at :entry hit "invalid address" on resident-looking buffers. By :return the callee
 * has read the buffer itself, so the page is guaranteed resident. The inner
 * (instance) return dumps and clears the stash, so the static wrapper's return
 * doesn't print a duplicate.
 *
 * RUN (9000 powered + FH 35mm holder in, SIP dtrace-restriction off, VueScan running
 * with Source = the 9000):
 *   sudo dtrace -Z -qs scsitask-trace.d -p "$(pgrep -x VueScan)" -o capture.txt
 *   → VueScan: Preview (frame detect + SET BOUNDARY), then Scan ONE frame at LOW dpi.
 *   → Ctrl-C. Look for [CDB] 2a 00 88 (SET BOUNDARY) / 24 (SET WINDOW) and the
 *     [DATA-OUT] dump that follows each.
 */
#pragma D option quiet
#pragma D option bufsize=32m
#pragma D option dynvarsize=16m
#pragma D option zdefs

/* ---- CDB: (this, UInt8 *cdb, UInt8 len) → arg1=cdb, arg2=len (same in s-wrapper) */
pid$target:SCSITaskLib:*SetCommandDescriptorBlock*:entry
{
    self->cdbPtr = arg1;
    self->cdbLen = arg2;
}

pid$target:SCSITaskLib:*SetCommandDescriptorBlock*:return
/self->cdbPtr != 0/
{
    this->n = self->cdbLen > 16 ? 16 : self->cdbLen;
    printf("\n[CDB %s] len=%d\n", probefunc, (int)self->cdbLen);
    tracemem(copyin(self->cdbPtr, this->n), 16, this->n);
    self->cdbPtr = 0;
}

/* ---- SG list: (this, IOVirtualRange *ranges, UInt8 count, UInt64 xfer, UInt8 dir)
 * dir: 1 = host→target (data-OUT — the geometry payload), 2 = target→host.
 * IOVirtualRange = {uint64 addr, uint64 len}. Read range[0] at return (resident),
 * remember it for the data dump at ExecuteTask time. */
pid$target:SCSITaskLib:*SetScatterGatherEntries*:entry
{
    self->sgPtr  = arg1;
    self->sgCnt  = arg2;
    self->sgXfer = arg3;
    self->sgDir  = arg4;
}

pid$target:SCSITaskLib:*SetScatterGatherEntries*:return
/self->sgPtr != 0/
{
    this->rng     = (uint64_t *)copyin(self->sgPtr, 16);
    self->bufAddr = this->rng[0];
    self->bufLen  = this->rng[1];
    printf("\n[SG %s] dir=%d nranges=%d xfer=%d addr=0x%llx len0=%d\n",
           probefunc, (int)self->sgDir, (int)self->sgCnt, (int)self->sgXfer,
           (unsigned long long)this->rng[0], (int)this->rng[1]);
    self->sgPtr = 0;
}

/* ---- Submission marker, and data-OUT payload dump. Dump at :return — by then the
 * kernel has wired/copied the buffer (IOMemoryDescriptor prepare), so it is resident.
 * The data-out buffer is not modified by the device, so return-time content == sent. */
pid$target:SCSITaskLib:*ExecuteTask*:entry
{
    printf("[EXEC %s]\n", probefunc);
}

pid$target:SCSITaskLib:*ExecuteTask*:return
/self->sgDir == 1 && self->bufAddr != 0/
{
    this->n = self->bufLen > 256 ? 256 : self->bufLen;
    printf("\n[DATA-OUT @ %s] addr=0x%llx len=%d\n",
           probefunc, (unsigned long long)self->bufAddr, (int)self->bufLen);
    tracemem(copyin(self->bufAddr, this->n), 256, this->n);
    self->bufAddr = 0;
}
