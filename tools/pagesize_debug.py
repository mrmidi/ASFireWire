#!/usr/bin/env python3
"""
pagesize_debug.py - OHCI Descriptor Page Boundary Analyzer

Investigates URE (Unrecoverable Error) issues caused by OHCI descriptor fetches
crossing 4 KiB page boundaries.

PROBLEM:
Many OHCI controllers perform 32-byte DMA reads for descriptors even when the
descriptor is only 16 bytes. If that 32-byte fetch crosses a 4 KiB boundary
and the next page isn't mapped/contiguous in IOVA, you get a fault → URE.

PREDICATE:
For each descriptor at IOVA `desc_iova`:
    page_off = desc_iova & 0xFFF
    UNSAFE if page_off >= 0xFE0 (last 32 bytes of page)
    
Because a 32-byte fetch from offset 0xFE0..0xFFF crosses into the next page.

LINUX FIX (--padding mode):
Leave the last 32 bytes of each page unused. This ensures no descriptor fetch
can ever cross a page boundary, allowing safe multi-page descriptor rings.

Usage:
    python3 pagesize_debug.py --num-packets 84 --blocks-per-packet 3
    python3 pagesize_debug.py --num-packets 200 --padding  # Linux-style multi-page
    python3 pagesize_debug.py --sweep --max-packets 200 --padding
    python3 pagesize_debug.py --base-iova 0x100000000 --num-descriptors 256
"""

import argparse
from dataclasses import dataclass
from typing import List, Optional
import sys

# Default Constants (can be overridden via CLI)
DEFAULT_PAGE_SIZE = 4096  # OHCI assumes 4K pages (1995 spec)
MAC_PAGE_SIZE = 16384     # macOS Apple Silicon native
OHCI_FETCH_SIZE = 32      # OHCI 32-byte prefetch that causes the problem


def get_danger_zone_start(page_size: int) -> int:
    """Calculate the danger zone start for a given page size."""
    return page_size - OHCI_FETCH_SIZE


@dataclass
class DescriptorInfo:
    """Information about a single descriptor."""
    index: int
    iova: int
    page_offset: int
    page_number: int
    is_unsafe: bool
    crosses_page: bool
    packet_index: int
    block_within_packet: int


@dataclass
class AnalysisResult:
    """Result of analyzing a descriptor ring configuration."""
    total_descriptors: int
    descriptor_size: int
    blocks_per_packet: int
    total_bytes: int
    pages_spanned: int
    unsafe_descriptors: List[DescriptorInfo]
    page_crossing_descriptors: List[DescriptorInfo]
    all_descriptors: List[DescriptorInfo]
    first_unsafe_index: Optional[int]
    max_safe_descriptors: int


@dataclass
class PageInfo:
    """Information about a single page in a padded layout."""
    page_number: int
    start_iova: int
    usable_bytes: int
    padding_bytes: int
    first_descriptor_index: int
    descriptor_count: int
    last_descriptor_offset: int


@dataclass
class PaddedLayoutResult:
    """Result of analyzing a padded multi-page descriptor layout."""
    total_descriptors: int
    descriptor_size: int
    blocks_per_packet: int
    padding_per_page: int
    usable_bytes_per_page: int
    descriptors_per_page: int
    total_pages: int
    total_allocated_bytes: int
    wasted_bytes: int
    pages: List[PageInfo]
    all_safe: bool
    

