# 参考构建 · Fusion Godot Racing 3.0.0

目标为本地 Racing Starter Kit 3.0.0 的 Shared Authority。2026-11-20 已比对 Racing 目录、Starter 3.0.0 ZIP、Godot 4.6 Preview 555 的 7z 包内的同名文件。三份插件可共用下列原生库证据。平台选库由[扩展清单](../../../../references/photon/photon-fusion-godot-starter-kit-racing_3.0.0/addons/fusion/fusion.gdextension)决定。

| 用途 | `addons/fusion/bin/` 中的文件 | SHA-256 |
| --- | --- | --- |
| Linux x86-64 静态分析，地址采用 ELF 虚拟地址 | `libfusion.linux.editor.x86_64.release.so` | `3BCBEC5F76344BB98F82F4F65448AFDF65206400B4268E28A1D4B95D38EBF61B` |
| Windows x86-64 原版布局探针，Godot 4.7.1 stable | `libfusion.windows.editor.x86_64.release.dll` | `104B1E2C3A0289B7391584C75A90F1480A9DAB750099114C99356A813CE69752` |
| Web 原版多人实验，Godot 4.6.1 随包模板 | `libfusion.web.template_debug.wasm32.nothreads.release.so` | `0330c28f94ea5b361805ac7d66d2d427076553aa536f1d357ca37333558bb19a` |

配套 `export_templates/godot.web.template_debug.wasm32.nothreads.dlink.zip` 的 SHA-256 是 `8e0c5993d79d9172033a4b3dc9b08f4617965b44e5a6edb2be6dc842c99190ad`。555 运行时报告 SDK `3.0.0.2787`、协议 8。555 的 Windows 已测编辑器接口与布局，也记录了区域查询失败。成功的多人房间实测来自原版 Web 扩展。Windows 单独补测的 Preview 684 编辑器 DLL 指纹为 `325537b53a183eef35a995bf940f3bffa885764411555cfd1348976199c77d3e`，运行时报告 `3.0.0.3379`、协议 9。684 的单机房实验只归于该版本。[运行条件和结果](../findings/client-audit.md)

```powershell
# 从 netcode 仓库根目录运行
$bin = '../references/photon/photon-fusion-godot-starter-kit-racing_3.0.0/addons/fusion/bin'
Get-FileHash -Algorithm SHA256 -LiteralPath "$bin/libfusion.linux.editor.x86_64.release.so","$bin/libfusion.windows.editor.x86_64.release.dll"
```

## 三份发行物的比对范围

| 核验对象 | 实际结果 |
| --- | --- |
| `addons/fusion/bin/` 的 17 个库 | 逐个取原始字节的 SHA-256，三份发行物的同名库全部相等。按文件名排序，将每行 `文件名<TAB>小写 SHA-256<LF>` 拼接后取 SHA-256，得到 `1cb668e2182de64eaff099278e5018c1d09087e212e1e5bf570615a3e70bbff0`。 |
| 其他插件文件 | 两个 Web 导出模板 ZIP 原始字节相等。17 个 `.cs`、扩展清单及其 UID、两份许可文本在统一 CRLF/LF 后相等。Racing 与 Starter 的 17 个 C# 脚本 UID 内容不同，独立 7z 未附这些脚本 UID。游戏场景和业务脚本由各样例单独提供。 |
| 可读源与符号 | 包内有 Godot 场景、GDScript、C# 包装和各平台编译库。Linux 编辑器 ELF 的 GNU Build ID 为 `e72d0279dbcdd63d8e33a329dfa3f3d8b15845be`，保留 `.dynsym`，常规 `.symtab` 和 DWARF 调试段缺失。相应 Windows DLL 的 PE 导出表只有 `fusion_init`，其余脚本接口经 Godot ClassDB 注册。 |

按平台工具可见的已定义文本符号、解名后以 `FusionCore::` 和 `PhotonMatchmaking::` 开头统计，Linux 三份均为 221 / 156，Web 两份均为 219 / 154。macOS 三份的 x86-64 与 ARM64 两个切片、iOS 两份均为 208 / 139。Android 四份的 Core 均为 220，Realtime 在 ARM64 为 170、x86-64 为 171。Windows 三份 PE 的正式导出入口均为 `fusion_init`。符号可见性和编译器内联合并影响计数，跨架构对照由公共入口及可观测结果串联。[Linux 函数索引](../findings/core-functions.tsv) · [Godot 类型与信号](../findings/godot-types.tsv)

下列命令在 Windows 上从 `netcode/` 复核 17 个库。7-Zip 通过标准输出返回成员字节。

```powershell
@'
from hashlib import sha256
from pathlib import Path
from subprocess import check_output
from zipfile import ZipFile

photon = Path('../references/photon')
distributions = photon / 'distributions'
racing = photon / 'photon-fusion-godot-starter-kit-racing_3.0.0'
preview = distributions / 'Photon-Fusion-Godot4.6-3.0.0-Preview-555.7z'
seven_zip = 'C:/Program Files/7-Zip/7z.exe'
manifest = []
with ZipFile(distributions / 'photon-fusion-godot-starter_3.0.0.zip') as starter:
    for library in sorted((racing / 'addons/fusion/bin').glob('libfusion.*')):
        member = library.relative_to(racing)
        digest = sha256(library.read_bytes()).hexdigest()
        assert sha256(starter.read(member.as_posix())).hexdigest() == digest, member
        data = check_output([seven_zip, 'x', '-so', str(preview), str(member)])
        assert sha256(data).hexdigest() == digest, member
        manifest.append(f'{library.name}\t{digest}\n')
fingerprint = sha256(''.join(manifest).encode()).hexdigest()
assert len(manifest) == 17, len(manifest)
assert fingerprint == '1cb668e2182de64eaff099278e5018c1d09087e212e1e5bf570615a3e70bbff0', fingerprint
print(len(manifest), fingerprint)
'@ | python -B -
```

复核各平台的符号可见性时，ELF 使用 `llvm-nm -D --defined-only -C --format=posix`，Web 和 iOS 去掉 `-D`，macOS 分别指定 `--arch=x86_64` 和 `--arch=arm64`。在输出中按上述两个命名空间前缀和 `T/W` 文本类型计数。Windows 用 `llvm-readobj --coff-exports` 查看正式导出。Windows 方法名和参数则从实际加载 DLL 后的 Godot ClassDB 取得。[ClassDB 探针](../probes/client_surface.gd)
