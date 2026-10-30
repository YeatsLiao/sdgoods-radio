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
"""Compile the real LVGL/skin code on the host and export offline render checks.

Requires native gcc and Pillow. Does not emulate hardware audio or physical input.
"""
import os
import re
from pathlib import Path
import shutil
import subprocess
import hashlib
from concurrent.futures import ThreadPoolExecutor
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "output/radio-host-current"
HOST = ROOT / "tools/host_radio"
LVGL = ROOT / "managed_components/lvgl__lvgl"


def main():
    compiler = shutil.which("gcc")
    if not compiler:
        raise SystemExit("Native gcc is required for the offscreen LVGL check")
    OUT.mkdir(parents=True, exist_ok=True)
    sources = sorted((LVGL / "src").rglob("*.c"))
    sources += [HOST / "render.c", ROOT / "main/apps/ui_radio_skin.c", ROOT / "main/apps/ui_radio_kit.c", ROOT / "main/apps/ui_radio_assets.c"]
    sources += [HOST / "integration.c", ROOT / "main/apps/ui_radio.c", ROOT / "main/apps/ui_radio_pages.c", ROOT / "main/radio_settings.c"]
    sources += [ROOT / "main/apps/ui_radio_title_font.c", ROOT / "main/radio_station_identity.c"]
    sources += [ROOT / "main/apps/ui_radio_redraw_assets.c"]
    sources += list((ROOT / "components/sdgoods_board/fonts").glob("*.c"))
    args = ["-std=c11", "-O1", "-DLV_CONF_INCLUDE_SIMPLE", "-D_POSIX_C_SOURCE=200809L"]
    args += ["-I" + str(p) for p in (HOST, ROOT / "main/apps", ROOT / "main", ROOT / "components/sdgoods_board/include", ROOT / "components/sdgoods_launcher/include", LVGL)]
    env = dict(os.environ)
    env["PATH"] = str(Path(compiler).parent) + os.pathsep + env.get("PATH", "")
    cache=OUT/"objects";cache.mkdir(exist_ok=True)
    headers=list(HOST.rglob("*.h"))+list((ROOT/"main").rglob("*.h"))
    header_time=max(p.stat().st_mtime_ns for p in headers)
    objects=[cache/(p.stem+"-"+hashlib.sha256(str(p).encode()).hexdigest()[:10]+".o") for p in sources]
    jobs=[(p,o) for p,o in zip(sources,objects) if not o.exists() or o.stat().st_mtime_ns<max(p.stat().st_mtime_ns,header_time)]
    print(f"Compiling native LVGL renderer: {len(jobs)} changed objects...",flush=True)
    def compile_one(job):
        source,obj=job
        subprocess.run([compiler,*args,"-c",str(source),"-o",str(obj)],cwd=ROOT,env=env,check=True)
    with ThreadPoolExecutor(max_workers=4) as pool:list(pool.map(compile_one,jobs))
    link=[str(p) for p in objects]+["-lm","-o",str(OUT/"render.exe")]
    response=OUT/"link.rsp"
    response.write_text("\n".join('"'+a.replace("\\","/")+'"' for a in link),encoding="utf8")
    subprocess.run([compiler,"@"+str(response)],cwd=ROOT,env=env,check=True)
    subprocess.run([str(OUT / "render.exe"), str(OUT)], cwd=ROOT, env=env, check=True)
    player = HOST / "player"
    subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I" + str(player),
                    "-I" + str(ROOT / "main"), str(player / "player_test.c"), "-lm", "-o", str(OUT / "player-test.exe")],
                   cwd=ROOT, env=env, check=True)
    subprocess.run([str(OUT / "player-test.exe")], cwd=ROOT, env=env, check=True)
    subprocess.run([compiler,"-std=c11","-Wall","-Wextra","-Werror","-I"+str(HOST/"network"),
                    "-I"+str(ROOT/"main"),str(HOST/"network_test.c"),"-o",str(OUT/"network-test.exe")],
                   cwd=ROOT,env=env,check=True)
    subprocess.run([str(OUT/"network-test.exe")],cwd=ROOT,env=env,check=True)
    registry_source = (ROOT / "main/apps/ui_radio_skin.c").read_text(encoding="utf-8")
    registry = re.findall(r'^\s*\{"([^"]+)",\s*"([^"]+)",\s*"([^"]+)"', registry_source, re.M)
    count = int(re.search(r'^#define\s+RADIO_SKIN_COUNT\s+(\d+)',
                         (ROOT / "main/apps/ui_radio_skin.h").read_text(encoding="utf-8"), re.M).group(1))
    assert len(registry) == count and len({row[0] for row in registry}) == count
    shots = [OUT / f"{row[0]}-playing.ppm" for row in registry]
    columns = min(4, count)
    rows = (count + columns - 1) // columns
    sheet = Image.new("RGB", (columns * 360, rows * 392), "#ddddda")
    draw = ImageDraw.Draw(sheet)
    for i, shot in enumerate(shots):
        image = Image.open(shot).convert("RGB")
        mask = Image.new("L", (360, 360))
        ImageDraw.Draw(mask).ellipse((0, 0, 359, 359), fill=255)
        x, y = (i % columns) * 360, (i // columns) * 392
        sheet.paste(image, (x, y), mask)
        draw.text((x + 10, y + 365), registry[i][2], fill="#222222")
    sheet.save(OUT / "skins-overview.png")
    for shot in OUT.glob("*.ppm"):
        Image.open(shot).save(shot.with_suffix(".png"))
    ids=[row[0] for row in registry if row[0] not in ("paper-radio","dot-matrix","sound-field")]
    titles=[next(row[1] for row in registry if row[0]==skin_id) for skin_id in ids]
    from PIL import ImageFont
    font=ImageFont.truetype(str(ROOT/"tools/fonts/NotoSansSC-Regular.ttf"),18)
    sequence=[]
    mask=Image.new("L",(240,240))
    ImageDraw.Draw(mask).ellipse((0,0,239,239),fill=255)
    for f in range(60):
        sheet=Image.new("RGB",(960,((len(ids)+3)//4)*272),"#deded8")
        labels=ImageDraw.Draw(sheet)
        for i in range(len(ids)):
            path=OUT/f"{ids[i]}-anim-{f:02d}.ppm"
            image=Image.open(path).resize((240,240),Image.Resampling.NEAREST)
            x,y=(i%4)*240,(i//4)*272
            sheet.paste(image,(x,y),mask)
            labels.text((x+12,y+245),titles[i],font=font,fill="#252725")
        sequence.append(sheet)
    palette=sequence[0].quantize(colors=256)
    frames=[f.quantize(palette=palette,dither=Image.Dither.NONE) for f in sequence]
    frames[0].save(OUT/"animated-skins.gif",save_all=True,append_images=frames[1:],duration=120,loop=0,disposal=2)
    print("Offline render output:", OUT)


if __name__ == "__main__":
    main()
