#!/usr/bin/env python3
"""Focused host/static contract for M3000 cross-interlocked dual-reed counting."""
import re
import subprocess
import tempfile
from pathlib import Path

project = Path(__file__).resolve().parents[1]
board_path = project / "components/dura_board/dura_board.c"
board_source = board_path.read_text(encoding="utf-8")


def extract_braced(text: str, start: int) -> str:
    brace = text.find("{", start)
    assert brace >= 0, "opening brace not found"
    depth = 0
    i = brace
    while i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start : i + 1]
        i += 1
    raise AssertionError("unterminated braced block")


def extract_function(text: str, name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^;{{}}]*\)\s*\{{", text)
    assert match is not None, f"missing function {name}"
    return extract_braced(text, match.start())


# Integration contract: queued events identify the GPIO that interrupted while
# retaining the two-level snapshot; GPIO21/A and GPIO2/B use distinct markers.
event_type = re.search(r"typedef\s+struct\s*\{(?P<body>.*?)\}\s*dura_flow_event_t\s*;", board_source, re.DOTALL)
assert event_type is not None, "missing dura_flow_event_t"
assert re.search(r"\buint8_t\s+source\s*;", event_type.group("body")), (
    "queued flow event lacks interrupt-source identity"
)
flow_isr = extract_function(board_source, "flow_isr")
assert ".source = (uint8_t)(uintptr_t)arg" in flow_isr
assert "gpio_get_level(DURA_GPIO_FLOW_A)" in flow_isr
assert "gpio_get_level(DURA_GPIO_FLOW_B)" in flow_isr
assert re.search(
    r"gpio_isr_handler_add\(DURA_GPIO_FLOW_A,\s*flow_isr,\s*\(void \*\)\(uintptr_t\)1u\)",
    board_source,
)
assert re.search(
    r"gpio_isr_handler_add\(DURA_GPIO_FLOW_B,\s*flow_isr,\s*\(void \*\)\(uintptr_t\)2u\)",
    board_source,
)
assert "GPIO_INTR_ANYEDGE" in board_source
assert "xQueueSendFromISR" in flow_isr
assert "s_reed_armed" not in board_source, "both-open rearming gate remains"

# Compile and execute the actual production qualifier extracted from dura_board.c.
qualifier = extract_function(board_source, "flow_event_counts")
harness = f'''\
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define DURA_FLOW_ACTIVE_LEVEL 0

typedef struct {{
    uint8_t source;
    uint8_t levels;
}} dura_flow_event_t;
static uint8_t s_last_valid_reed;
static bool {qualifier}

static unsigned feed(uint8_t source, uint8_t levels) {{
    const dura_flow_event_t event = {{.source = source, .levels = levels}};
    return flow_event_counts(&event) ? 1u : 0u;
}}
static void expect(const char *name, unsigned actual, unsigned expected) {{
    if (actual != expected) {{
        fprintf(stderr, "%s: got %u expected %u\\n", name, actual, expected);
        exit(1);
    }}
}}
int main(void) {{
    /* Active-low levels: 3=open/open, 2=A active, 1=B active, 0=both active. */
    s_last_valid_reed = 0;
    expect("open to A active", feed(1, 2), 1);
    expect("A release", feed(1, 3), 0);
    expect("A active again before B", feed(1, 2), 0);
    expect("B active while A remains active", feed(2, 0), 1);
    expect("B release", feed(2, 2), 0);
    expect("B active again before A", feed(2, 0), 0);
    expect("A active while B remains active", feed(1, 0), 1);

    s_last_valid_reed = 0;
    expect("A release only", feed(1, 3), 0);
    expect("B release only", feed(2, 3), 0);

    s_last_valid_reed = 0;
    expect("both-level snapshot counts only source A", feed(1, 0), 1);
    expect("same queued source cannot fabricate B", feed(1, 0), 0);

    s_last_valid_reed = 0;
    unsigned alternating = 0;
    alternating += feed(1, 2);
    alternating += feed(1, 3);
    alternating += feed(2, 1);
    alternating += feed(2, 3);
    alternating += feed(1, 2);
    expect("alternating non-overlap", alternating, 3);

    expect("invalid source", feed(0, 0), 0);
    return 0;
}}
'''
with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / "reed_qualifier.c"
    binary = Path(tmp) / "reed_qualifier"
    source.write_text(harness, encoding="utf-8")
    subprocess.run(
        ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(binary)],
        check=True,
    )
    subprocess.run([str(binary)], check=True)

# Counter and queue safety markers must remain in the production consumer.
take_pulses = extract_function(board_source, "dura_board_take_flow_pulses")
assert "if (s_flow_pulses != UINT32_MAX) s_flow_pulses++;" in take_pulses
assert "flow_event_counts(&event)" in take_pulses
assert take_pulses.count("s_flow_pulses++") == 1
assert "xQueueReceive(s_flow_event_queue, &event, 0)" in take_pulses
assert "xQueueCreate(32, sizeof(dura_flow_event_t))" in board_source

print("REED_FLOW_CONTRACT_PASS")
