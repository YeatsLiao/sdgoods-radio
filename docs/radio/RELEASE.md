# Radio 发布手册（编译 → 打包 → 上传全流程）

适用：把 SDGOODS Radio 发布到谷仓 SDGOODS 开放平台 / GitHub Release。
示例值取自 **rc7 实物发布目录** `dist/radio-1.1.0-rc7/`（权威样例，布局有疑问以它为准）。
平台侧通道区别与字段契约见 [docs/PUBLISHING.md](../PUBLISHING.md) 与 [docs/MCP_CONTRACT.md](../MCP_CONTRACT.md)。

> ⚠️ 两条铁律：**改码必重编 + 版本必 +1**；平台只收**不带地址**的应用包（`pack_app.py` 校验），
> `merge_bin` 合并镜像 / bootloader / 分区表传上去会 403 或让用户设备变砖。

## 1. 前置与环境

```bash
python3 tools/check_env.py          # 退出码 0 才继续；缺失项按提示补齐
```

**版本号唯一来源是仓库根的 `version.txt`**（当前 `1.1.0-rc8`），发布前手动 +1。
三处自动消费它，任何一处都不需要再改：

| 消费方 | 用途 |
| --- | --- |
| 根 `CMakeLists.txt` 的 `project()` | 写入 `esp_app_desc.version`（长按菜单/关于页显示） |
| `components/sdgoods_launcher/tools/gen_app_info.py` | 生成 `SDGOODS_APP_VERSION` 宏（纯透传，不 +1） |
| `tools/sdgoods_publish.py` / `publish_wizard.py` | `mcp-upload` / `mcp-update` 缺省 `--version` |

## 2. 字体（改过文案才需要）

```bash
python tools/gen_fonts.py            # 重扫 main/ 与 components/ 生成子集字体
python tools/gen_fonts.py --check    # 校验零缺字（有缺字直接报错退出）
python tools/font_metrics.py         # 圆屏弦宽/折行检查，超宽会标出
```

台名标题字体子集由 `tools/radio/prepare_radio_title_font.py` 维护（详见脚本头部注释）；
换过素材图集才需要 `tools/radio/prepare_radio_assets.py` / `tools/radio/prepare_radio_redraw.py`。

## 3. 主机回归与离线渲染

```bash
python tools/radio/test_radio_host.py
```

用本机 gcc 把**真实 LVGL + 真实应用源码**编成 `render.exe` 跑一遍：
输出 `output/radio-host-current/`（`skins-overview.png` 总览、`animated-skins.gif` 动图、
各皮肤四态静帧、各页面渲染），终端打印全部 PASS/FAIL 用例（视图重建、动画帧、
手势路由、播放器与 Wi-Fi 状态机）。**有 FAIL 不要继续**。
按 RC 习惯把该目录快照一份：`cp -r output/radio-host-current output/radio-host-rcN`。

## 4. 编译

```bash
idf.py build            # 或 tools/build.sh
```

记录两行硬证据：`Project build complete` 与 `SDGOODS_RADIO.bin binary size`。
**app 分区上限 3,040,870 B（2.9 MiB）**，编译警告 `smallest app partition is nearly full`
即余量见底（rc8 时已剩约 4%），加素材前先评估。当前固件大小史：rc6 = 2,977,168 B，rc7 = 3,015,984 B。

## 5. 应用包校验（必做，本地挡砖）

```bash
python tools/pack_app.py --allow-template-name
# → dist/SDGOODS_RADIO_app.bin + dist/SDGOODS_RADIO_app.json（含 sha256 / chipId / 版本）
```

六道门禁，任一不过退出码 1：非合并镜像（0x8000 无分区表 magic）、ESP magic 0xE9、
`esp_app_desc` magic、chip_id=9（ESP32-S3）、体积 ≤ 槽上限、身份非模板默认名
——本仓库就是官方 demo，包名固定 `SDGOODS_RADIO`，所以**必须带 `--allow-template-name`**。
注意 `pack_app.py` **不会替你重编**；核对 `dist/SDGOODS_RADIO_app.json` 里的版本与 `version.txt` 一致。

## 6. 组装发布目录（此前无记录的环节，按 rc7 实物逆向）

```bash
VER=$(cat version.txt)                    # 例：1.1.0-rc7
REL=dist/radio-$VER
mkdir -p "$REL/firmware" "$REL/docs/design" "$REL/verification"

cp build/SDGOODS_RADIO.bin "$REL/firmware/"
cp LICENSE LICENSING.md NOTICE "$REL/"
cp docs/radio/RADIO_OPERATIONS.md docs/radio/RADIO_STATION_IDENTITY.md \
   docs/radio/RADIO_STREAMING_PLAN.md docs/radio/RADIO_V3_RC*.md \
   docs/radio/RADIO_VISUAL_TUNING.md "$REL/docs/"          # 本版相关文档，按需取舍
cp output/radio-host-$VER/skins-overview.png \
   output/radio-host-$VER/animated-skins.gif "$REL/docs/design/"
cp output/$VER-build.log output/$VER-host.log "$REL/verification/"
```

