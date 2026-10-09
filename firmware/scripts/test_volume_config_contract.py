#!/usr/bin/env python3
"""Source configuration contract (runtime consumption covered by test_home_volume_ui)."""
from pathlib import Path
import re

root=Path(__file__).resolve().parents[1]
kconfig=(root/'components/dura_board/Kconfig.projbuild').read_text()
units=re.findall(r'DURA_UNITS_(\w+)\s*=',(root/'components/dura_meter/include/dura_meter.h').read_text())
expected={f'DURA_BOARD_VOLUME_{u}_DECIMALS':2 for u in units}
expected['DURA_BOARD_VOLUME_2DP_LIMIT_TENTHS']=1999
for name,value in expected.items():
    match=re.search(r'^config '+name+r'\n(.*?)(?=^config |^menu |^endmenu|\Z)',kconfig,re.M|re.S)
    assert match,f'missing Kconfig option {name}'
    block=match[1]
    assert re.search(r'^    int "[^"\n]+"',block,re.M),name
    assert re.search(r'^    default '+str(value)+r'$',block,re.M),name
    assert '    help\n' in block,name
    if name.endswith('_DECIMALS'):assert '    range 0 2\n' in block,name
    for config in ('sdkconfig','sdkconfig.defaults'):
        assert re.search(r'^CONFIG_'+name+'='+str(value)+r'$',(root/config).read_text(),re.M),f'{config}: {name}'
print('PASS per-enum Kconfig definitions, normal sdkconfig and defaults')