def analyze_descriptor_ring(
    num_descriptors: int,
    descriptor_size: int = 16,
    blocks_per_packet: int = 3,
    base_iova: int = 0,
    page_size: int = DEFAULT_PAGE_SIZE,
    verbose: bool = False
) -> AnalysisResult:
    """
    Analyze a descriptor ring for page boundary issues.
    
    Args:
        num_descriptors: Total number of 16-byte descriptor blocks
        descriptor_size: Size of each descriptor (16 or 32 bytes)
        blocks_per_packet: Number of descriptor blocks per IT packet (Z value)
        base_iova: Starting IOVA address (default: 0 for relative analysis)
        page_size: Page size to use for analysis (default: 4096 for OHCI)
        verbose: Print detailed per-descriptor info
        
    Returns:
        AnalysisResult with all findings
    """
    danger_zone_start = get_danger_zone_start(page_size)
    page_mask = page_size - 1
    page_shift = page_size.bit_length() - 1  # log2(page_size)
    
    all_descriptors = []
    unsafe_descriptors = []
    page_crossing_descriptors = []
    first_unsafe_index = None
    
    for i in range(num_descriptors):
        iova = base_iova + (i * descriptor_size)
        page_offset = iova & page_mask
        page_number = iova >> page_shift
        
        # Check if a 32-byte fetch from this descriptor crosses page boundary
        fetch_end = iova + OHCI_FETCH_SIZE - 1
        fetch_end_page = fetch_end >> page_shift
        crosses_page = fetch_end_page != page_number
        
        # Unsafe if in the danger zone (last 32 bytes of page)
        is_unsafe = page_offset >= danger_zone_start
        
        packet_index = i // blocks_per_packet
        block_within_packet = i % blocks_per_packet
        
        desc_info = DescriptorInfo(
            index=i,
            iova=iova,
            page_offset=page_offset,
            page_number=page_number,
            is_unsafe=is_unsafe,
            crosses_page=crosses_page,
            packet_index=packet_index,
            block_within_packet=block_within_packet,
        )
        
        all_descriptors.append(desc_info)
        
        if is_unsafe:
            unsafe_descriptors.append(desc_info)
            if first_unsafe_index is None:
                first_unsafe_index = i
                
        if crosses_page:
            page_crossing_descriptors.append(desc_info)
    
    total_bytes = num_descriptors * descriptor_size
    pages_spanned = (total_bytes + page_size - 1) // page_size
    
    # Calculate max safe descriptors (before first unsafe)
    max_safe = first_unsafe_index if first_unsafe_index is not None else num_descriptors
    
    return AnalysisResult(
        total_descriptors=num_descriptors,
        descriptor_size=descriptor_size,
        blocks_per_packet=blocks_per_packet,
        total_bytes=total_bytes,
        pages_spanned=pages_spanned,
        unsafe_descriptors=unsafe_descriptors,
        page_crossing_descriptors=page_crossing_descriptors,
        all_descriptors=all_descriptors,
        first_unsafe_index=first_unsafe_index,
        max_safe_descriptors=max_safe,
    )


def calculate_safe_descriptor_count(
    descriptor_size: int = 16,
    blocks_per_packet: int = 3,
    max_pages: int = 1,
    base_offset: int = 0,
) -> int:
    """
    Calculate maximum number of descriptors that can safely fit without
    risking page boundary crossing.
    
    Args:
        descriptor_size: Size of each descriptor (16 bytes typical)
        blocks_per_packet: Blocks per packet (Z value)
        max_pages: Maximum pages to use for descriptor ring
        base_offset: Starting offset within first page
        
    Returns:
        Maximum safe number of descriptor blocks
    """
    # Available space per page, leaving OHCI_FETCH_SIZE unused at end
    safe_per_page = DANGER_ZONE_START - base_offset
    if safe_per_page <= 0:
        return 0
        
    # For first page
    first_page_descriptors = safe_per_page // descriptor_size
    
    # For subsequent full pages
    full_page_safe_space = DANGER_ZONE_START  # Start at offset 0, leave 32 bytes
    descriptors_per_full_page = full_page_safe_space // descriptor_size
    
    if max_pages == 1:
        return first_page_descriptors
    else:
        return first_page_descriptors + (max_pages - 1) * descriptors_per_full_page