再手写两个清单文件（都在发布目录根）：

**`RELEASE.json`**（字段照 rc7 实物）：

```json
{
  "version": "1.1.0-rc7",
  "firmware": "firmware/SDGOODS_RADIO.bin",
  "sizeBytes": 3015984,
  "sha256": "3c2e0c09…109fe",
  "maxAppBytes": 3040870,
  "remainingAppBytes": 24886,
  "skins": 11,
  "preview": "docs/design/animated-skins.gif",
  "checks": ["idf.py build", "pack_app.py", "host regression", "font coverage"],
  "hardwareAcceptance": "物理触控手感、持续音频质量与长时间稳定性待真机验收"
}
```

**`CHECKSUMS.sha256`**（`sha256sum` 文本格式，路径相对发布目录根，**不含自身**）：

```bash
cd "$REL" && find . -type f ! -name CHECKSUMS.sha256 -exec sha256sum {} + > CHECKSUMS.sha256 && cd ../..
```

最后打 zip 与校验 sidecar（zip 内含单个顶层目录 `radio-<ver>/`）：

```bash
cd dist
zip -r SDGOODS_RADIO_$VER-update.zip radio-$VER
sha256sum SDGOODS_RADIO_$VER-update.zip > SDGOODS_RADIO_$VER-update.zip.sha256
cd ..
```

Windows PowerShell 等价：`Compress-Archive -Path dist\radio-$VER -DestinationPath dist\SDGOODS_RADIO_$VER-update.zip`；
`Get-FileHash -Algorithm SHA256 <文件>`。

## 7. 发布到平台（MCP 开发者令牌，AI/CI 首选）

```bash
python tools/sdgoods_publish.py set-token "sdg_你的令牌"    # 一次性，存本地
python tools/sdgoods_publish.py mcp-whoami                  # 验令牌有效

# 首次上架（category 必须是平台已有 slug，先 GET /api/categories 核对；
# version 缺省取 version.txt 自动加 v 前缀；--shots 最多 4 张）
python tools/sdgoods_publish.py mcp-upload \
  --file dist/SDGOODS_RADIO_app.bin --name "SDGOODS Radio" \
  --category <平台分类slug> --desc-zh "桌面网络电台：13 套像素皮肤" --desc-en "Desktop internet radio" \
  --shots docs/radio/design/skin/skins-overview.png docs/radio/design/skin/device.jpg

# 已上架后的更新（保 id 与下载量，优先于删除重传）
python tools/sdgoods_publish.py mcp-update <id> --file dist/SDGOODS_RADIO_app.bin
python tools/sdgoods_publish.py mcp-submit <id>             # draft → 提审

# 防砖校验：平台上的包与本地 sha256 一致
python tools/sdgoods_publish.py mcp-download <id> --check-sha256 \
  $(sha256sum dist/SDGOODS_RADIO_app.bin | cut -d' ' -f1)
```

- 令牌失效（`-32001`）：**向用户重新索取 `sdg_` 令牌**，绝不用邮箱 REST 绕过。
- 旧版下架/替换：`mcp-unpublish <id>`（下架）→ `mcp-delete <id>`（仅 draft/rejected 可删）→ 重新 `mcp-upload`。
- 网页手动通道：人类自己在 [sdgoods.ai](https://sdgoods.ai) 传包填表，AI 不代推。

## 8. GitHub Release（可选，tag 触发）

`.github/workflows/release.yml` 在推 `v*` tag 时自动：IDF v5.5 构建 →
`pack_app.py --allow-template-name -b build` → 生成 `SHA256SUMS.txt` 与 `flash-manifest.json` →
挂到 GitHub Release。

```bash
git tag v1.1.0-rc8 && git push origin v1.1.0-rc8     # tag 与 version.txt 保持一致
```

## 9. 收尾自检（对齐 AGENTS.md §8）

- [ ] `idf.py build` 真的过了（留有 `Project build complete` + binary size 两行）
- [ ] `gen_fonts.py --check` 零缺字；`font_metrics.py` 无超宽
- [ ] `pack_app.py` 通过，包是**不带地址**的应用镜像，版本与 `version.txt` 一致
- [ ] 主机回归全 PASS；UI 改动看过主机渲染或真机截屏
- [ ] 临时探针 / 调试代码已删干净
- [ ] 未删任何文件头的版权 / Required Notice 声明
- [ ] `RELEASE.json` 的 sha256 / sizeBytes 与实际 bin 一致；`CHECKSUMS.sha256` 覆盖全部文件
- [ ] zip 解开后是单个 `radio-<ver>/` 顶层目录，`.sha256` sidecar 与 zip 匹配
- [ ] 平台 `mcp-download --check-sha256` 校验通过
