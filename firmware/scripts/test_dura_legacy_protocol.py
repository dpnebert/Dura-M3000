#!/usr/bin/env python3
"""Build and run the Dura legacy protocol parser/serializer on the host."""
from pathlib import Path
import subprocess

project = Path(__file__).resolve().parents[1]
out = project / "build-host-tests" / "test_dura_legacy_protocol"
out.parent.mkdir(exist_ok=True)
cmd = [
    "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
    f"-I{project / 'tests/host_include'}",
    f"-I{project / 'components/dura_legacy_protocol/include'}",
    str(project / "components/dura_legacy_protocol/dura_legacy_protocol.c"),
    str(project / "tests/test_dura_legacy_protocol.c"),
    "-o", str(out),
]
print("+ " + " ".join(cmd), flush=True)
subprocess.run(cmd, check=True)
subprocess.run([str(out)], check=True)