def analyze_padded_layout(
    num_descriptors: int,
    descriptor_size: int = 16,
    blocks_per_packet: int = 3,
    base_iova: int = 0,
    padding_bytes: int = OHCI_FETCH_SIZE,
    page_size: int = DEFAULT_PAGE_SIZE,
) -> PaddedLayoutResult:
    """
    Analyze a multi-page descriptor layout with per-page padding.
    
    This implements the Linux firewire-ohci approach: leave padding_bytes
    unused at the end of each page so no descriptor fetch can cross a boundary.
    
    Args:
        num_descriptors: Total number of descriptors needed
        descriptor_size: Size of each descriptor (16 bytes typical)
        blocks_per_packet: Blocks per packet (Z value)
        base_iova: Starting IOVA address
        padding_bytes: Bytes to leave unused at end of each page (default: 32)
        page_size: Page size to use (default: 4096 for OHCI)
        
    Returns:
        PaddedLayoutResult with complete layout analysis
    """
    danger_zone_start = get_danger_zone_start(page_size)
    usable_per_page = page_size - padding_bytes
    descriptors_per_page = usable_per_page // descriptor_size
    
    if descriptors_per_page <= 0:
        # Pathological case
        return PaddedLayoutResult(
            total_descriptors=num_descriptors,
            descriptor_size=descriptor_size,
            blocks_per_packet=blocks_per_packet,
            padding_per_page=padding_bytes,
            usable_bytes_per_page=0,
            descriptors_per_page=0,
            total_pages=0,
            total_allocated_bytes=0,
            wasted_bytes=0,
            pages=[],
            all_safe=False,
        )
    
    total_pages = (num_descriptors + descriptors_per_page - 1) // descriptors_per_page
    pages = []
    
    remaining = num_descriptors
    current_desc_index = 0
    
    for page_num in range(total_pages):
        page_start_iova = base_iova + (page_num * page_size)
        descs_this_page = min(remaining, descriptors_per_page)
        
        # Last descriptor's offset within this page
        if descs_this_page > 0:
            last_offset = (descs_this_page - 1) * descriptor_size
        else:
            last_offset = 0
        
        pages.append(PageInfo(
            page_number=page_num,
            start_iova=page_start_iova,
            usable_bytes=usable_per_page,
            padding_bytes=padding_bytes,
            first_descriptor_index=current_desc_index,
            descriptor_count=descs_this_page,
            last_descriptor_offset=last_offset,
        ))
        
        current_desc_index += descs_this_page
        remaining -= descs_this_page
    
    total_allocated = total_pages * page_size
    actual_used = num_descriptors * descriptor_size
    wasted = total_allocated - actual_used
    
    # All descriptors are safe because none are in the danger zone
    all_safe = True
    for page in pages:
        if page.last_descriptor_offset >= danger_zone_start:
            all_safe = False
            break
    
    return PaddedLayoutResult(
        total_descriptors=num_descriptors,
        descriptor_size=descriptor_size,
        blocks_per_packet=blocks_per_packet,
        padding_per_page=padding_bytes,
        usable_bytes_per_page=usable_per_page,
        descriptors_per_page=descriptors_per_page,
        total_pages=total_pages,
        total_allocated_bytes=total_allocated,
        wasted_bytes=wasted,
        pages=pages,
        all_safe=all_safe,
    )


