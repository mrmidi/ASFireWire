#!/usr/bin/env python3
"""Specification PDF Section Extractor

Supports extracting sections from:
    * 1394 Open Host Controller Interface (OHCI) 1.1 specification
    * IEEE 1212-2001 Control and Status Register (CSR) Architecture specification

It parses (or falls back to manual) table of contents data, handles roman numeral
front matter pages, calculates page ranges, and writes per-section PDF files plus
a README with navigation links.

Usage: python extract_ohci_sections.py <pdf_filename> [--mode ohci|1212|1394]
Modes:
    ohci  (default) Extract OHCI 1.1 spec sections to docs/ohci
    1212  Extract IEEE 1212-2001 CSR Architecture spec sections to docs/csr
    1394  Extract IEEE 1394-2008 High-Performance Serial Bus spec sections to docs/1394
                 (arabic page 1 in TOC corresponds to PDF page 7; pages 1-6 are roman front matter and ignored)
"""

import sys
import os
import re
import argparse
from pathlib import Path
from typing import List, Tuple, Optional, Dict

try:
    import PyPDF2
except ImportError:
    print("Error: PyPDF2 not found. Install with: pip install PyPDF2")
    sys.exit(1)


class TOCSection:
    """Represents a section from the table of contents."""

    def __init__(self, title: str, page: str, level: int = 0):
        self.title = self._clean_title(title.strip())
        self.page_str = page.strip()
        self.level = level
        self.page_num = self._parse_page_number(page)
        self.filename = self._generate_filename()

    def _clean_title(self, title: str) -> str:
        """Clean up section title by removing dotted leaders and extra whitespace."""
        clean_title = re.sub(r'\.{3,}', '', title)
        clean_title = re.sub(r'\s*-\s*$', '', clean_title)
        clean_title = re.sub(r'\s+', ' ', clean_title).strip()
        return clean_title

    def _parse_page_number(self, page_str: str) -> Optional[int]:
        page_str = page_str.strip()
        roman_map = {
            'i': 1, 'ii': 2, 'iii': 3, 'iv': 4, 'v': 5, 'vi': 6, 'vii': 7,
            'viii': 8, 'ix': 9, 'x': 10, 'xi': 11, 'xii': 12, 'xiii': 13,
            'xiv': 14, 'xv': 15, 'xvi': 16, 'xvii': 17, 'xviii': 18
        }
        if page_str.lower() in roman_map:
            return roman_map[page_str.lower()]
        if page_str.isdigit():
            return int(page_str)
        return None

    def _generate_filename(self) -> str:
        clean_title = re.sub(r'[^\w\s-]', '', self.title.lower())
        clean_title = re.sub(r'\s+', '-', clean_title.strip())
        clean_title = re.sub(r'-+', '-', clean_title)
        if self.level == 0:
            prefix = f"{self.page_num:02d}-" if self.page_num else "00-"
        else:
            prefix = ""
        return f"{prefix}{clean_title}.pdf"

    def __repr__(self):
        return f"TOCSection('{self.title}', page={self.page_str}, level={self.level})"


