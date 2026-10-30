#!/usr/bin/env python3
# 谷仓共创计划 · 谷仓 SDGOODS 开放平台基础工程 · 开发工具
# https://github.com/SDGOODS/SDGOODS-ESP32S3
#
# Copyright (c) 2026 深圳希德创新网络有限公司 (SDGOODS)
# 「谷仓共创计划」与「谷仓 SDGOODS 开放平台」项目、谷仓次元屏（谷仓电子徽章）设备，
#   以及本基础代码的著作权与相关权利，均归深圳希德创新网络有限公司所有。
# SPDX-License-Identifier: Apache-2.0
#
# 本工具以 Apache-2.0 发布：可自由商用。详见 LICENSING.md。
#

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 深圳希德创新网络有限公司 (SDGOODS)
"""Read-only keyboard character coverage and circular hit-area checks; no C build."""
import ast
import math
import re
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]


def main():
    code=(ROOT/'main/apps/ui_radio_pages.c').read_text(encoding='utf-8')
    def number(name):
        return int(re.search(r'^#define\s+'+name+r'\s+(\d+)\b',code,re.M)[1])
    def charset(name):
        literal=re.search(r'static const char '+name+r'\[\] = ("(?:\\.|[^"\\])*");',code)[1]
        return ast.literal_eval(literal)
    alphabet,digits,symbols=(charset(name) for name in ('PW_ABC','PW_NUM','PW_SYM'))
    assert len(alphabet)==26 and set(alphabet)==set('abcdefghijklmnopqrstuvwxyz')
    assert len(digits)==10 and set(digits)==set('0123456789')
    assert len(symbols)==32 and len(set(symbols))==32
    assert set(alphabet+alphabet.upper()+digits+symbols+' ')==set(map(chr,range(32,127)))
    assert [(len(s)+number('PW_TEXT_KEYS')-1)//number('PW_TEXT_KEYS') for s in (alphabet,digits,symbols)]==[2,1,3]
    boxes=[]
    for row in range(number('PW_ROWS')):
        for col in range(number('PW_COLS')):
            boxes.append((number('PW_GRID_X')+col*(number('PW_KEY_W')+number('PW_COL_GAP')),
                          number('PW_GRID_Y')+row*(number('PW_KEY_H')+number('PW_ROW_GAP')),
                          number('PW_KEY_W'),number('PW_KEY_H')))
    for col in range(4):
        boxes.append((number('PW_ACTION_X')+col*(number('PW_ACTION_W')+number('PW_ACTION_GAP')),
                      number('PW_ACTION_Y'),number('PW_ACTION_W'),number('PW_ACTION_H')))
    max_radius=0
    for x,y,w,h in boxes:
        assert w>=42 and h>=30
        for px in (x,x+w-1):
            for py in (y,y+h-1):
                radius=math.hypot(px-180,py-180)
                assert radius<=162,(x,y,w,h,radius)
                max_radius=max(radius,max_radius)
    for i,(x,y,w,h) in enumerate(boxes):
        for a,b,c,d in boxes[i+1:]:
            assert x+w<=a or a+c<=x or y+h<=b or b+d<=y
    assert 138+26<number('PW_GRID_Y')
    print(f'PASS: 95 printable ASCII characters; pages 2/1/3; {len(boxes)} nonoverlapping hit areas; minimum circular edge margin {180-max_radius:.1f}px.')
    print('Static only: C input handling, rendering and physical touch were not executed.')


if __name__=='__main__':main()
