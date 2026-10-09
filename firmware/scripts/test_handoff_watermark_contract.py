#!/usr/bin/env python3
"""Static contract for the M3000 boot-log handoff watermark."""
import re
from pathlib import Path

EXPECTED_HANDOFF = "M3000-HO-20261008-41"
project = Path(__file__).resolve().parents[1]
version_path = project / "version.txt"
app_path = project / "main/app_main.c"
main_cmake_path = project / "main/CMakeLists.txt"

main_cmake = main_cmake_path.read_text(encoding="utf-8")
main_requires = re.search(
    r"set\s*\(\s*dura_main_requires(?P<body>.*?)\)",
    main_cmake,
    re.DOTALL,
)
assert main_requires is not None, "main/CMakeLists.txt must define dura_main_requires"
assert re.search(r"^\s*esp_app_format\s*$", main_requires.group("body"), re.MULTILINE), (
    "main must directly require esp_app_format for esp_app_desc.h"
)
assert re.search(
    r"REQUIRES\s+\$\{dura_main_requires\}", main_cmake
), "main component registration must consume dura_main_requires"

assert version_path.is_file(), "missing top-level version.txt handoff watermark"
version_bytes = version_path.read_bytes()
assert version_bytes == (EXPECTED_HANDOFF + "\n").encode("ascii"), (
    "version.txt must contain exactly the expected one-line handoff ID with LF"
)
handoff = version_bytes.decode("ascii").rstrip("\n")
assert re.fullmatch(r"M3000-HO-\d{8}-\d{2}", handoff), "handoff ID format is invalid"

app_source = app_path.read_text(encoding="utf-8")
assert re.search(r'^#include\s+"esp_app_desc\.h"\s*$', app_source, re.MULTILINE), (
    "app source must include esp_app_desc.h"
)
desc_match = re.search(
    r"const\s+esp_app_desc_t\s*\*\s*(?P<desc>\w+)\s*=\s*esp_app_get_description\s*\(\s*\)\s*;",
    app_source,
)
assert desc_match is not None, "startup identity logging must fetch the embedded app description"
hash_buffer = re.search(r"char\s+(?P<hash>\w+)\s*\[\s*65\s*\]", app_source)
assert hash_buffer is not None, "ELF SHA-256 destination must be a 65-byte char buffer"
hash_name = hash_buffer.group("hash")
assert re.search(
    rf"esp_app_get_elf_sha256\s*\(\s*{re.escape(hash_name)}\s*,\s*sizeof\s*\(\s*{re.escape(hash_name)}\s*\)\s*\)",
    app_source,
), "startup identity logging must fetch the full ELF SHA-256 into the 65-byte buffer"

log_format = "HANDOFF_WATERMARK=%s ELF_SHA256=%s"
assert app_source.count(log_format) == 1, "exactly one explicit handoff watermark log format is required"
desc_name = desc_match.group("desc")
assert re.search(
    rf'ESP_LOGI\s*\(\s*TAG\s*,\s*"{re.escape(log_format)}"\s*,\s*{re.escape(desc_name)}\s*->\s*version\s*,\s*{re.escape(hash_name)}\s*\)',
    app_source,
), "handoff log must use the embedded app descriptor version and full ELF hash"
assert EXPECTED_HANDOFF not in app_source, "handoff ID must not be duplicated as a C string"
assert "DURA_METER_FIRMWARE_VERSION" in app_source, "existing meter firmware identity log was removed"

print("HANDOFF_WATERMARK_CONTRACT_PASS")
