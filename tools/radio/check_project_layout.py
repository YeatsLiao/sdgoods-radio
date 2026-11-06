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
"""Read-only repository layout checks; does not build, generate assets or use hardware."""
import ast
import json
import re
from pathlib import Path
from urllib.parse import unquote

ROOT=Path(__file__).resolve().parents[2]


def main():
    errors=[]
    for path in (ROOT/'tools/radio').rglob('*.py'):
        try:ast.parse(path.read_text(encoding='utf-8'),filename=str(path))
        except SyntaxError as exc:errors.append(str(exc))
    cmake=(ROOT/'main/CMakeLists.txt').read_text(encoding='utf-8')
    for name in re.findall(r'"([^"\n]+\.c)"',cmake):
        if not (ROOT/'main'/name).is_file():errors.append('Missing CMake source: '+name)
    for path in (ROOT/'tools/radio/host_radio').rglob('*'):
        if path.suffix not in ('.c','.h'):continue
        for name in re.findall(r'#include\s+"(\.[^"]+)"',path.read_text(encoding='utf-8')):
            if not (path.parent/name).is_file():errors.append(f'{path.relative_to(ROOT)}: missing include {name}')
    for path in [ROOT/'README.md',*(ROOT/'docs/radio').rglob('*.md')]:
        for target in re.findall(r'\]\(([^)]+)\)',path.read_text(encoding='utf-8')):
            target=target.split('#')[0].strip('<>')
            if target and '://' not in target and not target.startswith('mailto:'):
                if not (path.parent/unquote(target)).exists():errors.append(f'{path.relative_to(ROOT)}: missing link {target}')
    skin=(ROOT/'main/apps/ui_radio_skin.c').read_text(encoding='utf-8')
    ids=re.findall(r'\{"([a-z0-9-]+)", "[^"]+", "[^"]+"',skin)
    manifest=json.loads((ROOT/'docs/radio/design/runtime-skins.json').read_text(encoding='utf-8'))
    assert manifest['ids']==ids,'Runtime skin list differs from C registry'
    if errors:raise SystemExit('\n'.join(errors))
    print('PASS: Python syntax, CMake sources, relative includes, local documentation links and runtime skin list.')
    print('No C compilation, asset generation or device operations performed.')


if __name__=='__main__':main()
