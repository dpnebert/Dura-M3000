#!/usr/bin/env python3
"""Prove title oracle sensitivity using isolated source copies, never canonical C."""
from pathlib import Path
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from customer_title_oracle import TITLE, c_function


def main():
    project = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=project.parent / 'verification/F06/negative-controls')
    out = parser.parse_args().out
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='title-negative-') as temp:
        copy = Path(temp) / 'source'
        shutil.copytree(project / 'components/dura_board', copy / 'components/dura_board')
        (copy / 'scripts').mkdir()
        shutil.copy2(project / 'scripts/customer_title_oracle.py', copy / 'scripts/customer_title_oracle.py')
        board = copy / 'components/dura_board/dura_board.c'
        original = board.read_text()
        manual = c_function(original, 'render_manual_run')
        call = 'dura_lcd_draw_bitmap_1bpp(0, 0, ' + TITLE + ', 128, 8, true);'
        assert manual.count(call) == 1
        variants = {
            'removed-call': manual.replace(call, '/* title draw removed */'),
            'wrong-asset': manual.replace(TITLE, 'dura_asset_logoData'),
            'wrong-coordinate': manual.replace(call, call.replace('(0, 0,', '(0, 1,')),
            'dead-call': manual.replace(call, 'if (false) { ' + call + ' }'),
        }
        def run(name, message=None):
            result = subprocess.run([sys.executable, str(copy / 'scripts/customer_title_oracle.py')],
                                    capture_output=True, text=True, cwd=temp,
                                    env={**os.environ, 'PYTHONDONTWRITEBYTECODE': '1'})
            log = result.stdout + result.stderr
            (out / (name + '.log')).write_text(log + f'\nEXIT {result.returncode}\n')
            if message:
                assert result.returncode != 0 and message in log, (name, log)
                assert 'CalledProcessError' not in log, 'compile failure is not an oracle RED'
            else:
                assert result.returncode == 0, (name, log)
            print('PASS', name)
        run('GREEN-original')
        for name, changed in variants.items():
            board.write_text(original.replace(manual, changed))
            run('RED-' + name, 'Manual title framebuffer/coordinates mismatch')
            board.write_text(original)
            run('GREEN-restored-' + name)
        assets = copy / 'components/dura_board/dura_lcd_assets.c'
        original_assets = assets.read_text()
        prefix, suffix = original_assets.split('const uint8_t ' + TITLE, 1)
        import re
        mutated = re.sub(r'0x([0-9A-Fa-f]{2})', lambda m: f'0x{int(m[1],16)^1:02X}', suffix, count=1)
        assets.write_text(prefix + 'const uint8_t ' + TITLE + mutated)
        run('RED-corrupt-artwork', 'approved genuine customer title bytes changed')
        assets.write_text(original_assets)
        run('GREEN-restored-artwork')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