def print_padded_analysis(result: PaddedLayoutResult, show_pages: bool = True, page_size: int = DEFAULT_PAGE_SIZE):
    """Print padded layout analysis results."""
    danger_zone_start = get_danger_zone_start(page_size)
    
    print("=" * 70)
    print("LINUX-STYLE PADDED LAYOUT ANALYSIS")
    print("=" * 70)
    print()
    
    print(f"Configuration:")
    print(f"  Total descriptors:     {result.total_descriptors}")
    print(f"  Descriptor size:       {result.descriptor_size} bytes")
    print(f"  Blocks per packet:     {result.blocks_per_packet} (Z value)")
    print(f"  Total packets:         {result.total_descriptors // result.blocks_per_packet}")
    print()
    
    print(f"Padding Strategy:")
    print(f"  Padding per page:      {result.padding_per_page} bytes (0x{result.padding_per_page:X})")
    print(f"  Usable per page:       {result.usable_bytes_per_page} bytes (0x{result.usable_bytes_per_page:X})")
    print(f"  Descriptors per page:  {result.descriptors_per_page}")
    print()
    
    print(f"Allocation:")
    print(f"  Total pages needed:    {result.total_pages}")
    print(f"  Total allocated:       {result.total_allocated_bytes} bytes ({result.total_allocated_bytes // 1024} KiB)")
    print(f"  Wasted bytes:          {result.wasted_bytes} ({result.wasted_bytes * 100 // result.total_allocated_bytes:.1f}% overhead)")
    print()
    
    if result.all_safe:
        print(f"✅ ALL DESCRIPTORS SAFE (no page boundary crossings possible)")
    else:
        print(f"⚠️  WARNING: Some descriptors may still cross boundaries!")
    print()
    
    if show_pages and result.pages:
        print("-" * 70)
        print("PAGE LAYOUT:")
        print("-" * 70)
        print(f"{'Page':>4} {'Start IOVA':>16} {'Descs':>6} {'FirstIdx':>8} {'LastOff':>10} {'Status'}")
        print("-" * 70)
        
        for page in result.pages:
            if page.last_descriptor_offset >= DANGER_ZONE_START:
                status = "⚠️ DANGER"
            else:
                margin = DANGER_ZONE_START - page.last_descriptor_offset
                status = f"✅ OK (margin: {margin}B)"
            
            print(f"{page.page_number:>4} 0x{page.start_iova:014X} {page.descriptor_count:>6} {page.first_descriptor_index:>8} 0x{page.last_descriptor_offset:03X}       {status}")
        print()
    
    # Show implementation guidance
    print("-" * 70)
    print("IMPLEMENTATION GUIDANCE:")
    print("-" * 70)
    print()
    print("C++ constants to use:")
    print(f"  constexpr size_t kPageSize = {PAGE_SIZE};")
    print(f"  constexpr size_t kPagePadding = {result.padding_per_page}; // Linux workaround")
    print(f"  constexpr size_t kUsablePerPage = {result.usable_bytes_per_page};")
    print(f"  constexpr size_t kDescriptorsPerPage = {result.descriptors_per_page};")
    print(f"  constexpr size_t kTotalPages = {result.total_pages};")
    print()
    print("Allocation strategy:")
    print(f"  1. Allocate {result.total_pages} contiguous pages ({result.total_allocated_bytes} bytes)")
    print(f"  2. Place max {result.descriptors_per_page} descriptors per page")
    print(f"  3. Leave last {result.padding_per_page} bytes of each page unused")
    print(f"  4. Branch address calculation must skip page gaps!")
    print()


def sweep_padded_mode(max_packets: int, blocks_per_packet: int, descriptor_size: int, page_size: int = DEFAULT_PAGE_SIZE):
    """Sweep with padding to show how many packets can fit safely."""
    print("=" * 70)
    print("SWEEP MODE - Padded Layout (Linux-style)")
    print("=" * 70)
    print()
    
    usable_per_page = page_size - OHCI_FETCH_SIZE
    descriptors_per_page = usable_per_page // descriptor_size
    
    print(f"Configuration: {blocks_per_packet} blocks/packet, {descriptor_size}B descriptors, {page_size}B pages")
    print(f"Padding: {OHCI_FETCH_SIZE} bytes/page → {descriptors_per_page} descriptors/page")
    print()
    print(f"{'Packets':>8} {'Descs':>8} {'Pages':>6} {'Allocated':>12} {'Waste':>8} {'Status'}")
    print("-" * 70)
    
    for num_packets in range(1, max_packets + 1):
        num_descriptors = num_packets * blocks_per_packet
        result = analyze_padded_layout(
            num_descriptors=num_descriptors,
            descriptor_size=descriptor_size,
            blocks_per_packet=blocks_per_packet,
            page_size=page_size,
        )
        
        waste_pct = result.wasted_bytes * 100 // result.total_allocated_bytes if result.total_allocated_bytes > 0 else 0
        status = "✅ SAFE" if result.all_safe else "⚠️ UNSAFE"
        
        print(f"{num_packets:>8} {num_descriptors:>8} {result.total_pages:>6} {result.total_allocated_bytes:>10}B {waste_pct:>6}%  {status}")
    
    print()
    print("-" * 70)
    print(f"With {OHCI_FETCH_SIZE}B padding per page, ALL configurations are safe!")
    print("-" * 70)