class SpecSectionExtractor:
    """Extracts sections from supported specification PDFs (OHCI, 1212)."""

    MODES: Dict[str, Dict[str, object]] = {
        # OHCI 1.1 spec configuration
        "ohci": {
            "output_dir": Path("docs/ohci"),
            # Arabic page 1 starts at PDF page index 18 (page 19 in 1-based terms)
            "arabic_start_pdf_index": 18,
            # Number of roman numeral pages preceding Arabic '1' (ii..xvii) -> 17
            "roman_page_count": 17,
            "manual_toc_func": "_get_manual_toc_ohci",
        },
        # IEEE 1212-2001 CSR Architecture specification configuration
        "1212": {
            "output_dir": Path("docs/csr"),
            # User indicated arabic page 1 (TOC) == PDF page 7 (index 6)
            "arabic_start_pdf_index": 6,
            # Preceding roman pages count (pages 1-6 are prelim / roman / front matter ignored)
            "roman_page_count": 6,
            "manual_toc_func": "_get_manual_toc_1212",
        },
        # IEEE 1394-2008 High-Performance Serial Bus specification configuration
        # Uses dynamic detection of arabic start and TOC range; placeholders set to None
        "1394": {
            "output_dir": Path("docs/1394"),
            "arabic_start_pdf_index": None,  # detected at runtime
            "roman_page_count": None,        # derived from detected arabic start
            "manual_toc_func": "_get_manual_toc_1394",
        },
    }

    def __init__(self, pdf_path: str, mode: str = "ohci"):
        if mode not in self.MODES:
            raise ValueError(f"Unsupported mode '{mode}'. Supported: {list(self.MODES.keys())}")
        self.mode = mode
        self.config = self.MODES[mode]
        self.pdf_path = Path(pdf_path)
        self.output_dir: Path = self.config["output_dir"]  # type: ignore
        self.sections: List[TOCSection] = []
        self.pdf_reader = None
        self.pdf_file = None
        self.roman_page_count: int = self.config["roman_page_count"]  # type: ignore
        
    def load_pdf(self) -> bool:
        """Load the PDF file."""
        try:
            self.pdf_file = open(self.pdf_path, 'rb')
            self.pdf_reader = PyPDF2.PdfReader(self.pdf_file)
            
            # Try to extract bookmarks/outline for better navigation
            self.bookmarks = self.pdf_reader.outline if hasattr(self.pdf_reader, 'outline') else []
            
            # Debug: Print some pages to understand the structure
            print("Analyzing PDF structure...")
            self._analyze_pdf_structure()

            # Dynamic layout detection for 1394 specification
            if self.mode == "1394":
                self._detect_1394_layout()
            
            return True
        except Exception as e:
            print(f"Error loading PDF: {e}")
            if self.pdf_file:
                self.pdf_file.close()
            return False

    def _detect_1394_layout(self):
        """Detect TOC range, clause 1 start, and derive roman page count for 1394 spec.

        Heuristics:
          * TOC starts at first page containing 'Table of Contents'
          * TOC continues while many dotted leader lines or Annex/Clause patterns persist
          * Clause 1 start: first page after TOC with heading starting with '1' and 'Scope'/'Overview'
        Sets:
          self.toc_start_index, self.toc_end_index, self.arabic_start_pdf_index, self.roman_page_count
        """
        if not self.pdf_reader:
            return
        import re
        pages = self.pdf_reader.pages
        total = len(pages)
        max_scan = min(160, total)
        toc_start = None
        toc_end = None
        clause1_index = None

        def toc_like(text: str) -> bool:
            lines = [l for l in (text or '').splitlines() if l.strip()]
            dotted = sum(1 for l in lines if re.search(r'\.{3,}\s*\d{1,4}$', l))
            annex = sum(1 for l in lines if re.search(r'^Annex\s+[A-Z]', l))
            clause = sum(1 for l in lines if re.search(r'^\d+\.', l))
            return dotted > 4 or (clause > 6 and dotted > 2) or annex > 0

        # Locate TOC start
        for i in range(max_scan):
            try:
                txt = pages[i].extract_text() or ''
            except Exception:
                continue
            if 'Table of Contents' in txt:
                toc_start = i
                break
        if toc_start is not None:
            # Extend to end
            toc_end = toc_start
            for j in range(toc_start, min(toc_start + 80, total)):
                try:
                    t = pages[j].extract_text() or ''
                except Exception:
                    break
                if toc_like(t):
                    toc_end = j
                    continue
                else:
                    break
        # Find Clause 1 start after TOC
        search_from = (toc_end + 1) if toc_end is not None else 0
        clause_pattern = re.compile(r'\b1(\.|\s)(?:\s*)(Scope|Overview)', re.IGNORECASE)
        for k in range(search_from, min(search_from + 60, total)):
            try:
                text = pages[k].extract_text() or ''
            except Exception:
                continue
            head = " ".join(text.splitlines()[:6])
            if clause_pattern.search(head):
                clause1_index = k
                break
        # Assign detected values
        if clause1_index is not None:
            self.config['arabic_start_pdf_index'] = clause1_index  # type: ignore
            self.arabic_start_pdf_index = clause1_index
            self.config['roman_page_count'] = clause1_index  # type: ignore
            self.roman_page_count = clause1_index
        else:
            # Fallback: assume clause 1 immediately after TOC or page 0
            fallback_index = (toc_end + 1) if toc_end is not None else 0
            self.config['arabic_start_pdf_index'] = fallback_index  # type: ignore
            self.arabic_start_pdf_index = fallback_index
            self.config['roman_page_count'] = fallback_index  # type: ignore
            self.roman_page_count = fallback_index
        self.toc_start_index = toc_start
        self.toc_end_index = toc_end
        print("1394 layout detection:")
        print(f"  TOC pages: {toc_start+1}-{toc_end+1}" if toc_start is not None and toc_end is not None else "  TOC pages: not found")
        print(f"  Clause 1 start PDF page: {self.arabic_start_pdf_index+1}")
        print(f"  Roman front-matter pages count: {self.roman_page_count}")
    
    def _analyze_pdf_structure(self):
        """Analyze PDF structure to understand page numbering."""
        print("PDF Structure Analysis:")
        print(f"Total pages: {len(self.pdf_reader.pages)}")
        
        # Check a few key pages to understand the numbering
        key_pages = [0, 1, 2, 16, 17, 18, 19, 20]  # Sample pages
        
        for page_idx in key_pages:
            if page_idx < len(self.pdf_reader.pages):
                try:
                    page = self.pdf_reader.pages[page_idx]
                    text = page.extract_text()[:200]  # First 200 chars
                    # Look for page numbers in the text
                    page_marker = self._extract_page_number_from_text(text)
                    print(f"  PDF page {page_idx+1}: {page_marker}")
                except Exception as e:
                    print(f"  PDF page {page_idx+1}: Could not extract text - {e}")
        
        # Check for bookmarks
        if self.bookmarks:
            print(f"Found {len(self.bookmarks)} bookmarks")
            for i, bookmark in enumerate(self.bookmarks[:5]):  # Show first 5
                if hasattr(bookmark, 'title'):
                    print(f"  Bookmark: {bookmark.title}")
        else:
            print("No bookmarks found in PDF")
    
    def _extract_page_number_from_text(self, text: str) -> str:
        """Extract page number indicators from page text."""
        # Look for common page number patterns
        import re
        
        # Look for roman numerals at start/end
        roman_pattern = r'\b([ivxlc]+)\b'
        roman_matches = re.findall(roman_pattern, text.lower())
        
        # Look for "Page N" patterns
        page_pattern = r'Page\s+([ivxlc\d]+)'
        page_matches = re.findall(page_pattern, text, re.IGNORECASE)
        
        # Look for numbered patterns
        number_pattern = r'\b(\d+)\b'
        number_matches = re.findall(number_pattern, text)
        
        indicators = []
        if roman_matches:
            indicators.append(f"roman:{roman_matches[0]}")
        if page_matches:
            indicators.append(f"page:{page_matches[0]}")
        if number_matches:
            indicators.append(f"numbers:{number_matches[:3]}")
            
        return " | ".join(indicators) if indicators else "no indicators"
    
    def close_pdf(self):
        """Close the PDF file."""
        if self.pdf_file:
            self.pdf_file.close()
    
    def extract_toc_from_content(self) -> List[TOCSection]:
        """Extract table of contents by parsing TOC pages or falling back to manual lists per mode."""
        sections: List[TOCSection] = []
        if self.mode == "1394":
            sections = self._extract_toc_1394()
            if not sections:
                manual_method_name = self.config["manual_toc_func"]  # type: ignore
                manual_method = getattr(self, manual_method_name)
                sections = manual_method()
            return sections
        if self.mode == "ohci":
            toc_start_page = 4
            toc_end_page = min(12, len(self.pdf_reader.pages))
        else:  # 1212: TOC appears earlier; heuristic pages 5-8 (arabic numbering starts later)
            toc_start_page = 4  # still page v like pattern
            toc_end_page = min(10, len(self.pdf_reader.pages))
        print(f"Scanning pages {toc_start_page+1}-{toc_end_page} for TOC content (mode {self.mode})...")
        for page_num in range(toc_start_page, toc_end_page):
            if page_num >= len(self.pdf_reader.pages):
                break
            try:
                page = self.pdf_reader.pages[page_num]
                text = page.extract_text()
                sections.extend(self._parse_toc_page(text))
            except Exception as e:
                print(f"Warning: Could not extract text from page {page_num+1}: {e}")
        if not sections:
            manual_method_name = self.config["manual_toc_func"]  # type: ignore
            manual_method = getattr(self, manual_method_name)
            sections = manual_method()
        return sections

    def _extract_toc_1394(self) -> List[TOCSection]:
        """Extract TOC for IEEE 1394-2008 using bookmarks first, then heuristic text parsing."""
        results: List[TOCSection] = []
        if not self.pdf_reader:
            return results
        # 1. Bookmark (outline) based extraction (top-level only)
        outline = []
        try:
            outline = self.pdf_reader.outline if hasattr(self.pdf_reader, 'outline') else []
        except Exception:
            outline = []
        def walk(items, level=0):
            for it in items:
                if isinstance(it, list):
                    walk(it, level+1)
                    continue
                title = getattr(it, 'title', '').strip()
                if not title:
                    continue
                # Only keep modest depth (0/1)
                if level > 1:
                    continue
                page_index = None
                try:
                    page_index = self.pdf_reader.get_destination_page_number(it)
                except Exception:
                    pass
                page_display = ''
                if page_index is not None and hasattr(self, 'arabic_start_pdf_index') and self.arabic_start_pdf_index is not None:
                    rel = (page_index - self.arabic_start_pdf_index) + 1
                    if rel >= 1:
                        page_display = str(rel)
                results.append(TOCSection(title, page_display or ''))
        if outline:
            walk(outline)
        # Basic sanity: need enough entries
        if len([r for r in results if r.page_str]) >= 10:
            print(f"1394: Extracted {len(results)} entries from bookmarks")
            return results
        # 2. Heuristic parse of TOC pages if we detected them
        import re
        toc_pages = []
        if hasattr(self, 'toc_start_index') and self.toc_start_index is not None and self.toc_end_index is not None:
            toc_pages = list(range(self.toc_start_index, self.toc_end_index + 1))
        else:
            # fallback: early pages
            toc_pages = list(range(0, min(40, len(self.pdf_reader.pages))))
        line_pattern = re.compile(r'^(?P<title>.{3,}?)(?:\s+\.{2,}\s+|\s+)(?P<page>\d{1,4})$')
        seen = set()
        for p in toc_pages:
            try:
                text = self.pdf_reader.pages[p].extract_text() or ''
            except Exception:
                continue
            for raw in text.splitlines():
                line = raw.strip()
                if len(line) < 4 or len(line) > 160:
                    continue
                m = line_pattern.match(line)
                if not m:
                    continue
                title = re.sub(r'\.{2,}', ' ', m.group('title')).strip()
                page = m.group('page')
                # Filter noise
                low = title.lower()
                if low in seen:
                    continue
                if re.fullmatch(r'\d+', title):
                    continue
                seen.add(low)
                results.append(TOCSection(title, page))
        # Return what we have (could be empty; caller will fallback to manual)
        if results:
            print(f"1394: Heuristic TOC produced {len(results)} entries")
        return results
    
    def _parse_toc_page(self, text: str) -> List[TOCSection]:
        """Parse TOC entries from a page of text."""
        sections = []
        lines = text.split('\n')
        
        for line in lines:
            line = line.strip()
            if not line:
                continue
            
            # Pattern matching for TOC entries
            # Look for patterns like:
            # "1. Introduction .......1"
            # "1.1 Related documents.......1" 
            # "PREFACE .......iii"
            # "Annex A. PCI Interface (optional) .......159"
            
            # Clean up dotted leaders first
            clean_line = self._clean_toc_line(line)
            
            # Main section pattern (numbered sections)
            main_pattern = r'^(\d+\.?\s+.+?)\s+-\s+(\d+)$'
            match = re.match(main_pattern, clean_line)
            if match:
                title = match.group(1).strip()
                page = match.group(2).strip()
                sections.append(TOCSection(title, page, 0))
                continue
            
            # Subsection pattern (numbered subsections)
            sub_pattern = r'^(\d+\.\d+\.?\s+.+?)\s+-\s+(\d+)$'
            match = re.match(sub_pattern, clean_line)
            if match:
                title = match.group(1).strip()
                page = match.group(2).strip()
                sections.append(TOCSection(title, page, 1))
                continue
                
            # Preface sections (roman numerals)
            preface_pattern = r'^([A-Z][A-Za-z\s]+(?:\(.*\))?)\s+-\s+([ivx]+)$'
            match = re.match(preface_pattern, clean_line)
            if match:
                title = match.group(1).strip()
                page = match.group(2).strip()
                sections.append(TOCSection(title, page, 0))
                continue
            
            # Annex pattern
            annex_pattern = r'^(Annex\s+[A-Z]\.?\s+.+?)\s+-\s+(\d+)$'
            match = re.match(annex_pattern, clean_line)
            if match:
                title = match.group(1).strip()
                page = match.group(2).strip()
                sections.append(TOCSection(title, page, 0))
                continue
        
        return sections
    
    def _clean_toc_line(self, line: str) -> str:
        """Clean up TOC line by replacing dotted leaders with clean separators."""
        # Replace sequences of 3 or more dots with " - "
        clean_line = re.sub(r'\.{3,}', ' - ', line)
        
        # Clean up extra whitespace
        clean_line = re.sub(r'\s+', ' ', clean_line).strip()
        
        return clean_line
    
    def _get_manual_toc_ohci(self) -> List[TOCSection]:
        """Manual TOC data for OHCI if parsing fails."""
        print("Using manual TOC for OHCI as fallback...")
        toc_data = [
            ("PREFACE", "iii", 0),
            ("Table of Contents", "v", 0),
            ("List of Figures", "xiii", 0),
            ("List of Tables", "xvii", 0),
            ("1. Introduction", "1", 0),
            ("2. Conventions - Notation and Terms", "11", 0),
            ("3. Common DMA Controller Features", "17", 0),
            ("4. Register addressing", "29", 0),
            ("5. 1394 Open HCI Registers", "35", 0),
            ("6. Interrupts", "61", 0),
            ("7. Asynchronous Transmit DMA", "69", 0),
            ("8. Asynchronous Receive DMA", "95", 0),
            ("9. Isochronous Transmit DMA", "111", 0),
            ("10. Isochronous Receive DMA", "129", 0),
            ("11. Self ID Receive", "147", 0),
            ("12. Physical Requests", "151", 0),
            ("13. Host Bus Errors", "153", 0),
            ("Annex A. PCI Interface (optional)", "159", 0),
            ("Annex B. Summary of Register Reset Values (Informative)", "171", 0),
            ("Annex C. Summary of Bus Reset Behavior (Informative)", "177", 0),
            ("Annex D. IT DMA Supplement (Informative)", "179", 0),
            ("Annex E. Sample IT DMA Controller Implementation (Informative)", "185", 0),
            ("Annex F. Extended Config ROM Entries", "191", 0),
        ]
        return [TOCSection(t, p, l) for t, p, l in toc_data]

    def _get_manual_toc_1212(self) -> List[TOCSection]:
        """Manual TOC for IEEE 1212-2001 (placeholder; expand with actual sections)."""
        print("Using manual TOC for 1212 as fallback (initial subset)...")
        # NOTE: Provide initial key chapters; user can extend list with more granular subsections.
        toc_data = [
            ("1. Overview", "1", 0),
            ("1.1 Scope", "1", 1),
            ("1.2 Purpose", "2", 1),
            ("2. References", "2", 0),
            ("3. Definitions and notation", "3", 0),
            ("3.1 Definitions", "3", 1),
            ("3.2 Notation", "6", 1),
            ("4. Architectural framework", "7", 0),
            ("4.1 Modules, nodes, and units", "7", 1),
            ("4.2 Addressing", "8", 1),
            ("5. Transaction set", "10", 0),
            ("5.1 Read and write transactions", "10", 1),
            ("5.2 Lock transactions", "11", 1),
            ("5.3 Bus-dependent transactions", "13", 1),
            ("5.4 Split transactions", "13", 1),
            ("5.5 Completion status", "14", 1),
            ("6. CSR definitions", "14", 0),
            ("6.1 STATE_CLEAR / STATE_SET registers", "17", 1),
            ("6.2 NODE_IDS register", "17", 1),
            ("6.3 RESET_START register", "18", 1),
            ("6.4 SPLIT_TIMEOUT register", "19", 1),
            ("6.5 MESSAGE_REQUEST / MESSAGE_RESPONSE registers", "20", 1),
            ("7. Configuration ROM", "21", 0),
            ("7.1 IEEE Registration Authority", "22", 1),
            ("7.2 ROM formats", "22", 1),
            ("7.3 CRC calculation", "27", 1),
            ("7.4 Minimal ASCII", "28", 1),
            ("7.5 Data structures", "29", 1),
            ("7.6 Required and optional usage", "44", 1),
            ("7.7 Directory entries", "49", 1),
            ("Annex A (informative) Configuration ROM examples", "60", 0),
            ("Annex B (informative) Keyword examples", "66", 0),
            ("Annex C (informative) Bibliography", "67", 0),
        ]
        return [TOCSection(t, p, l) for t, p, l in toc_data]

    def _get_manual_toc_1394(self) -> List[TOCSection]:
        """Manual fallback TOC (sparse placeholder) for IEEE 1394-2008."""
        print("Using sparse manual TOC for 1394 (placeholder; refine with accurate pages as needed)...")
        toc_data = [
            ("Clause 1 Scope", "1", 0),
            ("Clause 2 Normative references", "3", 0),
            ("Clause 3 Terms, definitions, and abbreviations", "5", 0),
            ("Clause 4 Symbols", "7", 0),
            ("Clause 5 Architectural overview", "9", 0),
            ("Annex A (informative) Examples", "400", 0),
            ("Annex S (informative) Bibliography", "1400", 0),
        ]
        return [TOCSection(t, p, l) for t, p, l in toc_data]
    
    def _is_roman_numeral(self, page_str: str) -> bool:
        """Check if a page string is a roman numeral."""
        roman_numerals = {'i', 'ii', 'iii', 'iv', 'v', 'vi', 'vii', 'viii', 'ix', 'x', 
                         'xi', 'xii', 'xiii', 'xiv', 'xv', 'xvi', 'xvii', 'xviii', 'xix', 'xx'}
        return page_str.lower() in roman_numerals
    
    def _calculate_pdf_page_index(self, section: TOCSection) -> int:
        """Calculate the actual PDF page index (0-based) for a section based on mode config."""
        arabic_base = self.config["arabic_start_pdf_index"]  # type: ignore
        if arabic_base is None:
            arabic_base = 0
        if self._is_roman_numeral(section.page_str):
            return section.page_num - 1
        # Arabic pages: 1212 observed needing +1; 1394 uses dynamic base exactly
        adjust = 1 if self.mode == "1212" else 0
        if self.mode == "1394":
            adjust = 0
        return arabic_base + section.page_num - 1 + adjust
    
    def calculate_pdf_page_ranges(self) -> List[Tuple[TOCSection, int, int]]:
        """
        Calculate actual PDF page ranges for each section.
        Returns list of (section, start_page, end_page) tuples.
        """
        ranges = []
        valid_sections = [s for s in self.sections if s.page_num is not None]
        
        if not valid_sections:
            print("Warning: No valid sections found with page numbers")
            return ranges
        
        print(f"Calculating page ranges for {len(valid_sections)} sections...")
        
        for i, section in enumerate(valid_sections):
            start_page = self._calculate_pdf_page_index(section)
            
            # Find end page by looking at next section
            if i + 1 < len(valid_sections):
                next_section = valid_sections[i + 1]
                next_start_page = self._calculate_pdf_page_index(next_section)
                
                # End current section on the page before next section starts
                # But handle overlapping sections (same page) 
                if next_start_page == start_page:
                    # Same page - section ends on same page it starts
                    end_page = start_page
                else:
                    # Normal case - end on page before next section
                    end_page = next_start_page - 1
            else:
                # Last section - extend to end of document
                end_page = len(self.pdf_reader.pages) - 1
            
            # Validate page range
            if start_page >= len(self.pdf_reader.pages):
                print(f"Warning: Section '{section.title}' starts beyond PDF end (page {start_page+1})")
                continue
            
            if end_page >= len(self.pdf_reader.pages):
                print(f"Warning: Section '{section.title}' extends beyond PDF, truncating to last page")
                end_page = len(self.pdf_reader.pages) - 1
            
            if start_page > end_page:
                print(f"Warning: Invalid range for '{section.title}': {start_page+1}-{end_page+1}")
                end_page = start_page  # Set to single page
            
            ranges.append((section, start_page, end_page))
            print(f"  {section.title}: pages {start_page+1}-{end_page+1} (PDF pages)")
        
        return ranges
    
    def extract_section_pdf(self, section: TOCSection, start_page: int, end_page: int) -> bool:
        """Extract a section to its own PDF file."""
        try:
            pdf_writer = PyPDF2.PdfWriter()
            
            # Add pages to writer
            for page_num in range(start_page, end_page + 1):
                if page_num < len(self.pdf_reader.pages):
                    pdf_writer.add_page(self.pdf_reader.pages[page_num])
            
            # Write to file
            output_path = self.output_dir / section.filename
            with open(output_path, 'wb') as output_file:
                pdf_writer.write(output_file)
                
            print(f"Extracted: {section.title} -> {section.filename} (pages {start_page+1}-{end_page+1})")
            return True
            
        except Exception as e:
            print(f"Error extracting {section.title}: {e}")
            return False
    
    def create_output_directory(self):
        """Create the output directory structure."""
        self.output_dir.mkdir(parents=True, exist_ok=True)
        print(f"Created output directory: {self.output_dir}")
    
    def generate_readme(self, extracted_sections: List[Tuple[TOCSection, int, int]]):
        """Generate README.md with navigation links, customized per spec mode."""
        readme_path = self.output_dir / "README.md"
        if self.mode == "ohci":
            title = "1394 Open Host Controller Interface Specification - Extracted Sections"
            intro = "This directory contains sections extracted from the OHCI 1.1 specification PDF."
            source_lines = [
                "Source: 1394 Open Host Controller Interface Specification / Release 1.1",
                "Copyright © 1996-2000 All rights reserved.",
                "Printed 1/10/00",
            ]
        elif self.mode == "1212":
            title = "IEEE 1212-2001 CSR Architecture Specification - Extracted Sections"
            intro = "This directory contains sections extracted from the IEEE 1212-2001 Control and Status Registers (CSR) Architecture specification PDF."
            source_lines = [
                "Source: IEEE Std 1212-2001, IEEE Standard for a Control and Status Register (CSR) Architecture for Microcomputer Buses",
                "© IEEE (fair use for study/reference).",
            ]
        else:  # 1394
            title = "IEEE 1394-2008 High-Performance Serial Bus - Extracted Sections"
            intro = "This directory contains sections extracted from the IEEE 1394-2008 High-Performance Serial Bus specification PDF."
            source_lines = [
                "Source: IEEE Std 1394-2008, IEEE Standard for a High-Performance Serial Bus",
                "© IEEE (fair use for study/reference).",
            ]
        with open(readme_path, 'w') as f:
            f.write(f"# {title}\n\n")
            f.write(intro + "\n\n")
            f.write("## Navigation\n\n")
            for section, start_page, end_page in extracted_sections:
                indent = "  " * section.level
                page_range = f"(PDF pages {start_page+1}-{end_page+1})"
                f.write(f"{indent}- [{section.title}](./{section.filename}) {page_range}\n")
            f.write("\n## Original Document\n\n")
            for line in source_lines:
                f.write(line + "\n")
            f.write("\n## Usage\n\n")
            f.write("Each section is provided as a separate PDF file for easy reference during development.\n")
            if self.mode == "1212":
                f.write("Arabic page numbers from the TOC map to PDF pages with an observed +1 adjustment (arabic page 1 -> PDF page 8). Front matter roman pages are ignored.\n")
            if self.mode == "1394":
                f.write("Page ranges derived from detected Clause 1 start and bookmark/TOC parsing. TOC pages and roman front matter are excluded from numbering.\n")
            f.write("The sections maintain the original page numbering and formatting from the source document.\n")
        print(f"Generated README: {readme_path}")
    
    def extract_all_sections(self):
        """Main extraction process."""
        print(f"Starting specification section extraction (mode={self.mode})...")
        # Load PDF
        if not self.load_pdf():
            return False
        print(f"Loaded PDF: {self.pdf_path} ({len(self.pdf_reader.pages)} pages) mode={self.mode}")
        # Create output directory
        self.create_output_directory()
        # Extract TOC
        self.sections = self.extract_toc_from_content()
        print(f"Found {len(self.sections)} sections in TOC")
        # Calculate page ranges
        page_ranges = self.calculate_pdf_page_ranges()
        print(f"Calculated {len(page_ranges)} page ranges")
        # Extract each section
        successful_extractions = []
        for section, start_page, end_page in page_ranges:
            if self.extract_section_pdf(section, start_page, end_page):
                successful_extractions.append((section, start_page, end_page))
        # Generate README
        self.generate_readme(successful_extractions)
        # Close PDF file
        self.close_pdf()
        print(f"\nExtraction complete! Extracted {len(successful_extractions)} sections to {self.output_dir}")
        return True


def main():
    """Main entry point."""
    parser = argparse.ArgumentParser(
        description="Extract per-section PDFs from supported specs (OHCI 1.1 or IEEE 1212-2001) using TOC parsing or manual fallback"
    )
    parser.add_argument("pdf_file", help="Path to the specification PDF file")
    parser.add_argument("--mode", choices=["ohci", "1212", "1394"], default="ohci", help="Specification mode: 'ohci' (OHCI 1.1), '1212' (IEEE 1212-2001 CSR Architecture), or '1394' (IEEE 1394-2008 High-Performance Serial Bus)")
    
    args = parser.parse_args()
    
    if not os.path.exists(args.pdf_file):
        print(f"Error: PDF file '{args.pdf_file}' not found")
        sys.exit(1)
    
    extractor = SpecSectionExtractor(args.pdf_file, mode=args.mode)
    if extractor.extract_all_sections():
        print("\nSection extraction completed successfully!")
    else:
        print("\nSection extraction failed!")
        sys.exit(1)


if __name__ == "__main__":
    main()