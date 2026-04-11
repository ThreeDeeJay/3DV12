#!/usr/bin/env python3
"""
scripts/gen_def.py – Generate src/d3d12.def with ordinals that exactly match
the real d3d12.dll on this machine.

Usage (called by CMakeLists.txt at configure time):
    python gen_def.py <real_dll_path> <output_def_path>

The script:
  1. Parses the PE export table of <real_dll_path> (pure Python, no dumpbin).
  2. For every named export found in the real DLL, emits a .def entry with the
     correct @ordinal if the function is in our KNOWN_EXPORTS set.
  3. For ordinals present in the real DLL that we do NOT implement, emits a
     forwarding stub name (UnknownExport_N) so the ordinal slot is filled and
     Windows can resolve the import.  The stub is a generated thin wrapper in
     dllmain.cpp that calls GetProcAddress by ordinal on the real DLL.
  4. Emits our full KNOWN_EXPORTS list even if not found in the real DLL
     (some may only exist in d3d12core.dll or on specific OS versions).
"""

import sys, os, struct

# ---------------------------------------------------------------------------
# Functions we implement in dllmain.cpp / the rest of the proxy.
# These MUST appear in the .def with @ordinal matching the real DLL.
# ---------------------------------------------------------------------------
KNOWN_EXPORTS = {
    "D3D12CoreCreateLayeredDevice",
    "D3D12CoreGetLayeredDeviceSize",
    "D3D12CoreRegisterDestroyedObject",
    "D3D12CreateDevice",
    "D3D12CreateRootSignatureDeserializer",
    "D3D12CreateVersionedRootSignatureDeserializer",
    "D3D12EnableExperimentalFeatures",
    "D3D12GetDebugInterface",
    "D3D12GetInterface",
    "D3D12PIXEventsReplaceBlock",
    "D3D12PIXGetThreadInfo",
    "D3D12PIXNotifyWakeFromFenceSignal",
    "D3D12PIXReportCounter",
    "D3D12SerializeRootSignature",
    "D3D12SerializeVersionedRootSignature",
    "OpenAdapter12",
    "SetAppCompatStringPointer",
}

# ---------------------------------------------------------------------------
# Minimal PE export-table parser (pure Python)
# ---------------------------------------------------------------------------

def _rva_to_offset(rva, sections):
    for (vaddr, vsize, raw_off, raw_size) in sections:
        if vaddr <= rva < vaddr + max(vsize, raw_size):
            return raw_off + (rva - vaddr)
    return None