def print_analysis(result: AnalysisResult, show_all: bool = False, show_unsafe: bool = True, page_size: int = DEFAULT_PAGE_SIZE):
    """Print analysis results in a human-readable format."""
    danger_zone_start = get_danger_zone_start(page_size)
    
    print("=" * 70)
    print("OHCI DESCRIPTOR PAGE BOUNDARY ANALYSIS")
    print("=" * 70)
    print()
    
    print(f"Configuration:")
    print(f"  Total descriptors:   {result.total_descriptors}")
    print(f"  Descriptor size:     {result.descriptor_size} bytes")
    print(f"  Blocks per packet:   {result.blocks_per_packet} (Z value)")
    print(f"  Total packets:       {result.total_descriptors // result.blocks_per_packet}")
    print(f"  Total ring size:     {result.total_bytes} bytes (0x{result.total_bytes:X})")
    print(f"  Pages spanned:       {result.pages_spanned}")
    print()
    
    print(f"Page Boundary Analysis (32-byte fetch, 4 KiB pages):")
    print(f"  Danger zone:         0x{DANGER_ZONE_START:03X} - 0xFFF (last {OHCI_FETCH_SIZE} bytes)")
    print(f"  Unsafe descriptors:  {len(result.unsafe_descriptors)}")
    print(f"  Page-crossing fetches: {len(result.page_crossing_descriptors)}")
    print()
    
    if result.first_unsafe_index is not None:
        print(f"⚠️  FIRST UNSAFE DESCRIPTOR: #{result.first_unsafe_index}")
        print(f"    Max safe descriptors:  {result.max_safe_descriptors}")
        print(f"    Max safe packets:      {result.max_safe_descriptors // result.blocks_per_packet}")
        print()
    else:
        print(f"✅ ALL DESCRIPTORS SAFE (none in danger zone)")
        print()
    
    if show_unsafe and result.unsafe_descriptors:
        print("-" * 70)
        print("UNSAFE DESCRIPTORS (in danger zone):")
        print("-" * 70)
        print(f"{'Idx':>5} {'IOVA':>14} {'PageOff':>8} {'Page':>5} {'Pkt':>5} {'Blk':>3} {'Status'}")
        print("-" * 70)
        
        for desc in result.unsafe_descriptors[:20]:  # Limit output
            status = "⚠️ UNSAFE" + (" + CROSSES" if desc.crosses_page else "")
            print(f"{desc.index:>5} 0x{desc.iova:012X} 0x{desc.page_offset:03X}    {desc.page_number:>5} {desc.packet_index:>5} {desc.block_within_packet:>3}   {status}")
        
        if len(result.unsafe_descriptors) > 20:
            print(f"  ... and {len(result.unsafe_descriptors) - 20} more unsafe descriptors")
        print()
    
    if show_all:
        print("-" * 70)
        print("ALL DESCRIPTORS:")
        print("-" * 70)
        print(f"{'Idx':>5} {'IOVA':>14} {'PageOff':>8} {'Page':>5} {'Pkt':>5} {'Blk':>3} {'Status'}")
        print("-" * 70)
        
        for desc in result.all_descriptors:
            if desc.is_unsafe:
                status = "⚠️ UNSAFE"
            elif desc.crosses_page:
                status = "⚡ CROSSES"
            else:
                status = "✓"
            print(f"{desc.index:>5} 0x{desc.iova:012X} 0x{desc.page_offset:03X}    {desc.page_number:>5} {desc.packet_index:>5} {desc.block_within_packet:>3}   {status}")
        print()


