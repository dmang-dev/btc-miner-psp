#!/usr/bin/env python3
"""
embed_cacert.py — convert a PEM cacert bundle into a C source file
the EBOOT.PBP can embed without runtime file I/O.

Reads ../cacert.pem (Mozilla bundle via https://curl.se/ca/cacert.pem)
and writes ../source/cacert_data.c with the bundle as a quoted-string
literal, plus an entry in ../include/cacert.h with the symbol+length.

Re-run when the upstream bundle changes. Output is committed; the
build doesn't depend on this script running.

The generated string is mbedtls-friendly: PEM is fine for
mbedtls_x509_crt_parse, no DER conversion needed.
"""
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
PEM_PATH = HERE.parent / "cacert.pem"
OUT_C    = HERE.parent / "source" / "cacert_data.c"
OUT_H    = HERE.parent / "include" / "cacert.h"

def main():
    if not PEM_PATH.exists():
        sys.exit(f"input bundle not found: {PEM_PATH}\n"
                 "  fetch: curl -fsSL -o cacert.pem https://curl.se/ca/cacert.pem")

    pem = PEM_PATH.read_text(encoding="utf-8")
    pem_bytes = pem.encode("utf-8")
    cert_count = pem.count("-----BEGIN CERTIFICATE-----")

    # Emit one C string-literal per source line, 80 cols max.
    # Use the array initializer form so the compiler doesn't have to
    # parse a single multi-megabyte string token.
    lines = []
    lines.append("/* GENERATED — do not edit by hand.")
    lines.append(" * Source: cacert.pem (Mozilla CA bundle via https://curl.se/ca/)")
    lines.append(f" * Certs:  {cert_count}")
    lines.append(f" * Size:   {len(pem_bytes)} bytes")
    lines.append(" * Regen:  python tools/embed_cacert.py")
    lines.append(" */")
    lines.append("#include <stddef.h>")
    lines.append("")
    lines.append("const unsigned char btcm_ca_bundle_pem[] = {")
    BYTES_PER_LINE = 16
    for i in range(0, len(pem_bytes), BYTES_PER_LINE):
        chunk = pem_bytes[i:i + BYTES_PER_LINE]
        hexvals = ", ".join(f"0x{b:02x}" for b in chunk)
        lines.append(f"    {hexvals},")
    # Trailing NUL so mbedtls_x509_crt_parse() can scan it as a C string.
    lines.append("    0x00")
    lines.append("};")
    lines.append("")
    # +1 for the trailing NUL; mbedtls wants the buffer-len to include
    # the terminator for PEM input.
    lines.append(f"const size_t btcm_ca_bundle_pem_len = {len(pem_bytes) + 1};")
    lines.append("")

    OUT_C.parent.mkdir(parents=True, exist_ok=True)
    OUT_C.write_text("\n".join(lines), encoding="utf-8")

    OUT_H.parent.mkdir(parents=True, exist_ok=True)
    OUT_H.write_text(
        "/* GENERATED header for source/cacert_data.c. */\n"
        "#ifndef BTCM_CACERT_H\n"
        "#define BTCM_CACERT_H\n"
        "\n"
        "#include <stddef.h>\n"
        "\n"
        "/* Mozilla CA bundle as a single PEM blob, NUL-terminated.\n"
        " * Pass directly to mbedtls_x509_crt_parse(). */\n"
        "extern const unsigned char btcm_ca_bundle_pem[];\n"
        "extern const size_t        btcm_ca_bundle_pem_len;\n"
        "\n"
        "#endif\n",
        encoding="utf-8",
    )

    print(f"wrote {OUT_C.relative_to(HERE.parent)}: "
          f"{cert_count} certs, {len(pem_bytes)} bytes input, "
          f"~{OUT_C.stat().st_size // 1024} KB C source")
    print(f"wrote {OUT_H.relative_to(HERE.parent)}")

if __name__ == "__main__":
    main()