def parse_exports(dll_path):
    """
    Returns a list of (ordinal: int, name: str | None) tuples sorted by ordinal.
    ordinal is the actual ordinal value (base-adjusted).
    name is None for ordinal-only (NONAME) exports.
    """
    with open(dll_path, "rb") as f:
        data = bytearray(f.read())

    if data[:2] != b"MZ":
        raise ValueError("Not a valid PE file (missing MZ magic)")

    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew : e_lfanew + 4] != b"PE\x00\x00":
        raise ValueError("Not a valid PE file (missing PE signature)")

    coff_off   = e_lfanew + 4
    machine    = struct.unpack_from("<H", data, coff_off)[0]
    num_sects  = struct.unpack_from("<H", data, coff_off + 2)[0]
    opt_size   = struct.unpack_from("<H", data, coff_off + 16)[0]

    is_pe32p   = (machine == 0x8664)  # x64
    opt_off    = coff_off + 20        # COFF header is 20 bytes

    # Export Directory RVA is data directory entry 0
    # Offset within Optional Header:
    #   PE32:   96  (0x60)
    #   PE32+: 112  (0x70)
    dd0_off = opt_off + (112 if is_pe32p else 96)
    export_dir_rva = struct.unpack_from("<I", data, dd0_off)[0]

    if export_dir_rva == 0:
        return []

    # Section table starts after the optional header
    sec_table_off = opt_off + opt_size
    sections = []
    for i in range(num_sects):
        s      = sec_table_off + i * 40
        vsize  = struct.unpack_from("<I", data, s +  8)[0]
        vaddr  = struct.unpack_from("<I", data, s + 12)[0]
        rsz    = struct.unpack_from("<I", data, s + 16)[0]
        roff   = struct.unpack_from("<I", data, s + 20)[0]
        sections.append((vaddr, vsize, roff, rsz))

    exp_off = _rva_to_offset(export_dir_rva, sections)
    if exp_off is None:
        return []

    ordinal_base  = struct.unpack_from("<I", data, exp_off + 16)[0]
    num_functions = struct.unpack_from("<I", data, exp_off + 20)[0]
    num_names     = struct.unpack_from("<I", data, exp_off + 24)[0]
    # addr_table_rva   at exp_off+28
    names_rva     = struct.unpack_from("<I", data, exp_off + 32)[0]
    ordinals_rva  = struct.unpack_from("<I", data, exp_off + 36)[0]

    names_off    = _rva_to_offset(names_rva, sections)
    ordinals_off = _rva_to_offset(ordinals_rva, sections)
    if names_off is None or ordinals_off is None:
        return []

    # name_ordinal[i] = (name_string, adjusted_ordinal)
    name_to_ordinal = {}
    ordinal_has_name = set()
    for i in range(num_names):
        name_rva = struct.unpack_from("<I", data, names_off + i * 4)[0]
        name_off = _rva_to_offset(name_rva, sections)
        if name_off is None:
            continue
        end  = data.index(0, name_off)
        name = data[name_off:end].decode("ascii", errors="replace")
        idx  = struct.unpack_from("<H", data, ordinals_off + i * 2)[0]
        actual_ordinal = idx + ordinal_base
        name_to_ordinal[name] = actual_ordinal
        ordinal_has_name.add(actual_ordinal)

    # All ordinals (including NONAME)
    all_ordinals = set(range(ordinal_base, ordinal_base + num_functions))
    noname_ordinals = all_ordinals - ordinal_has_name

    result = [(ord_, name) for name, ord_ in name_to_ordinal.items()]
    for ord_ in sorted(noname_ordinals):
        result.append((ord_, None))
    result.sort(key=lambda x: x[0])
    return result


# ---------------------------------------------------------------------------
# .def generator
# ---------------------------------------------------------------------------