def print_recommendations(result: AnalysisResult, page_size: int = DEFAULT_PAGE_SIZE):
    """Print fix recommendations based on analysis."""
    danger_zone_start = get_danger_zone_start(page_size)
    
    print("=" * 70)
    print("RECOMMENDATIONS")
    print("=" * 70)
    print()
    
    if not result.unsafe_descriptors:
        print("✅ Your current configuration is safe!")
        print()
        return
    
    # Strategy 1: Reduce descriptor count
    safe_packets = result.max_safe_descriptors // result.blocks_per_packet
    print(f"Option 1: REDUCE DESCRIPTOR COUNT")
    print(f"  Max safe descriptors: {result.max_safe_descriptors}")
    print(f"  Max safe packets:     {safe_packets}")
    print(f"  Adjust kNumPackets to {safe_packets} or less")
    print()
    
    # Strategy 2: Use 32-byte stride
    safe_with_32 = calculate_safe_descriptor_count(
        descriptor_size=32,
        blocks_per_packet=result.blocks_per_packet,
    )
    print(f"Option 2: USE 32-BYTE DESCRIPTOR STRIDE")
    print(f"  Allocate descriptors at 32-byte intervals")
    print(f"  Max safe with 32B stride: {safe_with_32} descriptors")
    print(f"  Max safe packets: {safe_with_32 // result.blocks_per_packet}")
    print()
    
    # Strategy 3: Multi-page with padding
    descriptors_per_page_safe = DANGER_ZONE_START // result.descriptor_size
    pages_needed = (result.total_descriptors + descriptors_per_page_safe - 1) // descriptors_per_page_safe
    print(f"Option 3: MULTI-PAGE WITH PADDING (Linux approach)")
    print(f"  Safe descriptors per 4K page: {descriptors_per_page_safe}")
    print(f"  Pages needed for {result.total_descriptors} descriptors: {pages_needed}")
    print(f"  Leave last 32 bytes of each page unused")
    print()
    
    # Show the math for current issue
    print("-" * 70)
    print("WHY YOUR CURRENT CONFIG FAILS:")
    print("-" * 70)
    first_unsafe = result.unsafe_descriptors[0]
    print(f"  Descriptor #{first_unsafe.index} at IOVA 0x{first_unsafe.iova:X}")
    print(f"  Page offset: 0x{first_unsafe.page_offset:03X} (>= 0x{DANGER_ZONE_START:03X})")
    print(f"  32-byte fetch would read 0x{first_unsafe.iova:X} - 0x{first_unsafe.iova + 31:X}")
    print(f"  This crosses from page {first_unsafe.page_number} into page {first_unsafe.page_number + 1}")
    print(f"  If page {first_unsafe.page_number + 1} isn't mapped → URE!")
    print()


def interactive_mode():
    """Run in interactive mode for exploration."""
    print("=" * 70)
    print("OHCI DESCRIPTOR PAGE DEBUG - INTERACTIVE MODE")
    print("=" * 70)
    print()
    print("Enter values to analyze, or 'q' to quit.")
    print()
    
    while True:
        try:
            num_input = input("Number of packets (or 'q' to quit): ").strip()
            if num_input.lower() == 'q':
                break
                
            num_packets = int(num_input)
            
            blocks = input("Blocks per packet [3]: ").strip()
            blocks_per_packet = int(blocks) if blocks else 3
            
            desc_size = input("Descriptor size [16]: ").strip()
            descriptor_size = int(desc_size) if desc_size else 16
            
            base = input("Base IOVA [0]: ").strip()
            base_iova = int(base, 0) if base else 0
            
            num_descriptors = num_packets * blocks_per_packet
            
            result = analyze_descriptor_ring(
                num_descriptors=num_descriptors,
                descriptor_size=descriptor_size,
                blocks_per_packet=blocks_per_packet,
                base_iova=base_iova,
            )
            
            print()
            print_analysis(result)
            print_recommendations(result)
            print()
            
        except ValueError as e:
            print(f"Invalid input: {e}")
        except KeyboardInterrupt:
            print("\nExiting...")
            break


def sweep_mode(max_packets: int, blocks_per_packet: int, descriptor_size: int, page_size: int = DEFAULT_PAGE_SIZE):
    """Sweep through packet counts to find the breaking point."""
    danger_zone_start = get_danger_zone_start(page_size)
    
    print("=" * 70)
    print("SWEEP MODE - Finding Breaking Point")
    print("=" * 70)
    print()
    print(f"Configuration: {blocks_per_packet} blocks/packet, {descriptor_size}B descriptors, {page_size}B pages")
    print(f"Danger zone: 0x{danger_zone_start:03X} - 0x{page_size-1:03X}")
    print()
    print(f"{'Packets':>8} {'Descs':>8} {'Ring Size':>12} {'Pages':>6} {'Unsafe':>8} {'Status'}")
    print("-" * 70)
    
    last_safe = 0
    first_unsafe = None
    
    for num_packets in range(1, max_packets + 1):
        num_descriptors = num_packets * blocks_per_packet
        result = analyze_descriptor_ring(
            num_descriptors=num_descriptors,
            descriptor_size=descriptor_size,
            blocks_per_packet=blocks_per_packet,
            page_size=page_size,
        )
        
        if result.unsafe_descriptors:
            status = f"⚠️ UNSAFE @ desc #{result.first_unsafe_index}"
            if first_unsafe is None:
                first_unsafe = num_packets
        else:
            status = "✅ SAFE"
            last_safe = num_packets
            
        print(f"{num_packets:>8} {num_descriptors:>8} {result.total_bytes:>10}B {result.pages_spanned:>6} {len(result.unsafe_descriptors):>8}  {status}")
    
    print()
    print("-" * 70)
    print(f"RESULT: Max safe packets = {last_safe}, first unsafe = {first_unsafe}")
    print("-" * 70)