def generate_def(exports, out_path, unknown_stubs_path):
    """
    Write .def file and a companion unknown_stubs.cpp for unrecognised exports.
    exports: list of (ordinal, name|None) from parse_exports()
    """
    known_by_name = {}     # name -> ordinal  for our implemented exports
    unknown_named = {}     # name -> ordinal  for exports we don't implement
    unknown_noname = []    # ordinals with no name

    for (ordinal, name) in exports:
        if name is None:
            unknown_noname.append(ordinal)
        elif name in KNOWN_EXPORTS:
            known_by_name[name] = ordinal
        else:
            unknown_named[name] = ordinal

    # Exports from the real DLL we haven't seen (may live in d3d12core.dll)
    missing_known = KNOWN_EXPORTS - set(known_by_name.keys())

    lines = [
        "; 3DV12 – generated by scripts/gen_def.py at configure time.",
        "; DO NOT EDIT – re-run CMake to regenerate.",
        "; Source DLL ordinals are preserved so games that import by ordinal work.",
        "",
        'LIBRARY "d3d12"',
        "",
        "EXPORTS",
    ]

    # Our known exports at their real ordinals
    for name in sorted(known_by_name):
        lines.append(f"    {name} @{known_by_name[name]}")

    # Known exports not found in real DLL (emit without explicit ordinal)
    if missing_known:
        lines.append("    ; Known exports not found in real DLL (d3d12core.dll / OS-version gap)")
        for name in sorted(missing_known):
            lines.append(f"    {name}")

    # Unknown named exports → forward stubs so ordinals are still valid
    if unknown_named:
        lines.append("    ; Forwarded stubs for real-DLL exports we do not intercept")
        for name in sorted(unknown_named, key=lambda n: unknown_named[n]):
            stub = f"UnknownExport_{unknown_named[name]}"
            lines.append(f"    {stub} @{unknown_named[name]}")

    # Ordinal-only (NONAME) exports
    if unknown_noname:
        lines.append("    ; NONAME ordinal-only exports")
        for ord_ in sorted(unknown_noname):
            lines.append(f"    UnknownOrdinal_{ord_} @{ord_} NONAME")

    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    with open(out_path, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"[gen_def] Wrote {out_path} ({len(exports)} exports from real DLL, "
          f"{len(known_by_name)} intercepted)")

    # Generate companion stub .cpp
    stub_names = (
        [(f"UnknownExport_{ord_}", ord_) for ord_ in sorted(unknown_named.values())] +
        [(f"UnknownOrdinal_{ord_}", ord_) for ord_ in sorted(unknown_noname)]
    )

    cpp_lines = [
        "// AUTO-GENERATED by scripts/gen_def.py – DO NOT EDIT.",
        "// Thin forwarding stubs for real d3d12.dll exports we do not intercept.",
        "// Each stub resolves the real function by ordinal at first call.",
        "#ifndef WIN32_LEAN_AND_MEAN",
        "#define WIN32_LEAN_AND_MEAN",
        "#endif",
        "#include <Windows.h>",
        "#include \"log.h\"",
        "",
        "extern HMODULE g_hRealD3D12;",
        "extern HMODULE g_hRealD3D12Core;",
        "",
    ]

    for (stub_name, ordinal) in stub_names:
        cpp_lines += [
            f"// Ordinal {ordinal}",
            f"static FARPROC g_pfn_{ordinal} = nullptr;",
            f"static FARPROC Resolve_{ordinal}() {{",
            f"    if (!g_pfn_{ordinal} && g_hRealD3D12)",
            f"        g_pfn_{ordinal} = GetProcAddress(g_hRealD3D12, (LPCSTR){ordinal});",
            f"    if (!g_pfn_{ordinal} && g_hRealD3D12Core)",
            f"        g_pfn_{ordinal} = GetProcAddress(g_hRealD3D12Core, (LPCSTR){ordinal});",
            f"    return g_pfn_{ordinal};",
            f"}}",
            f"extern \"C\" void {stub_name}() {{",
            f"    auto pfn = Resolve_{ordinal}();",
            f"    LOG_TRACE(\"Forwarding ordinal {ordinal}\");",
            f"    if (pfn) reinterpret_cast<void(*)()>(pfn)();",
            f"}}",
            "",
        ]

    os.makedirs(os.path.dirname(unknown_stubs_path) or ".", exist_ok=True)
    with open(unknown_stubs_path, "w") as f:
        f.write("\n".join(cpp_lines) + "\n")
    print(f"[gen_def] Wrote {unknown_stubs_path} ({len(stub_names)} stubs)")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    if len(sys.argv) < 4:
        print(f"Usage: {sys.argv[0]} <real_d3d12.dll> <output.def> <stubs.cpp>")
        sys.exit(1)

    dll_path        = sys.argv[1]
    out_def_path    = sys.argv[2]
    stubs_cpp_path  = sys.argv[3]

    if not os.path.exists(dll_path):
        print(f"[gen_def] ERROR: {dll_path} not found – using static fallback .def")
        sys.exit(2)

    try:
        exports = parse_exports(dll_path)
    except Exception as e:
        print(f"[gen_def] ERROR parsing {dll_path}: {e} – using static fallback .def")
        sys.exit(2)

    generate_def(exports, out_def_path, stubs_cpp_path)


if __name__ == "__main__":
    main()