def main():
    parser = argparse.ArgumentParser(
        description="Analyze OHCI descriptor ring for page boundary issues",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Analyze 84 packets with Z=3, 16-byte descriptors
  python3 pagesize_debug.py --num-packets 84
  
  # Same but specify descriptor count directly
  python3 pagesize_debug.py --num-descriptors 252
  
  # Sweep to find breaking point
  python3 pagesize_debug.py --sweep --max-packets 100
  
  # Interactive exploration
  python3 pagesize_debug.py --interactive
  
  # With specific base IOVA
  python3 pagesize_debug.py --num-packets 100 --base-iova 0x100000000
  
  # Linux-style padded layout analysis
  python3 pagesize_debug.py --num-packets 200 --padding
  
  # Sweep with padding to see how many pages you need
  python3 pagesize_debug.py --sweep --max-packets 200 --padding

  # Use macOS 16K page size instead of OHCI default 4K
  python3 pagesize_debug.py --num-packets 100 --page-size 16384
  python3 pagesize_debug.py --sweep --max-packets 500 --page-size 16384
"""
    )
    
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--num-packets", "-p", type=int, help="Number of IT packets")
    group.add_argument("--num-descriptors", "-n", type=int, help="Total descriptor blocks")
    group.add_argument("--interactive", "-i", action="store_true", help="Interactive mode")
    group.add_argument("--sweep", "-s", action="store_true", help="Sweep packet counts to find breaking point")
    
    parser.add_argument("--desc-size", "-d", type=int, default=16, help="Descriptor size in bytes (default: 16)")
    parser.add_argument("--blocks-per-packet", "-z", type=int, default=3, help="Blocks per packet / Z value (default: 3)")
    parser.add_argument("--base-iova", "-b", type=lambda x: int(x, 0), default=0, help="Base IOVA address (default: 0)")
    parser.add_argument("--max-packets", type=int, default=100, help="Max packets for sweep mode (default: 100)")
    parser.add_argument("--show-all", "-a", action="store_true", help="Show all descriptors (verbose)")
    parser.add_argument("--padding", action="store_true", help="Use Linux-style multi-page with per-page padding")
    parser.add_argument("--page-size", type=int, default=DEFAULT_PAGE_SIZE, 
                       help=f"Page size in bytes (default: {DEFAULT_PAGE_SIZE}, macOS: {MAC_PAGE_SIZE})")
    
    args = parser.parse_args()
    
    page_size = args.page_size
    
    if args.interactive:
        interactive_mode()
        return
    
    if args.sweep:
        if args.padding:
            sweep_padded_mode(args.max_packets, args.blocks_per_packet, args.desc_size, page_size)
        else:
            sweep_mode(args.max_packets, args.blocks_per_packet, args.desc_size, page_size)
        return
    
    # Calculate total descriptors
    if args.num_packets:
        num_descriptors = args.num_packets * args.blocks_per_packet
    elif args.num_descriptors:
        num_descriptors = args.num_descriptors
    else:
        # Default: analyze the current problematic config
        print("No packet/descriptor count specified, using default: 84 packets")
        num_descriptors = 84 * 3
    
    if args.padding:
        # Linux-style padded layout analysis
        result = analyze_padded_layout(
            num_descriptors=num_descriptors,
            descriptor_size=args.desc_size,
            blocks_per_packet=args.blocks_per_packet,
            base_iova=args.base_iova,
            page_size=page_size,
        )
        print_padded_analysis(result, show_pages=True, page_size=page_size)
    else:
        # Standard contiguous layout analysis
        result = analyze_descriptor_ring(
            num_descriptors=num_descriptors,
            descriptor_size=args.desc_size,
            blocks_per_packet=args.blocks_per_packet,
            base_iova=args.base_iova,
            page_size=page_size,
        )
        print_analysis(result, show_all=args.show_all, page_size=page_size)
        print_recommendations(result, page_size=page_size)


if __name__ == "__main__":
    main()
