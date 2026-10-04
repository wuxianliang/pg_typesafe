# Native Jev parity in `typesafe` — 计划校正稿

**状态：完成（2026-10-04）。** 方案审批后，Pair 完成实现、修复及 pgembed 安装测试；最终 Oracle `untitled-chat-89E63E` 返回 **APPROVE**。最终记录：`docs/evidence/final-oracle-approval.md`；权威运行证据：`docs/evidence/final-overlay-topup-cap.{md,json}`。下文保留历史设计与实验过程，旧状态/证据由最终记录取代。

## 执行进度（协调者维护）

- [x] WI-1：native C engine、shared transport、session/SPI/lifecycle；PG18.4 编译通过，已由 WI-2 standalone suite 间接覆盖运行时合同。bundled TAP 未启用，未作为绿证。
- [x] WI-2：SQL 0.1.0、upgrade、mock/parity/security suite；最终 PG18.4 R1 pass、R2 5/5、R3 16/16。U1-S 保留 PostgreSQL 原生 42809；最终 Oracle 已审核通过。
- [x] WI-3：pgembed 安全 overlay、metadata、PG18.4 bundled runtime 与 allowlist/WIP 证据通过；最终 post-fix top-up-cap overlay 已完成。证据：docs/evidence/final-overlay-topup-cap.{md,json}。
- [x] Oracle 实现终审：`untitled-chat-89E63E` 返回 APPROVE；证据：docs/evidence/final-oracle-approval.md。

**本轮只读实验（2026-10-03，无真实 API）：** Darwin pkg-config / Homebrew curl、bundled PG 18.4 `typesafe.endpoint` SUSET placeholder、stamp/libcurl、PGXS TAP 开关、`postgres_fdw.control` 存在于 `pg_config --sharedir`。**2026-10-03 23:12：** `$PGROOT/share/pgembed/` 为 `dir/0755`，`build-metadata.json` **存在**（`generated_at=2026-10-03T14:48:01+00:00`，无 `pg_textsearch`，`pg_typesafe` 仍为 `0.0.1`/`93a5acbb…`），`typesafe.dylib` 存在，`.venv` 下 `has_extension("pg_typesafe") is True`、`has_extension("pg_textsearch") is False`。**同日 Darwin 工具：** `/usr/bin/vmmap` 对同用户进程列出 `__TEXT` 镜像路径（在 zsh PID 上核实）；`/usr/bin/dyld_info` 只接受 **文件** 参数，不是 live-process 枚举器；本机 SDK **没有** `mach-o/dyld_process_info.h`。postgres backend 在 `CREATE EXTENSION`/`LOAD` 之后的 vmmap 输出 **尚未核实**（实施门禁，见 §D2）。pgembed WIP hunk 以 `git diff -U0` 实测为准（§D9）。

**符号：**

- **不变量：** 现在就固定，实施不得再选。
- **断言：** 实现后由命名测试证明；现在没有代码可绿。
- **预检门禁：** 工具/运行时能力尚未在目标进程上核实。未过预检不得把该路径写成已证明，也不得用附录工具（如 `lsof`）单独放行。

---

## 0. Review-response

**220053 九条 P1（已吸收，仍有效）**：D5 placeholder、D6 G2 ∈{0,1}、D8 config-before-cache、D8b relkind、D7 `TsConn` 不用 socket 差。不再引 `§5.4`/`§5.5`。D8b RLS 用 `SET ROLE`。

**本轮九条 P1（必须全部闭合才能实现；闭合 ≠ Oracle 批准）**

| # | P1 | 合同 | 门禁证据（现在 vs 实施后） | 节 |
|---|---|---|---|---|
| 1 | Darwin 加载镜像 | 同一持久 backend PID；**主证据**为 dyld 支持的进程镜像工具 `/usr/bin/vmmap`；`lsof` **只附录** | **已核实：** vmmap 对同用户 zsh 列出 dylib 路径。**未核实（WI-3 预检）：** 对 bundled postgres 在 `CREATE EXTENSION`+`jev_version()` 之后的同一 `LOAD_PID` 列出 `typesafe.dylib` 与 `/usr/lib/libcurl.4.dylib`。预检失败 → D2 FAIL，禁止 lsof-only 绿 | §D2 |
| 2 | psql SQLSTATE 帧 | 客户端 token `:SQLSTATE`，值匹配 `[0-9A-Z]{5}`；stdout+stderr **非阻塞二进制 multiplex**；诊断有界 | 断言 `test_persistent_psql_sqlstate_token`。禁止用 `ERROR:\s+(\d{5})` 数字解析。`XX000`/`57P01`/`0A000`/`42501`/`57014`/`54000`/`00000` 都必须能表示 | §D3 |
| 3 | D1 overlay | 唯一可执行入口 `$PGEMBED/tools/generate_bundle_metadata.py --local-overlay`；O2 与真实写盘**都只**走该入口；模式只由**初始 LIVE** 冻结；O2 **只**写隔离 workdir（`o2-work/` O_EXCL）；crash env 在 fsync 后 rename 前 `os._exit(91)`；然后 **恰好一次** 真实 overlay；证据文件 fail-fast 唯一（O_EXCL）；语义 baseline 全等 | 断言 `test_d1_evidence_excl`、`test_d1_o2_isolated_then_one_overlay`、`test_d1_semantic_baseline` | §D1 |
| 4 | 计账集合不交 | 六类对象两两不交；`session_bytes=∑`；禁止再加一层 overlapping `outstanding` | 断言 `test_m1_disjoint_buckets` | §D4 |
| 5 | executor 所有权 | 按 `QueryDesc*` 入栈；公开 `jev_*` 只读 executor hook 安装的 backend-local call context（**禁止**栈顶/最近 Start 启发式）；`Portal.cleanup` 链接 `jev_portal_cleanup`；abort 回退不得解引用已释放指针 | 断言 `test_x1_savepoint`、`test_x_nested_portal_abort`。PG18 hook 签名以 `executor.h` 为准（`ExecutorRun_hook(QueryDesc*, ScanDirection, uint64)`） | §D4 |
| 6 | X2 | **测试专用 fault 模式**：提交第一 job 后暂停后续 admission；pump 至第一 HTTP 完成并写 `x2-http-done`；客户端 admission ACK 后才把第二 job 标 QUEUED/ACTIVE，再 ready handshake；ACK **之后**才内部故障；**先** release HTTP/admission barrier 并 reset GUC，**再** cold-miss 恢复。产品 `jev.concurrency` / D6 G2 不变 | 断言 `test_x2_release_before_cold_miss` | §D4 |
| 7 | D7 cancel | 注入 stage 的 handshake（ready→cancel）；`57014` ≠ ProcDie FATAL；恢复禁止用已缓存答案 | 断言 `test_d7_cancel_stage_handshake`、`test_d7_procdie_not_57014` | §D7 |
| 8 | 父目录 allowlist | 新增/改动文件的每个父目录必须在 pre 中 type/mode 不变，或该父目录本身在目录 allowlist | 断言 `test_d9_parent_dir_allowlist` | §D9 |
| 9 | 真实 WIP hunk 清单 | 以 `git diff -U0` hunk 的 **`(relpath, body_sha256)`** 钉住当前 pgembed WIP；完整 hunk sha256（含 `@@` header）**只作证据**；拆分/合并按身份集合判定 | 断言 `test_d9_wip_hunk_manifest`。当前 hunk 表见 §D9（2026-10-03 实测） | §D9 |


---

## 0.1 诚实的未决项（不是 P1 借口）

这些是环境事实或刻意缩小的范围，实施时必须遵守，不能假装已经用 TAP/`make installcheck` 证过。

1. **Bundled PG18 未开 TAP：** `pg_config --configure` 无 `--enable-tap-tests`；PGXS `prove_installcheck` 只会打印 *TAP tests not enabled*。本机系统 Perl 无 `IPC::Run`、无已安装的 `PostgreSQL::Test::Cluster`。WI-3 **不得**把 `make TAP_TESTS=1 installcheck` 当作绿。改用 §D3 的 `pg_regress` + Python 等价测试。
2. **系统 `python3` 的 `import pgembed` 因缺 `psutil` 失败。** 预检必须用 `/Users/wxl/Projects/pgembed/.venv/bin/python`。`.venv` 下 `import pgembed` **成功**。**2026-10-03 23:12：** LIVE 已存在且 `has_extension("pg_typesafe") is True`；`has_extension("pg_textsearch") is False`。WI-3 **预检不得**要求 `has_extension("pg_typesafe") is False`；只要求 import 成功、`pg_textsearch` 为 False，并把当时的 `has_extension("pg_typesafe")` 写入 `$JEV_EVIDENCE/has_extension.pre.txt`。overlay 后新进程必须 True。
3. **已安装 0.0.1 的 `typesafe.batch_size` / `http_concurrency` / `mock_response` 在 `pg_settings.context` 里是 `user`，与当前工作区 `typesafe.c` 的 `PGC_SUSET` 不一致**（测的是 stamp 钉住的 dylib）。0.1.0 的 Jev GUC 以本文件 D5 为准，并用 `pg_settings.context` 断言，不以旧 dylib 为准。
4. **`EXTENSION_PACKAGES` 现有 `pg_textsearch`，`EXTENSION_SO_FILES` 没有该键**（`__init__.py` 相对 HEAD 已脏，见 §D9 钉住的 hunk）。metadata 缺失时 `_detect_extensions` 早退；LIVE 存在时 `_detect_extensions` 读 LIVE。若 LIVE 含 `pg_textsearch` 且 `built` 为 false，import 会对 `EXTENSION_SO_FILES[name]` **KeyError**。overlay **不得**把该键写入 LIVE。规则按 **初始 LIVE 状态** 选模式（§D1），不得事后改口：
   - `live-existing`：baseline = 初始 LIVE 字节。若其中已有 `pg_textsearch` → **失败退出、不写盘**。禁止 `generate()` 覆盖其它扩展（含 stannum）。
   - `bootstrap-generate`：`generate()` **可以**把 `pg_textsearch` 写进**内部 candidate**（当前 generator 遍历整个 `EXTENSIONS` dict），写入 `$JEV_EVIDENCE/build-metadata.generate-candidate.json` 后必须 strip 再作为 pre；candidate **不是** baseline。
   - O2 工作副本不得提升为 baseline。
   `stannum` 等其它扩展字段以所选 baseline 为准（LIVE 现为 stannum `version=0.1.0` / commit `e163585c…`；generator WIP 为 `0.5.0` / `d895c8e9…`）。overlay 只改 `pg_typesafe` + `generated_at`。禁止改 `__init__.py`。
5. **DNS cancel 不得用瞬时 NXDOMAIN 充当证明。** RFC 2606 `.invalid` 在 Darwin stub 上通常立刻失败，只能作为「立即错误」对照。Cancel 门禁见 §D7：合成 stage-entry + cold miss，断言协议层 `57014`（不是 `WHEN OTHERS`），`ProcDie` 仍为 FATAL。
6. **G2 在无 barrier 时允许 0 或 1。** 若产品需要演示「合法 batch 一定已出站」，只用可选 G2b，不把竞态写成「恰好 1」。
7. **当前 Darwin `pginstall`（2026-10-03 23:12）：** 有 `postgres` 18.4、`postgres_fdw.control`、`pg_regress`、`typesafe.dylib`、`share/pgembed/`（`dir/0755`）、`share/pgembed/build-metadata.json`（无 `pg_textsearch`）。WI-3 **禁止** `make bundle-metadata` / `main()` 的 `unlink`。模式由 **WI-3 启动瞬间** 的 LIVE 存在与否决定（当前树为 `live-existing`）。O2 只许写 `$JEV_EVIDENCE/o2-work/`。pgembed WIP 以 §D9 **当前** porcelain/hash 为准，不是旧的「generator 干净」描述。

---

## 1. P1 合同

### D1. Metadata overlay（P1-1）+ canonical source + atomic write

**已核实不变量（2026-10-03 23:12）：** `$PGROOT/share/pgembed/` **存在**，`type=dir`，`mode=0755`，非 symlink。`$PGROOT/share/pgembed/build-metadata.json` **存在**（mode `0600`），**不含** `pg_textsearch`；`extensions.pg_typesafe.version` 仍为 `0.0.1`。`make bundle-metadata` 禁止。Overlay 写 LIVE 必须走下面的唯一 `--local-overlay` 入口与单一 `atomic_write`，禁止 `args.output.unlink()`。`share/pgembed` 若缺失：以精确 `dir/0755` 创建；若已在：校验 type/mode，禁止改成 symlink。模式由 **`--freeze-mode` 那一次** 的初始 LIVE 是否为普通文件决定；O2 / 真实 overlay 只读已冻结的 `overlay.mode`，不得重选。

**Canonical source SHA-256（`scripts/canonical_source_sha256.py`，WI-3 原样落地）**

只扫 **仓库根**（不递归 `sql/`、`test/`、`t/`）：

```text
typesafe*.c
typesafe*.h
Makefile
typesafe.control
typesafe--*.sql
```

算法（UTF-8 相对 POSIX 路径，按字节排序）：

```python
#!/usr/bin/env python3
"""Print lowercase hex SHA-256 of the pg_typesafe install-source manifest."""
import hashlib, pathlib, sys
root = pathlib.Path(sys.argv[1]).resolve()
patterns = ("typesafe*.c", "typesafe*.h", "Makefile", "typesafe.control", "typesafe--*.sql")
files = []
for pat in patterns:
    files.extend(p for p in root.glob(pat) if p.is_file() and p.parent == root)
relpaths = sorted({p.relative_to(root).as_posix() for p in files})
h = hashlib.sha256()
for rel in relpaths:
    data = (root / rel).read_bytes()
    h.update(rel.encode("utf-8"))
    h.update(b"\0")
    h.update(str(len(data)).encode("ascii"))
    h.update(b"\0")
    h.update(data)
sys.stdout.write(h.hexdigest() + "\n")
```

断言：两次运行同一棵树输出相同；加入 `typesafe_jev.c` 后 digest 必须变。

**`pg_typesafe` 记录（必须从安装后产物重建，禁止抄 `EXTENSIONS['pg_typesafe']` 的 0.0.1/`93a5acbb…`）**

`make install` 之后、写 LIVE 之前，扫描：

```text
$PGROOT/share/postgresql/extension/typesafe.control     # default_version 必须是 0.1.0
$PGROOT/lib/postgresql/typesafe.$DLSUFFIX               # 文件存在
$PGROOT/share/postgresql/extension/typesafe--*.sql      # 走 generator 已有的 _installation_sql_paths(stem='typesafe')
```

写入 JSON 的 `extensions.pg_typesafe`：

```text
requested/built = true；skipped = false；skip_reason = null
built_for_postgres_major = 18
create_name = "typesafe"
preload_name = null；requires_preload = false；has_library = true
version = control 的 default_version（断言 == "0.1.0"）
source_commit = null
source_ref = "local-overlay:typesafe"
source_sha256 = <canonical hex>
source_submodules = {}
library/control/install_sql/update_sql = 相对 PGROOT 的 POSIX 路径（与 _relative/_installation_sql_paths 相同规则）
```

禁止把本机绝对路径写入任何字段。禁止输出 JSON 出现键 `pg_textsearch`（见 §0.1.4）。

**唯一 overlay 可执行入口（O2 与真实写盘都必须走这里；禁止 wrapper）**

WI-3 写盘 **唯一** 可执行文件：`$PGEMBED/tools/generate_bundle_metadata.py`。`argv` 含 `--local-overlay` 时走 `local_overlay_cli()` → `freeze_overlay_mode` / `run_overlay`；**禁止** `main()` 的 `args.output.unlink()`。禁止第二个 writer：`scripts/pgembed-metadata-overlay-proof.py` **不得写盘**、不得 `--crash-before-rename` / `--o2-work` wrapper、不得 `python -c` / `cp` 写 LIVE 或 o2-work JSON。

`scripts/pgembed-metadata-overlay-proof.py` **只读**（O1 语义比较）。允许 flags 仅：`--pre PATH --live PATH --canonical HEX --mode-file PATH`。出现 `--crash-before-rename`、`--o2-work`、`--output`、`--install-prefix` → 该脚本 exit 1。不得 `import`/`subprocess` 调用 overlay 写盘。

**CLI（精确 argv / env；O2 与真实 overlay 同一入口）**

```text
# 共同 env
JEV_EVIDENCE=<本次 mkdir 成功的空目录，且不在 $PGROOT 下>   # 必填
JEV_OVERLAY_CRASH_BEFORE_RENAME=1                        # 仅 O2

# 1) 冻结模式（只一次）
python3 "$PGEMBED/tools/generate_bundle_metadata.py" \
  --local-overlay --freeze-mode \
  --install-prefix "$PGROOT"

# 2) O2 隔离 crash proof（必须 --output 指向 o2-work LIVE，且 crash env=1）
JEV_OVERLAY_CRASH_BEFORE_RENAME=1 python3 "$PGEMBED/tools/generate_bundle_metadata.py" \
  --local-overlay \
  --install-prefix "$PGROOT" \
  --pg-typesafe-source-dir "$SRC" \
  --stamp "$STAMP" \
  --output "$JEV_EVIDENCE/o2-work/share/pgembed/build-metadata.json"
# 期望 exit 91。禁止省略 --output。禁止 --output 指向真实 LIVE。

# 3) 恰好一次真实 overlay（禁止 crash env；禁止 --output，默认 LIVE）
unset JEV_OVERLAY_CRASH_BEFORE_RENAME
python3 "$PGEMBED/tools/generate_bundle_metadata.py" \
  --local-overlay \
  --install-prefix "$PGROOT" \
  --pg-typesafe-source-dir "$SRC" \
  --stamp "$STAMP"
```

| flag / env | freeze | O2 | 真实 overlay |
|---|---|---|---|
| `--local-overlay` | 必填 | 必填 | 必填 |
| `--freeze-mode` | 必填 | 禁止 | 禁止 |
| `--install-prefix` | 必填 = `$PGROOT` | 必填 | 必填 |
| `--pg-typesafe-source-dir` | 忽略 | 必填 | 必填 |
| `--stamp` | 忽略 | `bootstrap-generate` 必填；`live-existing` 可省略 | 同 O2 |
| `--output` | 禁止 | 必填，resolve 后必须 **等于** `$JEV_EVIDENCE/o2-work/share/pgembed/build-metadata.json` | 禁止（默认真实 LIVE） |
| `JEV_EVIDENCE` | 必填 | 必填 | 必填 |
| `JEV_OVERLAY_CRASH_BEFORE_RENAME` | 忽略 | **必须** `1` | **必须未设置**（设置 → exit 1，不得碰 LIVE） |
| `--local-overlay-from` / `--crash-before-rename` / `--o2-work` / `--live` / `--mode-file` | **不存在**；出现即 argparse 失败 | 同 | 同 |

`--stamp` 在 `live-existing` 下不调用 `generate()`，不改 baseline。Baseline 只来自 `freeze_overlay_mode` 的初始 LIVE / 已冻结 `overlay.mode`。WI-3 **不得**在调用 O2 之前 `mkdir o2-work`（由 `run_overlay` O_EXCL 创建）。

**Overlay 过程（`generate_bundle_metadata.py` 增补；`--local-overlay` 不得落入 `main()` unlink）**

```python
import copy, json, os, stat, tempfile
from datetime import datetime, timezone
from pathlib import Path

LIVE = Path(install_prefix) / "share/pgembed/build-metadata.json"
EVIDENCE = Path(os.environ["JEV_EVIDENCE"])  # 必须在 PGROOT 外，见 D9
SHARE_PGEMBED_MODE = 0o755

def ensure_share_pgembed(prefix: Path) -> None:
    """Create or verify share/pgembed as dir/0755. Never a symlink."""
    share = prefix / "share"
    if not share.exists():
        share.mkdir(mode=SHARE_PGEMBED_MODE)
        os.chmod(share, SHARE_PGEMBED_MODE)
    path = share / "pgembed"
    if path.is_symlink():
        raise SystemExit("share/pgembed is a symlink")
    if path.exists():
        st = path.lstat()
        if not stat.S_ISDIR(st.st_mode):
            raise SystemExit("share/pgembed exists and is not a directory")
        mode = st.st_mode & 0o777
        if mode != SHARE_PGEMBED_MODE:
            raise SystemExit("share/pgembed mode %s != 0o755" % oct(mode))
        return
    path.mkdir(mode=SHARE_PGEMBED_MODE)
    os.chmod(path, SHARE_PGEMBED_MODE)
    st = path.lstat()
    if path.is_symlink() or not stat.S_ISDIR(st.st_mode) or (st.st_mode & 0o777) != SHARE_PGEMBED_MODE:
        raise SystemExit("failed to create share/pgembed as dir 0755")

def freeze_overlay_mode(live: Path, evidence: Path) -> str:
    """Mode is selected from INITIAL LIVE state. Call once before O2; never again."""
    mode_file = evidence / "overlay.mode"
    if mode_file.exists():
        raise SystemExit("overlay.mode already frozen; refuse to re-select from later LIVE state")
    initial = evidence / "build-metadata.live-initial.json"
    if initial.exists():
        raise SystemExit("live-initial already exists; evidence dir is not unique")
    if live.is_file() and not live.is_symlink():
        mode = "live-existing"
        fd = os.open(initial, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
        try:
            os.write(fd, live.read_bytes())
        finally:
            os.close(fd)
    else:
        mode = "bootstrap-generate"
    fd = os.open(mode_file, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    try:
        os.write(fd, (mode + "\n").encode("utf-8"))
    finally:
        os.close(fd)
    return mode

def read_frozen_mode(evidence: Path) -> str:
    mode = (evidence / "overlay.mode").read_text(encoding="utf-8").strip()
    if mode not in ("live-existing", "bootstrap-generate"):
        raise SystemExit("bad overlay.mode %r" % mode)
    return mode

def atomic_write(path: Path, payload: dict) -> None:
    # LIVE parent 必须已是校验过的 share/pgembed；禁止 mkdir(parents=True)+umask。
    parent = path.parent
    if not parent.is_dir() or parent.is_symlink():
        raise SystemExit("atomic_write parent is not a directory: %s" % parent)
    fd, tmp = tempfile.mkstemp(prefix=f".{path.name}.", dir=parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as fh:
            json.dump(payload, fh, indent=2, sort_keys=True)
            fh.write("\n")
            fh.flush()
            os.fsync(fh.fileno())
        # 真实 writer crash 注入：fsync 之后、rename 之前。os._exit 跳过 finally，
        # 模拟进程被杀。禁止用独立的 os.replace 玩具函数代替这条路径。
        # 仅 O2：run_overlay 已拒绝 LIVE + crash env；此处只认同一 env 名。
        if os.environ.get("JEV_OVERLAY_CRASH_BEFORE_RENAME") == "1":
            os._exit(91)
        os.replace(tmp, path)
        dirfd = os.open(path.parent, os.O_RDONLY)
        try:
            os.fsync(dirfd)
        finally:
            os.close(dirfd)
    finally:
        Path(tmp).unlink(missing_ok=True)

def reconstruct_pg_typesafe(doc: dict, prefix: Path, sha: str) -> dict:
    """Fill extensions.pg_typesafe from post-install artifacts, not EXTENSIONS dict."""
    control = prefix / "share/postgresql/extension/typesafe.control"
    text = control.read_text(encoding="utf-8")
    version = next(
        line.split("=", 1)[1].strip().strip("'")
        for line in text.splitlines() if line.startswith("default_version")
    )
    assert version == "0.1.0", version
    dlsuffix = "dylib" if (prefix / "lib/postgresql/typesafe.dylib").is_file() else "so"
    library = prefix / f"lib/postgresql/typesafe.{dlsuffix}"
    assert library.is_file()
    sql_paths = _installation_sql_paths(prefix / "share/postgresql/extension", "typesafe")
    doc.setdefault("extensions", {})
    ext = doc["extensions"].setdefault("pg_typesafe", {})
    ext.update({
        "requested": True, "built": True, "skipped": False, "skip_reason": None,
        "built_for_postgres_major": 18,
        "create_name": "typesafe", "preload_name": None,
        "requires_preload": False, "has_library": True,
        "version": version, "source_commit": None,
        "source_ref": "local-overlay:typesafe", "source_sha256": sha,
        "source_submodules": {},
        "library": _relative(library, prefix),
        "control": _relative(control, prefix),
        "install_sql": _relative(sql_paths[0], prefix),
        "update_sql": [_relative(p, prefix) for p in sql_paths[1:]],
    })
    assert not any(str(v).startswith("/") for v in (ext["library"], ext["control"], ext["install_sql"], *ext["update_sql"]))
    doc["generated_at"] = datetime.now(timezone.utc).isoformat()
    return doc

def build_pre(mode: str, evidence: Path, stamp_ns) -> dict:
    """pre 永远来自冻结的初始 LIVE 或 strip 后的 generate candidate，不是 O2 路径。"""
    if mode == "live-existing":
        pre = json.loads((evidence / "build-metadata.live-initial.json").read_bytes())
        if "pg_textsearch" in pre.get("extensions", {}):
            raise SystemExit("live-existing baseline contains pg_textsearch; refuse write")
        # 禁止 generate()：否则会按 WIP EXTENSIONS 改写 stannum 并可能插入 pg_textsearch。
    elif mode == "bootstrap-generate":
        candidate = generate(stamp_ns)  # 遍历 EXTENSIONS，当前 WIP 会发出 pg_textsearch
        cfd = os.open(evidence / "build-metadata.generate-candidate.json", os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
        os.write(cfd, (json.dumps(candidate, indent=2, sort_keys=True) + "\n").encode())
        os.close(cfd)
        if "pg_textsearch" in candidate.get("extensions", {}):
            candidate["extensions"].pop("pg_textsearch")
        pre = candidate
    else:
        raise SystemExit("unknown overlay mode %r" % mode)
    pfd = os.open(evidence / "build-metadata.pre.json", os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    os.write(pfd, (json.dumps(pre, indent=2, sort_keys=True) + "\n").encode())
    os.close(pfd)
    return copy.deepcopy(pre)

def run_overlay(prefix, sha, stamp_ns, output=None):
    """output 默认真实 LIVE。O2 必须显式传入 o2-work LIVE。禁止在此处冻结模式。"""
    evidence = Path(os.environ["JEV_EVIDENCE"])
    if not (evidence / "overlay.mode").is_file():
        raise SystemExit("overlay.mode missing; freeze from initial LIVE before O2 or real overlay")
    mode = read_frozen_mode(evidence)
    dest = Path(output) if output is not None else LIVE
    marker = evidence / "real-overlay.done"
    o2_root = (evidence / "o2-work").resolve()
    o2_dest = o2_root / "share" / "pgembed" / "build-metadata.json"
    crash = os.environ.get("JEV_OVERLAY_CRASH_BEFORE_RENAME") == "1"
    isolated = dest.resolve() == o2_dest.resolve()
    if isolated:
        if dest.resolve() == LIVE.resolve():
            raise SystemExit("O2 dest must not be real LIVE")
        if not crash:
            raise SystemExit("O2 isolated crash proof requires JEV_OVERLAY_CRASH_BEFORE_RENAME=1")
        try:
            os.mkdir(o2_root)  # O_EXCL：目录已存在则 FAIL
        except FileExistsError:
            raise SystemExit("o2-work already exists")
    else:
        if dest.resolve() != LIVE.resolve():
            raise SystemExit("real overlay dest must be LIVE")
        if crash:
            raise SystemExit("refuse crash-before-rename on real LIVE")
        if marker.exists():
            raise SystemExit("real overlay already ran; refuse second write")
    if not (evidence / "build-metadata.pre.json").is_file():
        build_pre(mode, evidence, stamp_ns)
    if dest.parent.name == "pgembed" and dest.parent.parent.name == "share":
        ensure_share_pgembed(dest.parent.parent.parent)
    pre = json.loads((evidence / "build-metadata.pre.json").read_bytes())
    doc = copy.deepcopy(pre)
    reconstruct_pg_typesafe(doc, prefix, sha)
    if "pg_textsearch" in doc.get("extensions", {}):
        raise SystemExit("refusing to write LIVE containing pg_textsearch")
    atomic_write(dest, doc)
    if dest.resolve() == LIVE.resolve():
        fd = os.open(marker, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
        os.write(fd, b"1\n")
        os.close(fd)

def local_overlay_cli():
    """唯一 WI-3 overlay argv 入口。禁止落入 main() unlink。"""
    import argparse, subprocess, sys
    p = argparse.ArgumentParser(prog="generate_bundle_metadata.py")
    p.add_argument("--local-overlay", action="store_true", required=True)
    p.add_argument("--freeze-mode", action="store_true")
    p.add_argument("--install-prefix", type=Path, required=True)
    p.add_argument("--pg-typesafe-source-dir", type=Path)
    p.add_argument("--stamp", type=Path)
    p.add_argument("--output", type=Path)
    args = p.parse_args()
    evidence = Path(os.environ["JEV_EVIDENCE"])
    prefix = args.install_prefix.resolve()
    live = prefix / "share/pgembed/build-metadata.json"
    global LIVE
    LIVE = live
    if args.freeze_mode:
        if args.output is not None:
            raise SystemExit("--output forbidden with --freeze-mode")
        print(freeze_overlay_mode(live, evidence))
        return
    if args.pg_typesafe_source_dir is None:
        raise SystemExit("--pg-typesafe-source-dir required")
    src = args.pg_typesafe_source_dir.resolve()
    sha = subprocess.check_output(
        [sys.executable, str(src / "scripts" / "canonical_source_sha256.py"), str(src)],
        text=True,
    ).strip()
    expected = (evidence / "source.sha256").read_text(encoding="utf-8").strip()
    if sha != expected:
        raise SystemExit("canonical sha mismatch vs JEV_EVIDENCE/source.sha256")
    run_overlay(prefix, sha, args.stamp, output=args.output)

if __name__ == "__main__":
    import sys
    if "--local-overlay" in sys.argv:
        local_overlay_cli()
    else:
        main()  # unlink 路径；WI-3 禁止调用
```

**模式（不变量，按初始 LIVE 状态一次选定）**

| 启动时 LIVE | `overlay.mode` | baseline | `pg_textsearch` |
|---|---|---|---|
| 普通文件存在 | `live-existing` | `$JEV_EVIDENCE/build-metadata.live-initial.json`（LIVE 字节拷贝） | 若 baseline 已有该键 → exit 1，**不 strip**（禁止把 LIVE 里的键藏掉） |
| 不存在 / 非普通文件 | `bootstrap-generate` | **不是** generate 原始输出；`generate()` 只写 `build-metadata.generate-candidate.json` | generate 可以发出该键（内部 candidate）；strip 后再当 pre；candidate 不得提升为 LIVE/O1 baseline |

当前树（2026-10-03 23:12）走 `live-existing`。O2 的 `$JEV_EVIDENCE/o2-work/**` **不能**成为 baseline，也不能在 O2 之后被当作「LIVE 已不存在」而改走 bootstrap。

禁止 `cp` 成 `$PGROOT/.../LIVE.pre-*`。禁止 `make bundle-metadata`，不得走 `main()` unlink。


Stamp → generate 参数（仅 `bootstrap-generate`）：

```text
--postgres-version stamp.postgres_version
--postgres-ref stamp.postgres_ref
--postgres-commit stamp.postgres_commit
--configure-flags stamp.postgres_configure
--requested stamp.requested_extensions
--built 扫描 PGROOT 实有 complete 扩展（install 之后必须含 pg_typesafe）
--skipped stamp.requested_extensions 减去 --built
--host-os stamp.host_os
--arch stamp.host_arch
--libc stamp.libc
--deployment-target stamp.macosx_deployment_target
--rust-toolchain stamp.rust_toolchain
--cargo-pgrx-version stamp.cargo_pgrx_version
--tigerfs-version 从 stamp.tigerfs 的 tag 段
--tigerfs-sha256 从 stamp.tigerfs 的 sha 段（final 含 tigerfs 时）
```

**断言 O1 — 语义 baseline（`test_d1_semantic_baseline`）**

`scripts/pgembed-metadata-overlay-proof.py --pre $JEV_EVIDENCE/build-metadata.pre.json --live LIVE --canonical HEX --mode-file $JEV_EVIDENCE/overlay.mode`

完整语义比较：除允许 overlay 的两个位置外，new 与 baseline **深拷贝相等**。允许的差集只有：

- 顶层 `generated_at` 可变；
- `extensions.pg_typesafe` 可新增或按 reconstruct 合同替换。

禁止任何其它键增删改（含其它 extension 记录、嵌套字段、类型变化）。

```python
import copy, json, os
from pathlib import Path

def strip_overlay_delta(doc):
    d = copy.deepcopy(doc)
    d.pop("generated_at", None)
    ex = d.setdefault("extensions", {})
    ex.pop("pg_typesafe", None)
    return d

old = json.loads(Path(pre_path).read_bytes())
new = json.loads(Path(live).read_text())
mode = Path(mode_file).read_text(encoding="utf-8").strip()
assert mode in ("live-existing", "bootstrap-generate")
evidence = Path(os.environ["JEV_EVIDENCE"])
if mode == "live-existing":
    baseline = json.loads((evidence / "build-metadata.live-initial.json").read_bytes())
    assert old == baseline, "pre must be semantically identical to live-initial"
else:
    cand_path = evidence / "build-metadata.generate-candidate.json"
    assert cand_path.is_file()
    assert Path(pre_path).resolve() != cand_path.resolve()
    baseline = old  # stripped generate output; not the raw candidate
assert "pg_textsearch" not in old.get("extensions", {})
assert "pg_textsearch" not in new.get("extensions", {})
assert strip_overlay_delta(new) == strip_overlay_delta(baseline)
assert set(new["extensions"]) == set(old["extensions"]) | {"pg_typesafe"}
rec = new["extensions"]["pg_typesafe"]
assert rec["source_sha256"] == canonical
assert rec["version"] == "0.1.0"
assert rec["source_commit"] is None
assert rec["source_ref"] == "local-overlay:typesafe"
assert rec["built"] is True and rec["library"]
assert not rec["library"].startswith("/")
assert rec["library"].startswith("lib/postgresql/typesafe.")
assert (prefix / rec["library"]).is_file()
for name, rec_old in old["extensions"].items():
    if name != "pg_typesafe":
        assert new["extensions"][name] == rec_old, name
```

`old` 永远是 `$JEV_EVIDENCE/build-metadata.pre.json`，不是 LIVE 旁副本，不是 O2 工作区，不是 `build-metadata.generate-candidate.json`。`live-existing`：pre 与 `live-initial` **对象相等**。`bootstrap-generate`：pre 是 generate 后、reconstruct 前、**已经 strip** 的对象。

**断言 O2 + 次序（`test_d1_o2_isolated_then_one_overlay`）**

固定次序，缺一步或颠倒即 FAIL。O2 与真实写盘 **argv 除 `--output` / crash env 外相同**，都执行上面的 `run_overlay()`：

1. **冻结：** `python3 generate_bundle_metadata.py --local-overlay --freeze-mode --install-prefix $PGROOT`（只此一次，O_EXCL 写 `overlay.mode`）。
2. **O2 只写隔离目录：** 同一可执行文件 `--local-overlay --output $JEV_EVIDENCE/o2-work/share/pgembed/build-metadata.json`，`JEV_OVERLAY_CRASH_BEFORE_RENAME=1`。`run_overlay` **O_EXCL `mkdir o2-work`**，`ensure_share_pgembed(o2-work)`，`atomic_write`：tmp `fsync` 后、`os.replace` 前 `os._exit(91)`。**禁止** `--output` 指向真实 LIVE。**禁止** `pgembed-metadata-overlay-proof.py` 承担此步。
3. 真实 LIVE 字节仍等于 `live-initial`（或仍不存在）。`real-overlay.done` 仍不存在。o2-work 目标 JSON **不存在**（rename 未发生）；允许残留 `.build-metadata.json.*` tmp。
4. **恰好一次**真实 overlay：同一可执行文件 `--local-overlay` 且 **无** `--output`、**无** crash env；`run_overlay(..., output=None)` 写 LIVE；创建 `real-overlay.done`（O_EXCL）。再调用必须 exit 1。
5. O1 语义比较通过（**只读** `pgembed-metadata-overlay-proof.py`）。新 Python 进程 `has_extension("pg_typesafe") is True`。`overlay.mode` 仍是步骤 1 的值。

禁止 `proof_replace_is_atomic` 玩具。禁止先删后写。禁止 O2 之后按「现在 LIVE 在不在」改模式。O2 工作区删除不得 copy 进 `$PGROOT`。

**断言证据唯一（`test_d1_evidence_excl`）**

`$JEV_EVIDENCE` 必须是本次新建的空目录（`mkdir` 成功；若目录已存在且非空 → exit 1）。下列路径一律 `O_CREAT|O_EXCL`，已存在即 FAIL：`overlay.mode`、`build-metadata.live-initial.json`、`build-metadata.pre.json`、`build-metadata.generate-candidate.json`、`real-overlay.done`。`o2-work/` 由 **O2 这次** `run_overlay` `os.mkdir` 独占创建（目录已存在 → exit 1）；WI-3 禁止事先 `mkdir o2-work`。

---


### D2. Curl provider 强制覆盖 + 可执行 load 校验（Review P1-1）

**不变量：** 构建 **禁止** 读取 ambient `pkg-config`。`scripts/install-into-pgembed.sh` 在 `make` 前设置下列环境，Makefile（实施时）优先使用它们：

```make
# 计划合同：TYPESAFE_CURL_LIBS 非空才走覆盖；空字符串不得 ifdef 命中后仍调 pkg-config
ifneq ($(TYPESAFE_CURL_LIBS),)
PG_CPPFLAGS += $(TYPESAFE_CURL_CFLAGS)
SHLIB_LINK  += $(TYPESAFE_CURL_LIBS)
else
PG_CPPFLAGS += $(shell pkg-config --cflags libcurl 2>/dev/null)
SHLIB_LINK  += $(shell pkg-config --libs libcurl 2>/dev/null)
endif
```

**构建 provider ≠ 最终 bundled/runtime libcurl**

| | 构建（compile/link） | 最终（ldd/otool + backend 映射） |
|---|---|---|
| Darwin，`pg_net_curl_prefix` 空 | `TYPESAFE_CURL_LIBS=-lcurl`，`PKG_CONFIG=/usr/bin/false` | `/usr/lib/libcurl.4.dylib`（系统，**不**进 PGROOT） |
| Linux，prefix 非空 | `-I$prefix/include -L$prefix/lib -lcurl` | `$PGROOT/lib/libcurl.so.4`（pgembed 已 `cp` 进 prefix；typesafe.so 的 RPATH 必须能找到它） |

Linux 空 prefix：拒绝。ldd 若仍指向 `$PG_NET_CURL_PREFIX/lib/libcurl.so.4` 而不是 `$PGROOT/lib/libcurl.so.4` → FAIL。Darwin otool/lsof 若出现 Cellar/Homebrew curl → FAIL。

**`scripts/install-into-pgembed.sh`（Bash，不是 sh）**

```bash
#!/usr/bin/env bash
set -euo pipefail
SOURCE=/Users/wxl/Projects/pg_typesafe
STAMP=/Users/wxl/Projects/pgembed/pgbuild/.postgres-bundle-config.stamp
while [[ $# -gt 0 ]]; do
  case "$1" in
    --source) SOURCE=$2; shift 2 ;;
    --stamp) STAMP=$2; shift 2 ;;
    *) echo "usage: install-into-pgembed.sh --stamp PATH --source PATH" >&2; exit 1 ;;
  esac
done
EVIDENCE=${JEV_EVIDENCE:-/tmp/jev-overlay-evidence}
mkdir -p "$EVIDENCE"
LOAD=
LOAD_PY=
cleanup() {
  if [[ -n "${LOAD_PY:-}" ]]; then kill "$LOAD_PY" 2>/dev/null || true; fi
  if [[ -n "${LOAD:-}" && -d "$LOAD/data" ]]; then
    "$PGROOT/bin/pg_ctl" -D "$LOAD/data" -m fast stop >/dev/null 2>&1 || true
  fi
  [[ -n "${LOAD:-}" ]] && rm -rf "$LOAD"
}
trap cleanup EXIT
cp "$STAMP" "$EVIDENCE/pgembed.stamp.pre"

python3 - "$STAMP" "$EVIDENCE/stamp.json" <<'PY'
import json, sys
d = {}
for line in open(sys.argv[1], encoding="utf-8"):
    line = line.strip()
    if not line or line.startswith("#") or "=" not in line:
        continue
    k, v = line.split("=", 1)
    d[k] = v
json.dump(d, open(sys.argv[2], "w"), indent=2)
PY
eval "$(python3 - "$EVIDENCE/stamp.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
def sh(k, envk=None):
    print("%s=%s" % (envk or k.upper(), json.dumps(d[k])))
sh("install_prefix", "PGROOT")
sh("pg_net_curl_prefix", "PG_NET_CURL_PREFIX")
sh("host_os", "HOST_OS")
sh("dlsuffix", "DLSUFFIX")
PY
)"
export PGROOT PG_NET_CURL_PREFIX HOST_OS DLSUFFIX
export PG_CONFIG="$PGROOT/bin/pg_config"
export PATH="$PGROOT/bin:$PATH"
unset PKG_CONFIG_PATH PKG_CONFIG_LIBDIR PKG_CONFIG_SYSROOT_DIR

if [[ -n "$PG_NET_CURL_PREFIX" ]]; then
  export TYPESAFE_CURL_CFLAGS="-I${PG_NET_CURL_PREFIX}/include"
  export TYPESAFE_CURL_LIBS="-L${PG_NET_CURL_PREFIX}/lib -lcurl"
  BUILD_CURL_SUBSTR=$PG_NET_CURL_PREFIX
  RUNTIME_CURL=$PGROOT/lib/libcurl.so.4
  test -f "$RUNTIME_CURL" || { echo "bundled libcurl missing: $RUNTIME_CURL" >&2; exit 1; }
else
  case "$HOST_OS" in
    Darwin)
      export TYPESAFE_CURL_CFLAGS=""
      export TYPESAFE_CURL_LIBS="-lcurl"
      BUILD_CURL_SUBSTR=system-lcurl
      RUNTIME_CURL=/usr/lib/libcurl.4.dylib
      ;;
    *)
      echo "stamp has empty pg_net_curl_prefix on $HOST_OS; refuse" >&2
      exit 1
      ;;
  esac
fi
export BUILD_CURL_SUBSTR RUNTIME_CURL
{
  printf 'build host_os=%s prefix=%s cflags=%s libs=%s\n' \
    "$HOST_OS" "$PG_NET_CURL_PREFIX" "$TYPESAFE_CURL_CFLAGS" "$TYPESAFE_CURL_LIBS"
  printf 'runtime expect=%s\n' "$RUNTIME_CURL"
} | tee "$EVIDENCE/jev-curl-provider.txt"

export PKG_CONFIG=/usr/bin/false
make -C "$SOURCE" clean
make -C "$SOURCE" -n "typesafe.$DLSUFFIX" > "$EVIDENCE/jev-curl-make-n.txt"
if grep -E 'pkg-config|Cellar/curl|/opt/homebrew/.*/curl|/opt/curl' "$EVIDENCE/jev-curl-make-n.txt"; then
  echo "dry-run used ambient curl provider" >&2; exit 1
fi
grep -F -- "$TYPESAFE_CURL_LIBS" "$EVIDENCE/jev-curl-make-n.txt" || {
  echo "dry-run missing TYPESAFE_CURL_LIBS" >&2; exit 1
}

make -C "$SOURCE" -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)" 2>&1 | tee "$EVIDENCE/jev-curl-make.txt"
if grep -E 'pkg-config|Cellar/curl|/opt/homebrew/.*/curl|/opt/curl' "$EVIDENCE/jev-curl-make.txt"; then
  echo "make used ambient curl provider" >&2; exit 1
fi
make -C "$SOURCE" install
LIB=$("$PG_CONFIG" --pkglibdir)/typesafe.$DLSUFFIX
test -f "$LIB"

if [[ "$(uname -s)" == Darwin ]]; then
  otool -L "$LIB" | awk 'NR>1 {print $1}' | grep -E '^/' | while IFS= read -r ref; do
    case "$ref" in
      "$PGROOT"/*)
        install_name_tool -change "$ref" "@loader_path/$(basename "$ref")" "$LIB"
        ;;
    esac
  done
  codesign --force --sign - --timestamp=none "$LIB"
  otool -L "$LIB" | tee "$EVIDENCE/jev-typesafe-otool.txt"
  grep -F /usr/lib/libcurl "$EVIDENCE/jev-typesafe-otool.txt" || exit 1
  if grep -E 'Cellar/curl|/opt/homebrew/.*/libcurl' "$EVIDENCE/jev-typesafe-otool.txt"; then exit 1; fi
elif [[ "$(uname -s)" == Linux ]]; then
  command -v patchelf >/dev/null
  # 单引号：RPATH 必须是字面量 $ORIGIN:$ORIGIN/..（与 pgembed relativize 对 postgresql/*.so 相同）
  # 禁止 '\$ORIGIN'（会把反斜杠写进 DT_RUNPATH）
  patchelf --set-rpath '$ORIGIN:$ORIGIN/..' "$LIB"
  readelf -d "$LIB" | tee "$EVIDENCE/jev-typesafe-readelf.txt"
  grep -F '$ORIGIN:$ORIGIN/..' "$EVIDENCE/jev-typesafe-readelf.txt" || exit 1
  ldd "$LIB" | tee "$EVIDENCE/jev-typesafe-ldd.txt"
  grep -F "$RUNTIME_CURL" "$EVIDENCE/jev-typesafe-ldd.txt" || exit 1
  if grep -F "$PG_NET_CURL_PREFIX/lib" "$EVIDENCE/jev-typesafe-ldd.txt"; then
    echo "ldd still maps libcurl to build prefix" >&2; exit 1
  fi
fi

# ---- 单一持久 client：CREATE EXTENSION/LOAD、打印 PID、保持阻塞；禁止第二段短命 psql ----
LOAD=$EVIDENCE/load-cluster
rm -rf "$LOAD"
mkdir -p "$LOAD/sock"
initdb -D "$LOAD/data" --auth=trust --no-sync --username=jev
LOAD_PORT=
for p in $(seq 25432 25532); do
  if python3 -c 'import socket,sys; s=socket.socket(); s.bind(("127.0.0.1", int(sys.argv[1]))); s.close()' "$p"; then
    LOAD_PORT=$p; break
  fi
done
test -n "$LOAD_PORT"
export LOAD_PORT
"$PGROOT/bin/pg_ctl" -D "$LOAD/data" -l "$LOAD/postgres.log" -w start \
  -o "-c listen_addresses='' -c unix_socket_directories='$LOAD/sock' -c port=$LOAD_PORT"
for _ in $(seq 1 100); do
  "$PGROOT/bin/pg_isready" -h "$LOAD/sock" -p "$LOAD_PORT" -U jev && break
  sleep 0.05
done
export PGHOST=$LOAD/sock PGPORT=$LOAD_PORT PGUSER=jev LC_ALL=C LC_MESSAGES=C
LOAD_PID_FILE=$LOAD/backend.pid
LOAD_READY=$LOAD/maps.ready
LOAD_DONE=$LOAD/maps.done
rm -f "$LOAD_PID_FILE" "$LOAD_READY" "$LOAD_DONE"
python3 - "$LOAD_PID_FILE" "$LOAD_READY" "$LOAD_DONE" "$EVIDENCE/jev-load.sql.out" <<'PY' &
import os, sys, time, subprocess
pid_file, ready_file, done_file, out_file = sys.argv[1:]
psql = os.path.join(os.environ["PGROOT"], "bin", "psql")
proc = subprocess.Popen(
    [psql, "-d", "postgres", "-v", "ON_ERROR_STOP=1", "-A", "-t", "-q"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
SENT = "__JEV_LOAD_END__"
def run(sql):
    proc.stdin.write(sql.rstrip(";") + ";\nSELECT '%s';\n" % SENT)
    proc.stdin.flush()
    lines = []
    while True:
        line = proc.stdout.readline()
        if line == "":
            raise SystemExit("persistent psql died: %s" % proc.stderr.read())
        if line.strip() == SENT:
            break
        lines.append(line)
    return "".join(lines)
chunks = []
chunks.append(run("CREATE EXTENSION typesafe"))
chunks.append(run("SELECT jev_version()"))
chunks.append(run("SHOW server_version"))
pid = run("SELECT pg_backend_pid()").strip()
assert pid.isdigit() and int(pid) > 1, pid
chunks.append(pid + "\n")
open(out_file, "w", encoding="utf-8").write("".join(chunks))
open(pid_file, "w", encoding="utf-8").write(pid + "\n")
proc.stdin.write("SELECT pg_sleep(3600);\n")
proc.stdin.flush()
time.sleep(0.2)
open(ready_file, "w", encoding="utf-8").write("1\n")
for _ in range(2000):
    if os.path.exists(done_file):
        break
    time.sleep(0.05)
else:
    raise SystemExit("maps inspection timeout")
proc.stdin.close()
proc.terminate()
proc.wait(timeout=10)
PY
LOAD_PY=$!
export LOAD_PY
for _ in $(seq 1 200); do
  [[ -f $LOAD_READY ]] && break
  sleep 0.05
done
test -f "$LOAD_READY"
grep -q '18.4' "$EVIDENCE/jev-load.sql.out"
LOAD_PID=$(cat "$LOAD_PID_FILE")
export LOAD_PID
test "$LOAD_PID" -gt 1
# 必须在这个仍阻塞的同一 backend 上取加载镜像。禁止再开短命 psql。
python3 "$SOURCE/scripts/d2_loaded_images.py" \
  --pid "$LOAD_PID" \
  --dlsuffix "$DLSUFFIX" \
  --runtime-curl "$RUNTIME_CURL" \
  --evidence "$EVIDENCE"
: > "$LOAD_DONE"
wait "$LOAD_PY"
LOAD_PY=
# 此处不手动 stop/rm：EXIT trap 清理。
cmp "$EVIDENCE/pgembed.stamp.pre" "$STAMP"
```

**已核实不变量（本机 Darwin stamp）：** `pg_net_curl_prefix` 空；已装 dylib 链 `/usr/lib/libcurl.4.dylib`；系统 libcurl 8.7.1 + AsynchDNS；Homebrew `libcurl.pc` 为 8.21.0 / Cellar 路径。不设隔离时 `pkg-config` 从 Homebrew mac pkgconfig 解析到 SDK `-lcurl`，**一旦 `PKG_CONFIG_PATH` 指向 `opt/curl` 就会链上 Cellar**。因此 Darwin overlay **永不调用 pkg-config**。Linux 上构建 prefix 与 `$PGROOT/lib/libcurl.so.4` 必须分开验证。

**Darwin 加载镜像（P1-1；`test_d2_darwin_loaded_images`）**

同一持久 backend：`CREATE EXTENSION` → `jev_version()` → `SHOW server_version` → 打印 PID → `pg_sleep` 保持阻塞。镜像枚举必须针对该 `LOAD_PID`。禁止第二段短命 `psql`。

| 角色 | 工具 | 规则 |
|---|---|---|
| Darwin **主证据** | `/usr/bin/vmmap $LOAD_PID`（Apple 进程镜像/映射工具；列出 `__TEXT` 路径） | PASS 当且仅当输出同时含 `typesafe.$DLSUFFIX` 与 `$RUNTIME_CURL`（Darwin 为 `/usr/lib/libcurl.4.dylib`） |
| Darwin **附录** | `/usr/sbin/lsof -p $LOAD_PID` → `$EVIDENCE/jev-load-lsof.txt` | 只记录，**不得**单独放行 |
| 明确不用 | `/usr/bin/dyld_info`（只接受文件，不是 live PID）；Homebrew `lldb`；未找到头文件的 `dyld_process_info_create` | 不得写成已证明路径 |
| Linux 主证据 | `/proc/$LOAD_PID/maps` | 同时含 `typesafe.so` 与 `$PGROOT/lib/libcurl.so.4` |

**预检门禁（实施前必须先过，失败即停）：** 2026-10-03 已核实 `/usr/bin/vmmap` 对本机同用户 **zsh** PID 列出 `/usr/lib/*.dylib`。**尚未**对 bundled postgres 在 LOAD 后的 backend 核实。WI-3 在 `pg_sleep` 窗口内跑 vmmap；若该 PID 的 vmmap 没有 typesafe/libcurl 镜像路径 → D2 FAIL。不得退回 lsof-only。

`scripts/d2_loaded_images.py` 合同（短；不要再加一段不可执行的巨壳）：

```python
#!/usr/bin/env python3
"""Primary loaded-image proof for one live backend PID."""
import argparse, os, subprocess, sys
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument("--pid", type=int, required=True)
p.add_argument("--dlsuffix", required=True)
p.add_argument("--runtime-curl", required=True)
p.add_argument("--evidence", required=True)
args = p.parse_args()
evidence = Path(args.evidence)
needle_ext = "typesafe." + args.dlsuffix
if sys.platform == "darwin":
    vmmap = subprocess.check_output(["/usr/bin/vmmap", str(args.pid)], text=True, errors="replace")
    (evidence / "jev-load-vmmap.txt").write_text(vmmap, encoding="utf-8")
    primary = vmmap
    try:
        lsof = subprocess.check_output(["/usr/sbin/lsof", "-p", str(args.pid)], text=True, errors="replace")
    except subprocess.CalledProcessError as exc:
        lsof = exc.output or ""
    (evidence / "jev-load-lsof.txt").write_text(lsof, encoding="utf-8")
else:
    primary = Path("/proc/%d/maps" % args.pid).read_text(encoding="utf-8", errors="replace")
    (evidence / "jev-load-maps.txt").write_text(primary, encoding="utf-8")
if needle_ext not in primary or args.runtime_curl not in primary:
    sys.exit("primary loaded-image listing missing %s or %s" % (needle_ext, args.runtime_curl))
```

**断言：** `cmp` stamp；dry-run 含 `TYPESAFE_CURL_LIBS` 且无 pkg-config/Cellar curl；Darwin otool 为 `/usr/lib/libcurl`；Linux `readelf` 含字面量 `$ORIGIN:$ORIGIN/..` 且 `ldd` 映射 `$PGROOT/lib/libcurl.so.4` 而不是构建 prefix；**一条**持久 client 在同一 `LOAD_PID` 上完成 `CREATE EXTENSION`+LOAD+`jev_version()`+`18.4`，并在该 PID **仍阻塞于 `pg_sleep`** 时取 **vmmap（Darwin）或 /proc/maps（Linux）**。两种主证据都必须同时含 `typesafe.$DLSUFFIX` 与 `RUNTIME_CURL`。lsof 只附录。失败路径只靠 EXIT trap 停库。

---


### D3. Harness 接线（Review P1-2）

**不变量：路径与变量（WI-3 唯一入口 `scripts/pgembed-jev-check.sh`，Bash）**

脚本开头必须 `export` 下列变量（子进程 `bash -c 'test -n "$PGPORT"'` 能看见）。禁止只赋值不 export。

```bash
#!/usr/bin/env bash
set -euo pipefail
PGROOT=/Users/wxl/Projects/pgembed/src/pgembed/pginstall
SRC=/Users/wxl/Projects/pg_typesafe
PG_CONFIG=$PGROOT/bin/pg_config
PG_REGRESS=$PGROOT/lib/postgresql/pgxs/src/test/regress/pg_regress
export PGROOT SRC PG_CONFIG PG_REGRESS
export PATH=$PGROOT/bin:$PATH
FDW_CONTROL=$("$PG_CONFIG" --sharedir)/extension/postgres_fdw.control
export FDW_CONTROL
unset TYPESAFE_API_KEY
export TYPESAFE_API_KEY
test "$(command -v pg_config)" = "$PG_CONFIG"
"$PG_CONFIG" --version | grep -q 'PostgreSQL 18.4'
"$PGROOT/bin/postgres" --version | grep -q 'PostgreSQL) 18.4'
"$PGROOT/bin/psql" --version | grep -q '18.4'
command -v initdb | grep -q "$PGROOT/bin"
command -v pg_ctl  | grep -q "$PGROOT/bin"
test -x "$PG_REGRESS"
test -f "$FDW_CONTROL" || { echo "missing postgres_fdw.control at $FDW_CONTROL" >&2; exit 1; }
```

禁止 `skip_all`。已核实：该 control 在 `pg_config --sharedir` 下存在。

**Mock 协议（`test/jev/mock_api.py`，WI-2 原样落地）**

CLI：

```text
python3 mock_api.py --bind 127.0.0.1 --port 0 --ready-file PATH --log-file PATH
```

- `--port 0`：绑定内核分配端口。
- 进程 **先** `bind/listen`，再把 ready-file **atomic replace** 写入（同 D1 `os.replace`）。未写完不得接受业务 POST。
- ready-file JSON（无其它键也可，但这些必填）：

```json
{"host":"127.0.0.1","port":54321,"url":"http://127.0.0.1:54321/v1/systemone"}
```

`port` 为 int，`url` 必须等于 `http://{host}:{port}/v1/systemone`。

同一 `ThreadingHTTPServer` 路由：

| 方法 | 路径 | 行为 |
|---|---|---|
| POST | `/v1/systemone` | **按 body 分派**（见下） |
| POST | `/ok` | 与 `/v1/systemone` 成功路径相同 |
| POST | `/stall` | `time.sleep(30)`，不写完整 HTTP 响应 |
| POST | `/429` | 429，`Retry-After: 30` |
| POST | `/flaky` | 前两次 429 + `Retry-After: 1`，其后成功（legacy TAP） |
| POST | `/always429` | 永远 429 |
| POST | `/huge` | 200，body ≥ 9MiB |
| POST | `/garbage` | 200，非 JSON |
| POST | `/wrongshape` | 200，`{"model":"jev-latest"}` 无 answers |
| GET | `/_mock/stats` | 见下 |
| POST | `/_mock/reset` | 计数器与 bodies 清零 |
| POST | `/_mock/barrier/arm` | body `{"after_requests":1}`：第 N 个业务 POST 完成后阻塞后续业务 POST |
| POST | `/_mock/barrier/release` | 放行 |

`/v1/systemone` 与 `/ok` 分派（无 Bearer `test-key` → 401，所有路径）：

1. JSON 无 `questions` → 422（legacy TAP）。
2. `state.condition` 含 `trigger422` → 422（pg-jev）。
3. **Jev 方言**（`state.rows` 为数组）：与现 pg-jev mock 相同。qid 为 `rN`/`sN` 时用 `int(qid[1:])` 取行；noul = last-word ∈ row JSON → 0.9 否则 0.1；score/choice = `len(row_json) % n`。
4. **Legacy 单题**（`questions` 含 `label` / `flag` / `rating`）：
   - `label` + choice → `choice=technical`，probabilities 同 `t/001_http.pl`。
   - `flag` + noul → `noul=0.42`。
   - `rating` + score → `score=1.6`，legend/probabilities 同 `sql/typesafe.sql` mock。
5. **Legacy 批量 `sN`**（`state.s0`… 字符串，`questions.sN`）：每个 qid 回 noul `0.42`，usage `input_tokens=7`（`t/002_hardening.pl` `answers_for`）。

`GET /_mock/stats` JSON：

```json
{
  "requests": 3,
  "accepted_connections": 1,
  "bodies": [
    {"path":"/v1/systemone","authorization":"Bearer test-key","json":{"model":"...","state":{"condition":"...","rows":[{}]},"questions":{}}}
  ]
}
```

`requests` = 已完成的业务 POST 数（不含 admin）。`bodies[].json` 是解析后的请求 JSON。`accepted_connections` = HTTP server 接受过的 TCP 连接数（ThreadingHTTPServer 的每次 `handle` 新 conn 计 1；keep-alive 复用同一 conn 不增加）。

**Harness 启动顺序（trap 在第一个资源之后立刻注册；失败即停）**

```bash
WORKDIR=${JEV_CHECK_WORKDIR:-$(mktemp -d /tmp/jev-check-XXXX)}
PGDATA=$WORKDIR/pgdata
SOCKDIR=$WORKDIR/sock
LOGDIR=$WORKDIR/log
MOCK=
export WORKDIR PGDATA SOCKDIR LOGDIR
mkdir -p "$SOCKDIR" "$LOGDIR"
cleanup() {
  if [[ -d ${PGDATA:-/nonexistent} ]]; then
    pg_ctl -D "$PGDATA" -m fast stop >/dev/null 2>&1 || true
  fi
  if [[ -n ${MOCK:-} ]]; then kill "$MOCK" 2>/dev/null || true; fi
  if [[ ${KEEP_JEV_LOGS:-0} != 1 ]]; then rm -rf "$WORKDIR"; fi
}
trap cleanup EXIT

initdb -D "$PGDATA" --auth=trust --no-sync --username=jev
PGPORT=
for p in $(seq 25432 25532); do
  if python3 -c 'import socket,sys; s=socket.socket(); s.bind(("127.0.0.1", int(sys.argv[1]))); s.close()' "$p"; then
    PGPORT=$p; break
  fi
done
test -n "$PGPORT" && test "$PGPORT" -gt 0
export PGPORT
pg_ctl -D "$PGDATA" -l "$LOGDIR/postgres.log" start \
  -o "-c listen_addresses='127.0.0.1' -c unix_socket_directories='$SOCKDIR' -c port=$PGPORT"
ready=0
for _ in $(seq 1 200); do
  if pg_isready -h "$SOCKDIR" -p "$PGPORT" -U jev; then ready=1; break; fi
  sleep 0.05
done
test "$ready" -eq 1
printf '%s\n' "$PGPORT" > "$WORKDIR/pg.port"
export PGHOST=$SOCKDIR PGUSER=jev PGDATABASE=postgres
export PGHOST PGPORT PGUSER PGDATABASE PGDATA

python3 "$SRC/test/jev/mock_api.py" \
  --bind 127.0.0.1 --port 0 \
  --ready-file "$WORKDIR/mock.json" \
  --log-file "$LOGDIR/mock.log" &
MOCK=$!
export MOCK
python3 - "$WORKDIR/mock.json" <<'PY'
import json, sys, time
path = sys.argv[1]
for _ in range(200):
    try:
        d = json.load(open(path, encoding="utf-8"))
        if int(d["port"]) > 0 and d["url"].endswith("/v1/systemone"):
            sys.exit(0)
    except (OSError, ValueError, KeyError, json.JSONDecodeError):
        time.sleep(0.05)
sys.exit("mock ready-file timeout")
PY
export JEV_API_URL=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["url"])' "$WORKDIR/mock.json")
export MOCK_STATS_URL=$(python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print("http://%s:%s/_mock/stats"%(d["host"], d["port"]))' "$WORKDIR/mock.json")
export MOCK_HOST=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["host"])' "$WORKDIR/mock.json")
export MOCK_PORT=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["port"])' "$WORKDIR/mock.json")
export FDW_PORT=$PGPORT
export JEV_HANDSHAKE_DIR=$WORKDIR

# 两个 regression DB + FDW 远端；缺一即 FAIL
createdb contrib_regression_typesafe
createdb contrib_regression_jev
createdb jev_fdw_remote
psql -d jev_fdw_remote -v ON_ERROR_STOP=1 -c \
  "CREATE TABLE jev_remote(id int PRIMARY KEY, name text); INSERT INTO jev_remote VALUES (1,'Berlin'),(2,'Tokyo');"
# 子进程可见性：
bash -c 'test -n "$PGPORT" && test -n "$JEV_API_URL" && test -n "$FDW_PORT" && test -n "$PGHOST" && test -n "$JEV_HANDSHAKE_DIR"'
```

**psql launcher（pg_regress `--launcher`：argv[1] 是 bindir/psql）**

`scripts/psql-jev-launcher.sh`：

```bash
#!/usr/bin/env bash
set -eu
: "${JEV_API_URL:?}" "${FDW_PORT:?}" "${PGUSER:?}"
psql_bin=$1
shift
# 禁止 -v ON_ERROR_STOP=1：pg_regress expected 含 ERROR 行，停掉会杀会话、截断后续语句。
exec "$psql_bin" \
  -v jev_api_url="$JEV_API_URL" \
  -v FDW_PORT="$FDW_PORT" \
  -v USER="$PGUSER" \
  "$@"
```

每个 `test/jev/sql/*.sql` 文件头：

```sql
CREATE EXTENSION IF NOT EXISTS typesafe;
SELECT current_setting('server_version') AS server_version;
SET jev.api_url = :'jev_api_url';
SET jev.api_key = 'test-key';
SET jev.notices = off;
```

`00_server_version` expected 含 `18.4`。R2 **不**使用 `--load-extension`（SQL 自己 CREATE）。

**执行顺序（失败即停，保留 `$LOGDIR`）**

| 步 | 命令 | 产物 |
|---|---|---|
| R1 | `"$PG_REGRESS" --use-existing --host="$PGHOST" --port="$PGPORT" --user="$PGUSER" --bindir="$PGROOT/bin" --dlpath="$PGROOT/lib/postgresql" --inputdir="$SRC" --expecteddir="$SRC" --outputdir="$LOGDIR/reg_typesafe" --dbname=contrib_regression_typesafe typesafe` | `sql/typesafe.sql` + `expected/typesafe.out` |
| R2 | 同上，改 `--inputdir="$SRC/test/jev" --expecteddir="$SRC/test/jev" --outputdir="$LOGDIR/reg_jev" --launcher="$SRC/scripts/psql-jev-launcher.sh" --dbname=contrib_regression_jev`，测试名 `00_server_version 01_basic 02_errors 03_streaming 04_relations` | `test/jev/sql/*.sql` + `test/jev/expected/*.out` |
| R3 | `python3 "$SRC/test/jev/run_python_suite.py"`，环境必须已 export：`PGHOST PGPORT PGUSER PGROOT PGDATA JEV_API_URL MOCK_STATS_URL MOCK_HOST MOCK_PORT FDW_PORT JEV_HANDSHAKE_DIR` | `python.json`；覆盖 001/002 HTTP（含 sN/label/flag/rating 与 `/flaky` `/always429` `/huge` `/garbage` `/wrongshape` `/stall` 401/422）、004 生命周期、005 升级、D8b mock body、G1/G2、**M1/X1/X2/D7 持久 framed 会话** |

`run_python_suite.py`（系统 Python，只用 stdlib + `$PGROOT/bin/psql`）：

一次性查询可用 `ON_ERROR_STOP=1`。**M1 / X1 / X2 / D7 cancel 必须走下方 framed 协议**（断言 `test_persistent_psql_sqlstate_token`）。SQLSTATE **只**来自 psql 变量 `:SQLSTATE`，值必须匹配 `^[0-9A-Z]{5}$`（含 `XX000`、`57P01`、`0A000`、`42501`、`57014`、`54000`、`00000`）。禁止用 `ERROR:\s+(\d{5})` 从报文猜数字码。`query_canceled` 必须是语句 ERROR `57014`，禁止 `WHEN OTHERS` 伪造。`ProcDie` 是 FATAL：帧不完整 / stderr 含 `FATAL:` / stdout EOF，不得改写成 `57014`。

stdout 上必须出现一行 `JEV_SQLSTATE [0-9A-Z]{5}`。这就是 status token。stderr 只作有界诊断，不是 SQLSTATE 来源。

**预检门禁：** 本轮未对 bundled `psql` 18.4 实测 `:SQLSTATE` 展开。WI-3 在跑 M1 之前必须先用同一 `PersistentPsql` 对 `SELECT 1 FROM nonexistent_rel_jev` 断言 token 为 `42P01`（或当前 catalog 对应的 `[0-9A-Z]{5}`），否则 D3 FAIL，禁止退回解析 `ERROR:` 行。


```python
import errno, fcntl, os, re, select, subprocess, time, uuid

psql = os.path.join(os.environ["PGROOT"], "bin", "psql")
SQLSTATE_TOKEN = re.compile(rb"^JEV_SQLSTATE ([0-9A-Z]{5})$")
MAX_STDOUT, MAX_STDERR, STDERR_IDLE_S = 1 << 20, 64 << 10, 0.2

class FrameError(RuntimeError):
    pass

class PersistentPsql:
    """One backend. SQLSTATE from :SQLSTATE only. Binary multiplex both fds."""

    def __init__(self, db="contrib_regression_jev"):
        env = os.environ.copy()
        env["LC_ALL"] = env["LC_MESSAGES"] = "C"
        self.proc = subprocess.Popen(
            [psql, "-d", db, "-v", "ON_ERROR_STOP=0", "-A", "-t", "-q",
             "-v", "jev_api_url=" + os.environ["JEV_API_URL"]],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            bufsize=0, env=env)
        for fd in (self.proc.stdout.fileno(), self.proc.stderr.fileno()):
            fcntl.fcntl(fd, fcntl.F_SETFL, fcntl.fcntl(fd, fcntl.F_GETFL) | os.O_NONBLOCK)
        self._out = b""
        self._err = b""
        self._meta(r"\set VERBOSITY verbose")
        self._meta(r"\set SHOW_CONTEXT never")
        self.backend_pid = self.scalar("SELECT pg_backend_pid();")
        assert int(self.backend_pid) > 1

    def _write(self, s):
        self.proc.stdin.write(s.encode("utf-8"))
        self.proc.stdin.flush()

    def _pump(self, timeout):
        fds = [self.proc.stdout.fileno(), self.proc.stderr.fileno()]
        r, _, _ = select.select(fds, [], [], timeout)
        for fd in r:
            try:
                chunk = os.read(fd, 4096)
            except OSError as exc:
                if exc.errno in (errno.EAGAIN, errno.EWOULDBLOCK):
                    continue
                raise
            if not chunk:
                continue
            if fd == self.proc.stdout.fileno():
                self._out += chunk
                if len(self._out) > MAX_STDOUT:
                    raise FrameError("stdout diagnostics exceeded %d" % MAX_STDOUT)
            else:
                self._err += chunk
                if len(self._err) > MAX_STDERR:
                    raise FrameError("stderr diagnostics exceeded %d" % MAX_STDERR)

    def _take_lines(self):
        lines, rest = [], self._out
        while True:
            i = rest.find(b"\n")
            if i < 0:
                break
            lines.append(rest[:i].rstrip(b"\r"))
            rest = rest[i + 1:]
        self._out = rest
        return lines

    def _wait_end(self, token, deadline_s):
        end_at = time.monotonic() + deadline_s
        marker = ("JEV_END " + token).encode()
        while time.monotonic() < end_at:
            self._pump(max(0.01, end_at - time.monotonic()))
            for line in self._take_lines():
                if line == marker:
                    return
        raise FrameError("missing meta END %s" % token)

    def _meta(self, cmd):
        token = uuid.uuid4().hex
        self._write("%s\n\\echo JEV_END %s\n" % (cmd, token))
        self._wait_end(token, 10.0)
        idle = time.monotonic() + STDERR_IDLE_S
        while time.monotonic() < idle:
            self._pump(max(0.0, idle - time.monotonic()))
        self._err = b""

    def send(self, sql):
        """Write framed SQL without waiting (X2/D7 handshake)."""
        self._token = uuid.uuid4().hex
        body = sql.rstrip().rstrip(";")
        self._write(
            "\\echo JEV_START %s\n%s;\n\\echo JEV_SQLSTATE :SQLSTATE\n\\echo JEV_END %s\n"
            % (self._token, body, self._token))

    def wait_frame(self, deadline_s=60.0):
        end_at = time.monotonic() + deadline_s
        saw_start = saw_state = saw_end = False
        sqlstate = None
        rows = []
        start = ("JEV_START " + self._token).encode()
        end = ("JEV_END " + self._token).encode()
        while time.monotonic() < end_at and not saw_end:
            self._pump(max(0.01, end_at - time.monotonic()))
            for line in self._take_lines():
                if line == start:
                    saw_start = True
                    continue
                m = SQLSTATE_TOKEN.match(line)
                if m:
                    sqlstate = m.group(1).decode("ascii")
                    saw_state = True
                    continue
                if line == end:
                    saw_end = True
                    continue
                if saw_start and not saw_state:
                    rows.append(line.decode("utf-8", "replace"))
            if self.proc.poll() is not None and not saw_end:
                raise FrameError("backend died (ProcDie/FATAL stays fatal): %s"
                                 % self._err.decode("utf-8", "replace")[-512:])
        idle_end = time.monotonic() + STDERR_IDLE_S
        while time.monotonic() < idle_end:
            self._pump(max(0.0, idle_end - time.monotonic()))
        err = self._err.decode("utf-8", "replace")
        self._err = b""
        if not (saw_start and saw_state and saw_end and sqlstate):
            raise FrameError("missing :SQLSTATE frame start=%s state=%s end=%s err=%s"
                             % (saw_start, sqlstate, saw_end, err[:512]))
        if "FATAL:" in err and sqlstate == "57014":
            raise FrameError("FATAL diagnostics must not be mapped to 57014: %s" % err[:512])
        return "\n".join(rows), sqlstate, err[:MAX_STDERR]

    def _run(self, sql):
        self.send(sql)
        return self.wait_frame()

    def scalar(self, sql):
        out, st, err = self._run(sql)
        if st != "00000":
            raise FrameError("unexpected error %s: %s" % (st, err))
        return out.strip()

    def expect_sqlstate(self, sql, state):
        """Capture :SQLSTATE token before the next command. No PL/pgSQL EXCEPTION."""
        if not re.fullmatch(r"[0-9A-Z]{5}", state):
            raise FrameError("wanted SQLSTATE is not [0-9A-Z]{5}: %r" % state)
        if state == "57014" and ("WHEN OTHERS" in sql or re.search(r"\bEXCEPTION\b", sql, re.I)):
            raise FrameError("57014 must not be faked through PL/pgSQL WHEN OTHERS")
        out, st, err = self._run(sql)
        if st == "00000":
            raise FrameError("unexpected success, wanted %s" % state)
        if st != state:
            raise FrameError("expected SQLSTATE %s, got %s: %s" % (state, st, err))
        return out, err

    def assert_same_backend(self):
        assert self.scalar("SELECT pg_backend_pid();") == self.backend_pid

def q(sql, db="contrib_regression_jev"):
    return subprocess.check_output(
        [psql, "-d", db, "-v", "ON_ERROR_STOP=1", "-tAc", sql], text=True).strip()

assert q("SELECT current_setting('server_version')").startswith("18.4")
assert q("SHOW data_directory").startswith(os.environ["PGDATA"])
sess = PersistentPsql()
assert q("SELECT 1") == "1"  # 其它连接可死；sess.backend_pid 必须贯穿 M1/X1/X2/D7
```

`\\echo JEV_SQLSTATE :SQLSTATE` 是 **psql 客户端** 在语句结束后展开的 token，SQL ERROR 之后仍会发出。stdout+stderr 必须在同一 `select` 里非阻塞二进制读取。缺帧、意外 `00000`、意外 SQLSTATE、超限诊断、`FATAL`/EOF 都是 harness FAIL。禁止对 M1/X1/X2/D7 用会因 ERROR 退出的 `psql -v ON_ERROR_STOP=1` 子进程。`04_relations.sql` 里 U1 的 `WHEN OTHERS` 只断言 `0A000`，不用于 `57014`。

**Makefile（实施时，WI-2）：**

```make
DATA = typesafe--0.0.1.sql typesafe--0.1.0.sql typesafe--0.0.1--0.1.0.sql
REGRESS = typesafe
TAP_TESTS = 1
```

`test/jev/sql` **不**加入 `REGRESS=`。WI-3 不跑 TAP。失败日志：`$LOGDIR/reg_*/regression.diffs`、`postgres.log`、`mock.log`、`python.json`。`KEEP_JEV_LOGS=1` 时 trap 不删 `$WORKDIR`。

---


### D4 / D7b. 生命周期、计账、subxact（Review P1-7）

**计账集合（不变量，`test_m1_disjoint_buckets`）**

逻辑字节，**不含** palloc header、MAXALIGN、libcurl/TLS/SPI tuptable/RSS。对象必须落入下列 **两两不交** 的桶之一（同一 CString 只进一个桶）：

| 桶 | 符号 | 成员 |
|---|---|---|
| Cache | `B_CACHE` | cache key / row JSON / answer |
| Job | `B_JOB` | `JevJob` + index + pending map 节点 |
| Fetched | `B_FETCHED` | 尚未 publish 的 SPI 拷贝行 JSON |
| HTTP | `B_HTTP` | ACTIVE/QUEUED request body + response buf |
| Skipped | `B_SKIPPED` | skipped deque（与 fetched/cache 不重复计） |
| Conn | `B_CONN` | `sizeof(TsConn)` |

```c
static Size jev_bucket[6];          /* 上表六桶 */
static Size jev_reserved_bytes;     /* 已 reserve 未 commit；与六桶不交 */
/* SQL 可见： */
/* session_bytes == sum(jev_bucket) */
/* 可见点 jev_reserved_bytes == 0 */
```

公式（禁止第三层 overlapping `outstanding`）：

```text
accounted     = B_CACHE + B_JOB + B_FETCHED + B_HTTP + B_SKIPPED + B_CONN
session_bytes = accounted
cap_check(n)  = accounted + reserved + n > cap  → 54000，且不增加任何计数器
page_admit    = accounted + reserved + N_next_page <= cap
```

对象在桶之间 **移动**（净账按 ΔN）：fetched → cache 时 `B_FETCHED -= n; B_CACHE += n`。禁止 `session_bytes + outstanding` 这种把 fetched/HTTP 加两遍的写法。`jev_outstanding_bytes` **不是**独立计数器；需要“未发布压力”时读 `B_FETCHED + B_HTTP`（已是 `accounted` 的子集，只用于 harvest 决策，不再加到 cap 上）。

`reserve(n)` / `commit(n, bucket)` / `release_reserve(n)` / `free_committed(n, bucket)`。公开 `jev_*` 返回时 `reserved==0`。

**对象 → MemoryContext → 释放（不变量）**

全部 AllocSet 都是 `TopMemoryContext` 的子孙，**禁止**把 Jev 对象留在 `CurrentMemoryContext` / SPI proc context / 用户 subxact context 里过 SPI_finish。

| 对象 | 上下文 | N | reserve | commit | 释放 | COMMIT_SUB | ABORT_SUB（用户） |
|---|---|---|---|---|---|---|---|
| `JevSessionCtx` | `AllocSetContextCreate(TopMemoryContext, "JevSession")` | 不计 | `_PG_init` | — | 进程退出 | 存活 | 存活 |
| `JevCacheContext` | session 子上下文 | — | 首次 cache | — | `jev_cache_clear` `MemoryContextReset` | 存活 | 存活 |
| cache key / row JSON / answer | `JevCacheContext` | `strlen+1` | 插入前 | memcpy 后 | clear / 身份切换 | 已在 cache ctx，不随 subxact 走 | 已发布条目保留 |
| `JevJob` + index | 每 job 一个 `JevJobContext`（session 子） | `sizeof+index` | palloc 前 | palloc 后 | job destroy | 见下 generation | `owner_subid==mySubid` 则 destroy |
| fetched queue 行 JSON | `JevJobContext` | `strlen+1`；**计入 cap 公式** | SPI 拷贝前 | 拷贝后 | 发布进 cache（改记 cache）或 job destroy | 随 job 重挂 | 随 job 销毁 |
| skipped deque | `JevJobContext` | 与 row JSON 同，同一 CString 只计一次 | `skip()` 前 | 入队后 | 弹出 / destroy | 随 job | 随 job |
| request body / response buf | `JevJobContext` | body_len / Δcap | 拷贝/扩大前 | 之后 | harvest 或 cancel | 随 job | 随 job |
| pending map 节点 | `JevJobContext` | `sizeof(node)` | insert 前 | insert 后 | harvest/cancel | 随 job | 随 job |
| `TsConn` | `JevCurlContext`（session 子） | `sizeof(TsConn)` | palloc 前 | 后 | `CLOSED` 删除 | 存活 | 存活（不因用户 savepoint 关 CURLM） |

**SPI 拷贝归属：** `SPI_getvalue` / tuptable 指针在 `SPI_finish` 后无效。内部扫描必须 `MemoryContextStrdup(job_ctx, …)`（或 cache_ctx，若直接发布）**之后**才 `SPI_finish`。禁止 `SPI_palloc` 作为跨 SPI 所有权。

Cap：`jev.max_session_memory_kb * 1024UL`。SQL 可见的 `session_bytes` = `accounted`。触 cap 时 `B_FETCHED` 在 PG_CATCH 中 `free_committed(..., B_FETCHED)`，不得变成 `B_CACHE`。

**不计入：** libcurl 堆、TLS、SPI tuptable、executor、调用栈、palloc chunk 开销。

达到 cap **禁止** eviction 已成功 cache。`jev_cache_clear`：先 cancel 全部 job/请求（释放 `B_JOB`/`B_HTTP`/`B_SKIPPED`/`B_FETCHED`），再 free `B_CACHE`。TsConn IDLE 可保留（仍在 `B_CONN`）。

**54000 清理顺序**

1. `reserve` 失败：本轮未 commit；`PG_CATCH` 把当前 **QueryDesc scope generation** 中尚未 `ACTIVE` 的请求标 `CANCELLED` 且不得 `perform`；`ACTIVE` 的按 D6/D7 cancel，释放其 `B_HTTP`。
2. 已成功 cache **保留**。
3. 回到 SQL 时 `in_flight=0` 且 `jev_reserved_bytes=0`。
4. 提示调用 `jev_cache_clear()` 或提高 `jev.max_session_memory_kb`。

**分页预算（行数维度三分开，字节维度不另加计数器）**

产品默认 `jev.batch_size=20`、`jev.concurrency=16` 不得改成 16-cap 页。

| 预算 | 符号 | 不变量 |
|---|---|---|
| SPI 页大小 | `JEV_SPI_PAGE_ROWS`（编译期 **128**） | 每次内部 `SPI_execute` 最多拷贝这么多行进 `B_FETCHED`。**不**等于 `jev.batch_size` |
| HTTP batch | `jev.batch_size`（默认 **20**） | 一个 HTTP 请求的 `state.rows` 长度。M1 测试可 SET 成 1 |
| 未发布压力 | `B_FETCHED + B_HTTP` | **已包含在** `accounted` 内；只用于是否 harvest，禁止再加到 cap |

规则：

- **禁止**先把 `max_prefetch_rows` 全部读进 queue 再发送。
- 允许 **保留** 尚未 publish 的页/batch（`B_FETCHED` 与 `B_HTTP` 可同时非零）。
- **不得**为了取下一页而强行把当前页 publish 进 cache。
- **skipped-only 翻页：** 若本 SPI 页每一行都是 skip，则只推进 SPI 游标，不 HTTP、不要求本页 publish。
- 下一 SPI 页仅当 `accounted + reserved + N_next_page <= cap` 且 `in_flight < jev.concurrency`。否则有界 harvest：等待已 ACTIVE 的 transfer 完成，完成的 batch 写入 `B_CACHE` 并释放 `B_HTTP`，再重试 admit。若仍不够 → `ereport(54000)`，已成功 cache 保留。
- Adaptive 只缩小 **下一 SPI 页** 到 `Min(JEV_SPI_PAGE_ROWS, 1)` 当 `cap - accounted < 2 * last_row_N`；**不**改 `jev.batch_size`。

**断言 M1（`PersistentPsql`，同一 `backend_pid`）**

```sql
-- sess = PersistentPsql()
CREATE TABLE jev_mem (id int PRIMARY KEY, payload text);
INSERT INTO jev_mem SELECT g, repeat('x', 4000) FROM generate_series(1, 400) g;
SELECT jev_cache_clear();
SET jev.batch_size = 1;
SET jev.notices = off;
SET jev.api_url = :'jev_api_url';
SET jev.api_key = 'test-key';
```

校准（同一会话，高 cap）：

```sql
SET jev.max_session_memory_kb = 65536;
SELECT jev_prob(t, 'the name is Tokyo') FROM jev_mem t WHERE id = 1;
SELECT (jev_stats()->>'session_bytes')::bigint AS s1,
       (jev_stats()->>'cached_answers')::int AS c1;
SELECT jev_cache_clear();
```

`s1` 为一条成功 cache 的字节。然后：

```sql
SET jev.max_session_memory_kb = <ceil(s1 * 3 / 1024.0) 且至少 1024>;  -- cap 允许 ≥1 条 cache，不够 400 条+queue
```

`sess.expect_sqlstate("SELECT jev_prob(jev_mem, 'the name is Tokyo') FROM jev_mem", "54000")`
`sess.assert_same_backend()`

```sql
SELECT (jev_stats()->>'session_bytes')::bigint,
       (jev_stats()->>'in_flight')::int,
       (jev_stats()->>'cached_answers')::int;
```

PASS：`in_flight = 0`；`session_bytes <= cap`；**`cached_answers >= 1`**；`session_bytes >= cached_answers * (s1 的 cache 部分下界)`。`test_m1_disjoint_buckets`：可见点 `reserved==0`；`session_bytes` 等于六桶之和（可用测试 GUC/`jev_stats` 调试字段，不得把六桶做成生产 API）。随后 `jev_cache_clear` + 单行 `jev_prob` 成功，PID 不变。

**Subxact / SPI（PG18 头文件原样）**

```c
typedef void (*SubXactCallback) (SubXactEvent event, SubTransactionId mySubid,
                                 SubTransactionId parentSubid, void *arg);
typedef void (*XactCallback) (XactEvent event, void *arg);
/* SUBXACT_EVENT_ABORT_SUB, XACT_EVENT_ABORT */
```

```c
int jev_spi_depth;                 /* 每次 SPI_connect ++，SPI_finish -- */
/* 内部页扫描必须包一层 subxact，且 depth 在 rollback 之后才 -- */
uint64 jev_call_generation;
uint64 jev_curl_generation;

/* 使用 PG18 utils/portal.h 的 PortalCleanupFunc；不要重新 typedef */
typedef struct JevExecScope {
    QueryDesc *qd;           /* 键：只比较指针，ExecutorEnd / portal cleanup 之后不得解引用 */
    uint64     generation;
    Portal     portal;       /* Start 时的 ActivePortal；可为 NULL */
    PortalCleanupFunc prev_portal_cleanup; /* 仅当 ActivePortal->queryDesc==qd 时非 NULL */
    int        nest;         /* executor 栈深度，不是 xact nest */
    bool       active;
} JevExecScope;

typedef struct JevCallContext {
    QueryDesc    *qd;            /* 本层 Executor* hook 的 queryDesc 参数，不是栈顶扫描 */
    JevExecScope *scope;         /* jev_scope_lookup_exact(qd)；允许 NULL */
    struct JevCallContext *prev; /* 外层 hook 保存的 context */
} JevCallContext;
static JevCallContext *jev_call_context; /* backend-local */
/* 栈：ExecutorStart 且 spi_depth==0 时 push 未激活 scope(qd, ActivePortal) */
/* 公开 jev_* 只读 jev_call_context->qd；禁止「栈顶 / 最近 Start」启发式 */
```

PG18 `executor.h` 签名（实施必须按此，不要编造第四参）：

```c
ExecutorStart_hook(QueryDesc *queryDesc, int eflags);
ExecutorRun_hook(QueryDesc *queryDesc, ScanDirection direction, uint64 count);
ExecutorFinish_hook(QueryDesc *queryDesc);
ExecutorEnd_hook(QueryDesc *queryDesc);
```

`PortalData.queryDesc`（`utils/portal.h`）在 executor 活动时非空。嵌套 SELECT / SQL 函数 / 游标各自有自己的 `QueryDesc`，因此 **禁止** 后端级 `bool jev_executor_owned`。

**按 QueryDesc 的 ownership（`test_x_nested_portal_abort`）**

公开 C 入口如何取得当前 `QueryDesc *`（精确，禁止启发式）：

- PG18 没有「当前 QueryDesc」全局变量，也没有把 desc 传入 `PG_FUNCTION_ARGS`。**唯一合法来源**是 executor hook 的 `queryDesc` 参数。
- `ExecutorStart_hook` / `ExecutorRun_hook` / `ExecutorFinish_hook` 在 `jev_spi_depth==0` 时，用 hook 参数安装 backend-local token：
  `JevCallContext ctx = { .qd = queryDesc, .scope = jev_scope_lookup_exact(queryDesc), .prev = jev_call_context }; jev_call_context = &ctx;` 然后 `PG_TRY` 调 prev hook，`PG_FINALLY` 恢复 `jev_call_context = ctx.prev`。
- `jev_spi_depth>0`：**不**改 `jev_call_context`（内部 SPI executor 不得覆盖用户 QueryDesc）。
- 公开 `jev_*`：`if (jev_call_context == NULL || jev_call_context->qd == NULL) ereport(ERROR, … "outside executor call context")`；`qd = jev_call_context->qd`；`scope = jev_scope_lookup_exact(qd)`（指针相等）。**禁止**「栈顶且 active」「最近 Start」或按 `ActivePortal` 猜 desc。找不到 scope → `ereport`（未 Start 就调用）。
- `jev_scope_lookup_exact` 只按 `scope->qd == qd` 扫栈，不取栈顶作为缺省。

其余栈规则：

- `ExecutorStart_hook` 且 `jev_spi_depth==0`：push `{qd=queryDesc, portal=ActivePortal, prev_portal_cleanup=NULL, generation=0, active=false, nest=stack_len}`。若 `ActivePortal != NULL && ActivePortal->queryDesc == queryDesc`：`prev_portal_cleanup = ActivePortal->cleanup`；`ActivePortal->cleanup = jev_portal_cleanup`（链式；PG18 **没有** `PortalDrop_hook`，这就是 Portal drop 事件接线）。内部 SPI 扫描（`spi_depth>0`）**不** push、**不**改 `portal->cleanup`。
- 公开 `jev_*` 且 `spi_depth==0`：用上面的 call context 取 `qd` 后，若 `!scope->active`，`generation++`（每个 QueryDesc **一次**），`active=true`；然后 `call_generation++`，job 复制该 `generation`。
- 同一 QueryDesc 内多谓词/多行：共享该 scope 的 `generation`，不同 `call_generation`。`ExecutorEnd(queryDesc)` 只收割 **该 qd** 的 generation，不是全后端。
- 嵌套 QueryDesc（SQL 函数里再 `jev_*`，或 `DECLARE c CURSOR FOR SELECT jev_prob...`）：内层 Start push 新 scope；内层 hook 安装内层 call context；内层 End 只 pop/harvest 内层。外层仍 active。内层 **不得** 覆盖外层 `portal->cleanup`（因为 `ActivePortal->queryDesc` 仍是外层 desc）。
- 公开入口且 `spi_depth>0`（Jev 自己的内部扫描又进到 jev_*）：**不** push、不重新认领；挂到 **当前 call context 仍指向的** 用户 scope；没有则误用 ERROR。内部 subxact 对 job **no-op**。
- `ExecutorEnd_hook(queryDesc)`：按指针从栈中移除匹配 scope（通常是栈顶）。harvest 该 generation 已完成响应，cancel 其余 in-flight。**禁止** `queryDesc->` 在 prev hook / `FreeQueryDesc` 之后再读。若栈顶 `qd` 不是这个指针：abort 回退（见下）。`spi_depth>0` 只调 prev hook。End 的 `queryDesc` 来自 hook 参数，不经 call context 猜测。
- **Portal drop 事件（`jev_portal_cleanup`）与状态转移：** PG18 `PortalDrop` / `AtAbort_Portals` → `AtCleanup_Portals` 会调用 `portal->cleanup`（默认 `PortalCleanup` in `src/backend/tcop/pquery.c`）：此时 `portal->queryDesc` **仍非 NULL**，随后 `ExecutorFinish`+`ExecutorEnd`+`FreeQueryDesc` 并把 `portal->queryDesc=NULL`。`jev_portal_cleanup(Portal portal)` 必须：
  1. 在调 prev **之前**用 `scope->portal == portal && scope->prev_portal_cleanup != NULL` 找到被接管的 scope（若 portal->queryDesc 非 NULL，也可 `jev_scope_lookup_exact(portal->queryDesc)` 交叉校验，不一致则 abort 回退）。
  2. 若该 scope 仍在栈上：走与 End 栈不匹配相同的 abort 回退（`nest >= 该 scope->nest` 的 job cancel + 摘栈）。
  3. `portal->cleanup = scope->prev_portal_cleanup`（或直接调用 prev）；然后 `prev(portal)`。prev 返回后 **禁止**再解引用 `queryDesc`。
  4. 若 scope 已在 `ExecutorEnd` 摘掉：只调 prev，no-op。
- **Abort 回退：** `XACT_EVENT_ABORT`、`ABORT_SUB`（用户，`spi_depth==0`）、End 时栈不匹配、或 `jev_portal_cleanup` 发现 scope 仍在栈上：cancel 所有 `nest >= 被中止层` 的 scope 的 job；把那些 scope 从栈摘掉。只保存 `QueryDesc*` 当键，**解引用前必须仍在栈上且当前 End 参数等于该键**（cleanup 路径用 portal 身份，不在 prev 之后读 qd）。
- 用户 PL/pgSQL 捕获 subxact：destroy `owner_subid==mySubid` 的 job；**所属 QueryDesc scope 仍 active** 直到该 desc 的 `ExecutorEnd` 或 portal cleanup。
- identity 变化：`jev_curl_generation++`，cancel 旧 curl generation。
- `COMMIT_SUB` 且 `spi_depth==0`：job 重挂 parent subid；**QueryDesc generation 不变**。`spi_depth>0` no-op。

| 回调 | 条件 | 动作 |
|---|---|---|
| `ExecutorStart_hook` | `spi_depth==0` | push 未激活 scope(`queryDesc`, `ActivePortal`)；若 `ActivePortal->queryDesc==queryDesc` 则链式 `portal->cleanup`；安装 `JevCallContext` 包住 prev |
| `ExecutorRun_hook` / `ExecutorFinish_hook` | `spi_depth==0` | 安装 `JevCallContext{qd=hook参数}` 包住 prev |
| `ExecutorStart/Run/Finish_hook` | `spi_depth>0` | 不改 `jev_call_context`；Start 不 push |
| `ExecutorEnd_hook` | `spi_depth==0` 且栈含该 `queryDesc*` | harvest/cancel 该 generation；pop；保留 cache；prev 之后不得解引用 qd |
| `ExecutorEnd_hook` | 栈顶不匹配 | abort 回退该 nest 及以上 |
| `ExecutorEnd_hook` | `spi_depth>0` | 只调 prev hook |
| `jev_portal_cleanup` | `portal->cleanup` 被调用（`PortalDrop` / `AtCleanup_Portals`）且我们链式接管过 | 若 scope 仍在栈上：abort 回退；再调 `prev_portal_cleanup`（默认 `PortalCleanup` → End/FreeQueryDesc） |
| `SubXactCallback` | `COMMIT_SUB` 且 `spi_depth>0` | no-op |
| `SubXactCallback` | `COMMIT_SUB` 且 `spi_depth==0` | job 重挂 parent；scope 不动 |
| `SubXactCallback` | `ABORT_SUB` 且 `spi_depth>0` | no-op |
| `SubXactCallback` | `ABORT_SUB` 且 `spi_depth==0` | destroy `owner_subid==mySubid` job；scope 留到 End / portal cleanup |
| `XactCallback` | `XACT_EVENT_ABORT` | cancel 全部 job；清空 scope 栈与 `jev_call_context`；保留 cache |

`jev_cache_clear`：先 cancel 再释放 cache。Curl 完成回调禁止调用 PG API。

**断言 X-nest（`test_x_nested_portal_abort`）** 同一 `PersistentPsql` PID：

```sql
CREATE FUNCTION jev_nest() RETURNS float8 LANGUAGE sql AS $$
  SELECT jev_prob(t, 'the name is Tokyo') FROM jev_heap t WHERE id = 1
$$;
SELECT jev_prob(t, 'the name is Tokyo') FROM jev_heap t WHERE id = 1;
SELECT jev_nest();
BEGIN;
DECLARE c CURSOR FOR SELECT jev_prob(t, 'the name is Tokyo') FROM jev_heap t;
FETCH ALL FROM c;
CLOSE c;
ROLLBACK;
SELECT (jev_stats()->>'in_flight')::int;  -- 0
```

嵌套函数与 cursor 都成功；ROLLBACK 后 `in_flight=0`，cache 可保留。禁止因内层 End 把外层 job 提前 cancel（把 `jev_nest` 放进外层 `SELECT jev_prob(...) FROM ...` 的 SELECT 列表时，两层 generation 都必须收割完才 `in_flight=0`）。

**断言 X1**（`PersistentPsql`，PID 贯穿 BEGIN/ROLLBACK TO SAVEPOINT/COMMIT）

```sql
BEGIN;
CREATE TABLE jev_x1 (id int PRIMARY KEY, name text);
INSERT INTO jev_x1 VALUES (1,'Berlin'),(2,'Tokyo');
SAVEPOINT s;
SELECT name, jev_prob(jev_x1, 'the name is Tokyo') AS p FROM jev_x1 ORDER BY id;
ROLLBACK TO SAVEPOINT s;
SELECT (jev_stats()->>'in_flight')::int;  -- 0
SELECT name, jev_prob(jev_x1, 'the name is Tokyo') AS p FROM jev_x1 ORDER BY id;
COMMIT;
```

第二次 SELECT 成功（cache 允许命中）。`expected`：Berlin `0.1`，Tokyo `0.9`。

**断言 X2（`test_x2_release_before_cold_miss`：ACK 后内部故障 → release/reset → 再 cold-miss 恢复）**

测试 GUC：

- `jev._test_internal_spi_fault`（`PGC_SUSET`，默认 off，`GUC_NOT_IN_SAMPLE`）
- `jev._test_handshake_dir`（`PGC_SUSET`，默认 `''`，`GUC_NOT_IN_SAMPLE`）= `$JEV_HANDSHAKE_DIR`

**C 侧（禁止用户 SQL `1/0` view）：** `fault=on` 且 `jev._test_handshake_dir` 非空时进入 **测试专用 fault 模式**，分两道确定性屏障。此 admission gating **只** 在该 fault 模式生效；**不得** 改产品默认 `jev.concurrency=16`，**不得** 改 D6 G2（无 barrier 时 mock 请求数 ∈{0,1}；默认不做「必须先 pump 再 admission」）。禁止只靠 mock HTTP barrier、也禁止靠把 `jev.concurrency` 降到 1，来假设第二 job 已 admission。

**Admission barrier / checkpoint（测试专用：提交第一 job 后暂停后续 admission → pump 至第一 HTTP 完成并写 `x2-http-done` → 客户端 ACK 后才 admission 第二 job）：**
两行 + 产品 `jev.concurrency=16` 时，正常路径允许两个 job 在第一次 HTTP 完成前都进入 QUEUED/ACTIVE，于是「已完成 ≥1 HTTP 且即将 admission 下一条尚未 QUEUED 的 job」永远不会在第一 HTTP 完成后出现，`x2-http-done` 检查点不会触发。因此 **fault 模式必须主动串行化 admission**，步骤：

1. **第一条 job** 正常 admission（标 `QUEUED` 并可 `curl_multi_add_handle` 进入 ACTIVE）。此时 **立即暂停** 本 statement 内第二及之后 job 的 admission，即使 `in_flight < jev.concurrency`。
2. **Pump** `curl_multi` 直到本 statement **第一次 HTTP 完成**。
3. 若 `$handshake_dir/x2-http-done.json` 不存在：atomic replace 写入 `{backend_pid, http_completed, in_flight}`。
4. `WaitLatch` 直到 `$handshake_dir/x2-admit-next.ack` 存在或 10s 超时（超时是测试错误，不是产品 XX000）。
5. **ACK 之后**才把 **第二条** job 标 `QUEUED`（随后 `curl_multi_add_handle` 可被 mock HTTP barrier 阻塞，job 保持 QUEUED/ACTIVE），然后才允许进入 Fault handshake。

第一条 job 的 admission **不等** 此屏障（此时还没有完成的 HTTP）。禁止在第一 HTTP 完成并写出 `x2-http-done.json`、并收到 `x2-admit-next.ack` 之前，把第二及之后 job 标 QUEUED/ACTIVE。

**Fault handshake：** 仅当 `fault=on` 且 `jev_spi_depth>0` 且 **admission barrier 已通过**（第二 job 已被 admission）且当前 **QueryDesc scope** 已有 ≥1 个 `JevJob` 为 QUEUED 或 ACTIVE 且本 statement 已完成 ≥1 个 HTTP 时（第一 job 的 HTTP 完成后可能已离开 QUEUED/ACTIVE，因此 handshake 依赖第二 job）：

1. 把 `{backend_pid, jobs, in_flight}` 写入 `$handshake_dir/x2-job-ready.json`（atomic replace）。禁止在 `x2-admit-next.ack` 之前写 ready。
2. `WaitLatch` 直到 `$handshake_dir/x2-handshake.ack` 存在或 10s 超时（超时是测试错误，不是产品 XX000）。
3. **ACK 之后**再核实 job 仍为 QUEUED/ACTIVE，然后 `ereport(ERROR, errcode(ERRCODE_INTERNAL_ERROR), errmsg("jev_test: internal SPI fault"))`。不得在 handshake ACK 之前 ereport。

用户查询里的 view/表达式错误不算。不得在 handshake ACK 之前 ereport。

`PersistentPsql` + mock 序列（stats/错误观察都在 `sess` 上）：

```sql
CREATE TABLE jev_x2_heap (id int PRIMARY KEY, name text);
INSERT INTO jev_x2_heap VALUES (1,'Berlin'),(2,'Tokyo');
SELECT jev_cache_clear();
SET jev.batch_size = 1;
-- 不得 SET jev.concurrency；产品默认 16 保持不变。admission 串行化只在 fault 模式 C 侧 gating，不改 G2。
```

Python 在 `send` 之前：`sess.scalar("SET jev._test_handshake_dir = '%s'" % os.environ["JEV_HANDSHAKE_DIR"].replace("'", "''"))`。

Python（`send` 后不得在同一连接上插其它 SQL，直到 `wait_frame`）：

```python
import json, os, time, urllib.request
hdir = os.environ["JEV_HANDSHAKE_DIR"]
ready, ack = os.path.join(hdir, "x2-job-ready.json"), os.path.join(hdir, "x2-handshake.ack")
http_done, admit = os.path.join(hdir, "x2-http-done.json"), os.path.join(hdir, "x2-admit-next.ack")
for p in (ready, ack, http_done, admit):
    if os.path.exists(p):
        os.unlink(p)
def mock(path, data=None):
    url = os.environ["MOCK_STATS_URL"].replace("/_mock/stats", path)
    body = None if data is None else json.dumps(data).encode()
    headers = {"Content-Type": "application/json"} if body else {}
    req = urllib.request.Request(url, data=body, method="POST", headers=headers)
    return urllib.request.urlopen(req).read()
try:
    mock("/_mock/reset", {})
    mock("/_mock/barrier/arm", {"after_requests": 1})  # HTTP barrier：第 2 个 POST 阻塞
    sess.scalar("SET jev._test_internal_spi_fault = on")
    sess.send("SELECT jev_prob(h, 'the name is Tokyo') FROM jev_x2_heap h")
    stats = None
    for _ in range(200):
        stats = json.loads(urllib.request.urlopen(os.environ["MOCK_STATS_URL"]).read().decode())
        if stats["requests"] >= 1:
            break
        time.sleep(0.05)
    else:
        raise SystemExit("X2 mock first HTTP timeout")
    for _ in range(200):
        if os.path.isfile(http_done):
            break
        time.sleep(0.05)
    else:
        raise SystemExit("X2 http-done timeout")
    # C 侧已在第一 job 提交后暂停后续 admission；写 ACK 才放行第二 job 进入 QUEUED/ACTIVE
    open(admit, "w", encoding="utf-8").write("1\n")
    for _ in range(200):
        if os.path.isfile(ready):
            break
        time.sleep(0.05)
    else:
        raise SystemExit("X2 job-ready timeout")
    info = json.loads(open(ready, encoding="utf-8").read())
    assert str(info["backend_pid"]) == sess.backend_pid, info
    assert int(info["jobs"]) >= 1, info
    open(ack, "w", encoding="utf-8").write("1\n")
    _out, st, err = sess.wait_frame()
    if st == "00000":
        raise SystemExit("X2 unexpected success")
    if st != "XX000" or "jev_test: internal SPI fault" not in err:
        raise SystemExit("X2 expected XX000 jev_test, got %s %s" % (st, err))
    sess.assert_same_backend()
    assert sess.scalar("SELECT (jev_stats()->>'in_flight')::int") == "0"
    # 恢复前必须 release HTTP barrier + reset GUC。失败则不得跑 recovery（fail-fast）。
    mock("/_mock/barrier/release", {})
    if os.path.exists(admit):
        os.unlink(admit)
    sess.scalar("RESET jev._test_internal_spi_fault")
    sess.scalar("RESET jev._test_handshake_dir")
    sess.scalar("SELECT jev_cache_clear()")
    hits = int(sess.scalar("SELECT (jev_stats()->>'cache_hits')::int"))
    qtext = "the name is Tokyo x2-recover %s" % time.time_ns()
    sess.scalar("SELECT jev_prob(h, '%s') FROM jev_x2_heap h WHERE id = 2" % qtext.replace("'", "''"))
    assert int(sess.scalar("SELECT (jev_stats()->>'cache_hits')::int")) == hits  # cold miss
    sess.assert_same_backend()
finally:
    try:
        mock("/_mock/barrier/release", {})
    except Exception:
        pass
    try:
        sess.scalar("RESET jev._test_internal_spi_fault")
        sess.scalar("RESET jev._test_handshake_dir")
    except Exception:
        pass
    for p in (ready, ack, http_done, admit):
        if os.path.exists(p):
            os.unlink(p)
```

PASS：第一 job 提交 → C 侧暂停第二+ → 第一 HTTP → `x2-http-done.json` → 测试写 `x2-admit-next.ack` → 第二 job 已 QUEUED/ACTIVE 才出现 `x2-job-ready.json` → handshake ACK → XX000；**recovery SQL 不得在 HTTP barrier 仍 arm、admission ack 仍卡住、或 fault GUC 仍 on 时执行**；recovery 前 `jev_cache_clear` + 唯一问句，`cache_hits` 不增加。错误与恢复同一 PID。禁止 `in_flight>0` 残留。禁止 `CREATE VIEW … 1/0`。`finally` 只是安全网，不能代替步骤里的 release/reset。此 gating 不影响产品 concurrency / G2。

---


### D5. GUC 权限、delta、placeholder（Review P1-3）

**已核实不变量（PG 18.4 `guc.c` + 本机实验，对象为已安装 `typesafe.endpoint`，`pg_settings.context=superuser`）：**

1. 模块未加载时，非超用户 `SET typesafe.endpoint = '…'` **成功**（placeholder 为 `PGC_USERSET`）。
2. 随后该 backend 第一次调用 C 入口触发 `_PG_init`：`WARNING: permission denied to set parameter "typesafe.endpoint"`（`define_custom_variable` → `set_config_option_ext(..., WARNING)`），值 **回到 GUC 默认**，不是攻击者值。
3. 加载后非超用户 `SET` → **ERROR** `42501`。
4. 超用户 `ALTER ROLE attacker SET typesafe.endpoint = '…'` 后，attacker 登录 **保留** 该值；attacker 自己 `SET` 仍 ERROR。
5. `ALTER ROLE` 对未知 custom GUC：非超用户会被拒；超用户可以写入 `pg_db_role_setting`（`validate_option_array_item`）。

因此：**不**在 `_PG_init` 里额外清 placeholder 作为主防护。核心已经拒绝把非超用户 placeholder 应用到 `PGC_SUSET`。实施不得「假设能绕过」而发明另一套策略。

**断言（004 / Python，每个 SUSET 名都要跑）：** 对 `jev.api_key`、`jev.api_url`、`jev.batch_size`、`jev.concurrency`、`jev.max_prefetch_rows`、`jev.max_rows_per_statement`、`jev.max_chars_per_statement`、`jev.max_session_memory_kb` 重复 1–4。`jev.api_key` 另测 `GUC_SUPERUSER_ONLY`：非超用户 `SHOW` → examine 拒绝。

若某次断言失败（值粘住），那是实施 bug，应修 GUC 注册 flags，而不是静默改合同。

**Key 解析（仅 Jev live，不变量）**

1. 非空 `jev.api_key`
2. 否则服务器 `TYPESAFE_API_KEY`
3. 否则 ERROR `jev: no API key. …`  
不读 `typesafe.api_key` / `typesafe.api_key_file`。空字符串视为未设。

**完整 GUC 表（每一行相对 pg-jev 的 delta 必须标注）**

| 设置 | Default | 范围 | Context | vs pg-jev | 测试 |
|---|---|---|---|---|---|
| `jev.api_key` | `''` + `GUC_NO_SHOW_ALL\|GUC_SUPERUSER_ONLY\|GUC_NOT_IN_SAMPLE` | string | `PGC_SUSET` | **security delta**（pg-jev 任意角色可 SET） | placeholder 套件；SHOW 隐藏；env fallback |
| `jev.api_url` | TypeSafe HTTPS | 解析后的 URL | `PGC_SUSET` | **security delta** | placeholder；SSRF |
| `jev.batch_size` | 20 | 1..128 | `PGC_SUSET` | **resource delta** | `pg_settings.context=superuser` |
| `jev.concurrency` | 16 | 1..64 | `PGC_SUSET` | **resource delta** | 同上 |
| `jev.max_prefetch_rows` | 5000 | 1..1e6 | `PGC_SUSET` | **resource delta** | 同上 |
| `jev.max_rows_per_statement` | 0 | 0..INT_MAX | `PGC_SUSET` | **resource delta** | D6 |
| `jev.max_chars_per_statement` | 0 | 0..INT_MAX | `PGC_SUSET` | **resource delta** | D6 |
| `jev.max_session_memory_kb` | 65536 | 1024..1048576 | `PGC_SUSET` | **additive resource delta** | M1 |
| `jev.model` | `jev-latest` | string | `PGC_USERSET` | parity | cache miss on change |
| `jev.threshold` | 0.5 | [0,1] | `PGC_USERSET` | parity | 三层优先级 |
| `jev.timeout` | 30 s | 0.001..600 | `PGC_USERSET` | parity | stall |
| `jev.keepalive` | 600 | 0..86400 | `PGC_USERSET` | parity | C3 |
| `jev.notices` | on | bool | `PGC_USERSET` | parity | on/off |
| `jev._test_internal_spi_fault` | off | bool | `PGC_SUSET` + `GUC_NOT_IN_SAMPLE` | **test-only** | X2 |
| `jev._test_handshake_dir` | `''` | path | `PGC_SUSET` + `GUC_NOT_IN_SAMPLE` | **test-only** | X2 |
| `jev._test_cancel_stage` | `''` | `dns`/`connect`/`response`/`retry_sleep`/空 | `PGC_SUSET` + `GUC_NOT_IN_SAMPLE` | **test-only** | D7 |

全部 `MarkGUCPrefixReserved("jev")`。与 `typesafe.*` **零 alias**。

---

### D6. Spend guard 与 G2 竞态（Review P1-4）

**不变量：** 逐 batch，在 `curl_multi_add_handle` **之前** reservation。超限 ERROR，该 batch 不得进入 `QUEUED`。先通过的 batch **可以**已发送。cache hit / retry 不计。`jev_cache_clear` 不重置 statement 计数。guard **不**增加 `errors`。字符 = Unicode code point（Python `len`）。

Guard 失败时：`PG_CATCH` 对 **尚未 ACTIVE** 的同 QueryDesc scope 请求 `CANCELLED` 且不得 `perform`；已 ACTIVE 的合法 batch 可完成或被 cancel（见 G2）。

**验收**

| ID | 合同 |
|---|---|
| G1 | 6 行，`batch_size` 默认，`max_rows=3` → mock **0** 请求 |
| **G2** | 6 行，`batch_size=2`，`max_rows=3` → mock 请求数 ∈ **{0,1}**；若为 1，body 的 `state.rows` 长度为 2；**没有任何** body 的 rows 长度为 3 或使累计>3；ERROR 文本含 would send；`errors` 不变 |
| G2b（可选，确定性） | mock `/barrier`：第一批 POST 后阻塞直到测试进程写 ready 文件；再 admission 第二批。此时 mock **恰好 1** |
| G3 | 6 行，每行 JSON 字符数使累计 Unicode code point = 255，`max_chars_per_statement=254` → mock **0** 请求，ERROR 含 would send |
| G4 | 含 emoji 的一行，`max_chars` = `len(s)`（Python code point）−1 → 0 请求；按 UTF-8 字节计数则 FAIL |
| G5 | 先跑通 3 行；再 `max_rows=1` 重复同一 3 行 → 全 cache hit，mock 增量 0 |
| G6 | 禁止「G1 ERROR 传到客户端后再 `jev_cache_clear()` 续扫」：该 SQL 已结束，不是同一 statement。必须在**同一外部 QueryDesc** 内、**任何超限 ERROR 之前**调用 `jev_cache_clear()`，证明 spend reservation 在 clear 后仍属本 scope，随后同 scope 的新 demand 才超限。确定性有序表达式（`CASE` 短路；禁止 AND/OR、并列 tlist、volatile 重排；禁止 LATERAL/CTE/子查询/SQL 函数等嵌套 QueryDesc 计数归属假设）：`batch_size=1`，`max_rows_per_statement=1`，互异 cold-miss 常量 `$a`/`$b`，单条 `SELECT CASE WHEN jev_prob($a) IS NOT NULL THEN CASE WHEN jev_cache_clear() IS NOT NULL THEN jev_prob($b) END END`。`$a` 必须先返回（本 scope 已 commit 1 行 reservation）再进入 clear；随后 `$b` ERROR（文本含 would send），该 batch 不得 `QUEUED`；mock 请求数 **恰好 1**（不得因 `$b` 变成 2）；`errors` 不变。若 clear 重置 statement 计数则 `$b` 成功，本条 FAIL |
| G7 | `/flaky` 429 重试成功：`retries>=1`，`requests` 只计成功 batch 一次，spend 不因重试翻倍 |

实施 **默认做 G2，不做「必须先 pump 再 admission」**（那会改变延迟）。只有明确要测「合法 batch 已出站」时才开 G2b。

---

### D7. `TsConn` 状态、`CURLM` 重置、cancel fixture（Review P1-5）

**不变量：** 不用 `open_sockets - active_sockets` 冒充 idle。每个连接是扩展记录：

| 状态 | 进入 | 退出 |
|---|---|---|
| `OPENING` | opensocket 回调 | 握手完 / 失败 close |
| `ACTIVE` | easy 开始 transfer | 完成或 cancel |
| `IDLE` | transfer 成功且 keepalive>0 且 endpoint+credential 未变 | 复用→ACTIVE；到期/对端关→STALE |
| `STALE` | `now-last_use ≥ keepalive` 或 `alive==0` | close → `CLOSED` |
| `CLOSED` | closesocket | 从登记表删除 |

`jev_stats.connections` = 状态为 `IDLE` 的记录数。

**`CURLM` 重建（不只 `FORBID_REUSE`）** 当发生：`jev.keepalive=0`、`jev.api_url` 变、credential fingerprint 变、AsynchDNS 检查失败需要拒绝 live 路径。顺序：cancel ACTIVE → `curl_multi_cleanup` → 丢弃全部 `TsConn` → 新 `curl_multi_init`。`keepalive=0` 的下一次 live 调用走新 multi，无 IDLE。

Jev：`CURL_HTTP_VERSION_1_1`。编译期 libcurl ≥ 7.66。第一次 Jev live：`CURL_VERSION_ASYNCHDNS` 否则 `FEATURE_NOT_SUPPORTED`。

**Cancel：注入 stage handshake（`test_d7_cancel_stage_handshake`）；57014 ≠ ProcDie；恢复禁止 cache**

测试 GUC `jev._test_cancel_stage`：`''` / `dns` / `connect` / `response` / `retry_sleep`。握手目录复用 `jev._test_handshake_dir`。

**合成入口（门禁，必须）：** live **cache miss** 之后、`curl_multi_add_handle` **之前**，若 GUC 非空：

1. 设置 `jev_stats.last_cancel_stage` 为该名。
2. 把 `{backend_pid, stage}` 原子写入 `$handshake_dir/cancel-stage-ready.json`。
3. `WaitLatch` 直到 `$handshake_dir/cancel-stage.ack` 存在，或看到 `QueryCancelPending` / `ProcDiePending`，或 10s 超时（超时是测试失败）。
4. **不**依赖真实 DNS、connect、响应体、429、AsynchDNS、连接 cache、keepalive、`statement_timeout` 竞态。
5. `ProcDiePending` → `ProcessInterrupts()` / FATAL（`57P01` 等）。**禁止**改写成 `57014`。
6. `QueryCancelPending` → `ereport(ERROR, errcode(ERRCODE_QUERY_CANCELED) /* 57014 */)`。会话继续活着。

**Cold cache / miss（每个 stage 前）：** `jev_cache_clear()`；`keepalive=0`；mock URL；唯一问句。

**57014 门禁（禁止 `WHEN OTHERS`，禁止 `statement_timeout` 作为合成门禁）：**

```python
import json, os, time, urllib.request
hdir = os.environ["JEV_HANDSHAKE_DIR"]
ctrl = os.environ.copy()  # 第二条短连接只用来 pg_cancel_backend，不是第二段业务 psql 会话
for stage in ("dns", "connect", "response", "retry_sleep"):
    ready, ack = os.path.join(hdir, "cancel-stage-ready.json"), os.path.join(hdir, "cancel-stage.ack")
    for p in (ready, ack):
        if os.path.exists(p):
            os.unlink(p)
    sess.scalar("SELECT jev_cache_clear()")
    sess.scalar("SET jev.keepalive = 0")
    sess.scalar("SET jev._test_handshake_dir = '%s'" % hdir.replace("'", "''"))
    sess.scalar("SET jev._test_cancel_stage = '%s'" % stage)
    qtext = "the name is Tokyo d7 %s %s" % (stage, time.time_ns())
    hits = int(sess.scalar("SELECT (jev_stats()->>'cache_hits')::int"))
    sess.send("SELECT jev_prob(t, '%s') FROM jev_heap t WHERE id=1" % qtext.replace("'", "''"))
    for _ in range(200):
        if os.path.isfile(ready):
            break
        time.sleep(0.05)
    else:
        raise SystemExit("D7 stage-ready timeout: %s" % stage)
    info = json.loads(open(ready, encoding="utf-8").read())
    assert str(info["backend_pid"]) == sess.backend_pid, info
    assert info["stage"] == stage, info
    t0 = time.monotonic()
    q("SELECT pg_cancel_backend(%s)" % sess.backend_pid)
    open(ack, "w", encoding="utf-8").write("1\n")
    _out, st, err = sess.wait_frame(deadline_s=5.0)
    if st != "57014":
        raise SystemExit("D7 expected 57014 got %s %s" % (st, err))
    assert time.monotonic() - t0 <= 0.25
    assert sess.scalar("SELECT jev_stats()->>'last_cancel_stage'") == stage
    sess.assert_same_backend()
    sess.scalar("RESET jev._test_cancel_stage")
    sess.scalar("SELECT jev_cache_clear()")
    rec = "the name is Tokyo d7-recover %s %s" % (stage, time.time_ns())
    sess.scalar("SELECT jev_prob(t, '%s') FROM jev_heap t WHERE id=1" % rec.replace("'", "''"))
    assert int(sess.scalar("SELECT (jev_stats()->>'cache_hits')::int")) == 0
    sess.assert_same_backend()
```

`pg_cancel_backend` 走 **harness 的独立一次性 `q()`**，不是第二条 PersistentPsql 业务会话。合成门禁用 cancel 注入，不用 `statement_timeout` 竞态（timeout 只留在可选真实 `/stall` fixture）。

**ProcDie（`test_d7_procdie_not_57014`，一次，独立 handshake）：**

```python
import json, os, signal, time
ready, ack = os.path.join(hdir, "cancel-stage-ready.json"), os.path.join(hdir, "cancel-stage.ack")
for p in (ready, ack):
    if os.path.exists(p):
        os.unlink(p)
sess.scalar("SELECT jev_cache_clear()")
sess.scalar("SET jev.keepalive = 0")
sess.scalar("SET jev._test_handshake_dir = '%s'" % hdir.replace("'", "''"))
sess.scalar("SET jev._test_cancel_stage = 'dns'")
sess.send("SELECT jev_prob(t, 'the name is Tokyo procdie') FROM jev_heap t WHERE id=1")
for _ in range(200):
    if os.path.isfile(ready):
        break
    time.sleep(0.05)
else:
    raise SystemExit("ProcDie stage-ready timeout")
info = json.loads(open(ready, encoding="utf-8").read())
assert str(info["backend_pid"]) == sess.backend_pid
os.kill(int(sess.backend_pid), signal.SIGTERM)
open(ack, "w", encoding="utf-8").write("1\n")
try:
    sess.wait_frame(deadline_s=5.0)
    raise SystemExit("ProcDie must not return a normal frame")
except FrameError as exc:
    assert "57014" not in str(exc)
```

此 backend 已死，**不做** `assert_same_backend`，也 **不做** cache 恢复。另开新 `PersistentPsql()` 只证明集群仍能服务。`57014` 恢复只发生在仍活着的 cancel 会话上，且必须 cold miss。

**可选真实 fixture**（有则跑，没有不得 skip 掉合成门禁）：

- DNS：仅当 `CURL_VERSION_ASYNCHDNS` 且 `CURLOPT_DNS_SERVERS` 可指向本机 Python DNS。否则 **不得** 把 `.invalid` 当 cancel。
- connect：仅当存在会真正阻塞 SYN 的夹具。`127.0.0.1` 未监听端口是 **立即 RST**，不算。
- response：mock `/stall` + `statement_timeout` → 协议层 `57014`（仍走 framed `expect_sqlstate`，不是 `WHEN OTHERS`）。
- retry_sleep：`/429` + `Retry-After: 30` + timeout → `57014`。

**立即错误（单独断言，SQLSTATE ≠ 57014）**

| 夹具 | 期望 |
|---|---|
| `http://jev-cancel.invalid:9/v1/systemone` 若瞬时 NXDOMAIN | 连接/解析错误，不是 57014 |
| `127.0.0.1:$closed_port` 立即 RST | 连接错误，不是 57014 |

证据文件 `$LOGDIR/curl-features.txt`：`curl_version()` 与 `AsynchDNS=1`。

**断言 C1–C3：** 扫描后 `connections` 为 IDLE 计数；mock `accepted_connections < requests`；keepalive 过期后 accept 增加。

---

### D8. Config-before-cache 与 last_request 隔离（Review P1-6）

**不变量（有意偏离 pg-jev）：** pg-jev 在 `load_cfg()` 之前用 `[rel_type, query, kind, opts]` 查 cache，**不含** model/url/key。Native 每次公开入口：

1. 解析当前 `jev.*`（含 key 链）→ `JevCfg`
2. 校验 endpoint 策略
3. cache key = canonical(user, typid, typmod, relid, query, kind, options, **model**, **api_url**, **credential fingerprint**)
4. 再 lookup

缺 key：即使以前有 cache，live 路径仍 ERROR（stats 可读）。fingerprint = SHA-256(backend_nonce || key)，nonce 在 session 内随机，**禁止**写入日志/NOTICE/错误/`jev_stats`。

model/url/key 变化：新 identity → miss；`jev_curl_generation++` 并 cancel 旧 job；若 url 或 credential 变，按 D7 重建 `CURLM`。迟到完成不得写入新 cache。

**last_request：** `save_last_request` **仅** `typesafe_*` 的 `execute_request` / `http_post_many`。Jev `ts_http_submit(..., TS_FAMILY_JEV)` 不得调用它。共享 `curl_multi` 可以，共享「last body 全局」不行。

**断言 L1：** `SELECT typesafe_last_request()` 非空后跑 `jev_prob`；`typesafe_last_request()` 不变。  
**断言 L2：** 改 `jev.model` / `jev.api_url` / `jev.api_key` 后下一 miss 发请求。  
**断言 L3：** mixed 401：`typesafe_*` 错误仍含截断 body；`jev_*` 不含 provider body、不含 key。

---

### D8b. Relation / RLS / FDW fixture（Review P1-8）

文件：`test/jev/sql/04_relations.sql` + `expected/04_relations.out`。同一 D3 cluster。`postgres_fdw.control` 已在 harness 启动前断言存在。禁止 `skip_all`。

远端库由 D3 在 R2 前创建（`jev_fdw_remote.jev_remote` 两行 Berlin/Tokyo）。`FDW_PORT`/`USER` 由 launcher `-v` 注入。

**`04_relations.sql` 原样（文件头 GUC 见 D3；此处从 CREATE EXTENSION 之后写起）：**

```sql
CREATE EXTENSION IF NOT EXISTS typesafe;
CREATE EXTENSION postgres_fdw;

CREATE TABLE jev_heap (id int PRIMARY KEY, name text);
INSERT INTO jev_heap VALUES (1,'Berlin'),(2,'Tokyo');
CREATE MATERIALIZED VIEW jev_mv AS SELECT * FROM jev_heap;
CREATE VIEW jev_v AS SELECT * FROM jev_heap;
CREATE TABLE jev_part (id int, name text) PARTITION BY RANGE (id);
CREATE TABLE jev_part_a PARTITION OF jev_part FOR VALUES FROM (0) TO (10);
INSERT INTO jev_part VALUES (1,'Berlin'),(2,'Tokyo');

CREATE SERVER jev_loopback FOREIGN DATA WRAPPER postgres_fdw
  OPTIONS (host '127.0.0.1', port :'FDW_PORT', dbname 'jev_fdw_remote');
CREATE USER MAPPING FOR CURRENT_USER SERVER jev_loopback
  OPTIONS (user :'USER', password_required 'false');
CREATE FOREIGN TABLE jev_ft (id int, name text)
  SERVER jev_loopback OPTIONS (schema_name 'public', table_name 'jev_remote');

CREATE TABLE jev_rls (id int, name text, owner name);
INSERT INTO jev_rls VALUES (1,'Berlin','jev_rls_role'),(2,'secret','no_one');
ALTER TABLE jev_rls ENABLE ROW LEVEL SECURITY;
CREATE POLICY jev_rls_p ON jev_rls FOR SELECT USING (owner = CURRENT_USER);
CREATE ROLE jev_rls_role LOGIN;
GRANT USAGE ON SCHEMA public TO jev_rls_role;
GRANT SELECT ON jev_rls TO jev_rls_role;
GRANT EXECUTE ON FUNCTION jev_prob(anyelement, text) TO jev_rls_role;

-- 调用矩阵：mock last-word = Tokyo → 0.9，否则 0.1
SELECT name, jev_prob(jev_heap, 'the name is Tokyo') AS p FROM jev_heap ORDER BY id;
SELECT name, jev_prob(jev_mv,   'the name is Tokyo') AS p FROM jev_mv   ORDER BY id;
SELECT name, jev_prob(jev_v,    'the name is Tokyo') AS p FROM jev_v    ORDER BY id;
SELECT name, jev_prob(jev_part, 'the name is Tokyo') AS p FROM jev_part ORDER BY id;
SELECT name, jev_prob(jev_ft,   'the name is Tokyo') AS p FROM jev_ft   ORDER BY id;
SELECT s.name, jev_prob(s, 'the name is Tokyo') AS p
  FROM (SELECT name FROM jev_heap) s ORDER BY 1;

SET ROLE jev_rls_role;
SELECT name, jev_prob(jev_rls, 'the name is Tokyo') AS p FROM jev_rls ORDER BY id;
RESET ROLE;

-- U1：named composite / relkind 'c'，内部扫描入口必须 0A000
CREATE TYPE jev_u1_comp AS (id int, name text);
DO $$
DECLARE
  st text;
  msg text;
BEGIN
  BEGIN
    PERFORM jev_prob(ROW(1,'Berlin')::jev_u1_comp, 'the name is Tokyo');
    RAISE EXCEPTION 'U1 expected 0A000, query succeeded';
  EXCEPTION
    WHEN feature_not_supported THEN
      GET STACKED DIAGNOSTICS st = RETURNED_SQLSTATE, msg = MESSAGE_TEXT;
      IF st IS DISTINCT FROM '0A000' THEN
        RAISE EXCEPTION 'U1 expected SQLSTATE 0A000, got %', st;
      END IF;
      IF position('relkind' in lower(msg)) = 0 THEN
        RAISE EXCEPTION 'U1 message must mention relkind: %', msg;
      END IF;
    WHEN OTHERS THEN
      GET STACKED DIAGNOSTICS st = RETURNED_SQLSTATE, msg = MESSAGE_TEXT;
      RAISE EXCEPTION 'U1 expected 0A000, got % %', st, msg;
  END;
END $$;

CREATE SEQUENCE jev_seq;
DO $$
DECLARE
  st text;
BEGIN
  BEGIN
    PERFORM jev_prob(s, 'the name is Tokyo') FROM jev_seq s;
    RAISE EXCEPTION 'U1-S expected 0A000, query succeeded';
  EXCEPTION
    WHEN feature_not_supported THEN
      GET STACKED DIAGNOSTICS st = RETURNED_SQLSTATE;
      IF st IS DISTINCT FROM '0A000' THEN
        RAISE EXCEPTION 'U1-S expected 0A000, got %', st;
      END IF;
    WHEN OTHERS THEN
      GET STACKED DIAGNOSTICS st = RETURNED_SQLSTATE;
      RAISE EXCEPTION 'U1-S expected 0A000, got %', st;
  END;
END $$;

DROP FOREIGN TABLE IF EXISTS jev_ft;
DROP USER MAPPING IF EXISTS FOR CURRENT_USER SERVER jev_loopback;
DROP SERVER IF EXISTS jev_loopback;
DROP EXTENSION IF EXISTS postgres_fdw;
```

**`expected/04_relations.out` 调用矩阵（数值行；NOTICE/CREATE 按 pg_regress 实际输出）：**

```
 name  |  p
-------+-----
 Berlin| 0.1
 Tokyo | 0.9
```

heap/mv/view/part/ft 各一份（列名相同）。anonymous：

```
 name  |  p
-------+-----
 Berlin| 0.1
 Tokyo | 0.9
```

RLS（只见 Berlin）：

```
 name  |  p
-------+-----
 Berlin| 0.1
```

U1：DO 成功（无 result set）。U1 是能进入 `jev_eval_core` / `jev_relkind_error` 的内部 unsupported relkind 夹具（named composite `relkind='c'` → `0A000`）。

U1-S：`PERFORM jev_prob(s, …) FROM jev_seq s` 在 parse analysis 的 `makeWholeRowVar` 即被核心拒绝为 **42809**（`reltype=0`，sequence 没有 composite type）。toast/index 同样 `reltype=0`。禁止用全局 `planner_hook` / `post_parse_analyze_hook` 把核心 42809 改写成 0A000。U1-S 断言 42809，且 `feature_not_supported`（0A000）视为失败。Oracle review 确认此计划/测试修正。

**R3 Python 补断言（R2 之后，读 `MOCK_STATS_URL`）：**

- `bodies` 中至少各有一条 `state.rows` 含 `Berlin`/`Tokyo` 的请求，证明 heap/mv/view/part/ft/anonymous 走了 mock（可用 `requests` 在 R2 前后的增量 ≥ 5）。
- `json.dumps(body).lower()` 在 **SET ROLE jev_rls_role 那段之后新增的 bodies** 中不得出现 `"secret"`。
- 非复合：`SELECT jev_prob(1, 'x')` 必须是普通 SQL 错误（42883/22703 等），**不是** `0A000`。`0A000` 只用于已解析到 `pg_class` 且 `relkind ∉ {r,m,v,p,f}`。

R2 结束后 harness：`psql -d postgres -c 'DROP DATABASE jev_fdw_remote'`。

---

### D9. pginstall 清单、allowlist、import 预检（Review P1-9）

**已核实不变量（2026-10-03 23:12，禁止修 `__init__.py`）：**

| 项 | 实测 |
|---|---|
| `$PGROOT/share/pgembed` | **存在**，`type=dir`，`mode=0755`，非 symlink |
| `$PGROOT/share/pgembed/build-metadata.json` | **存在**，mode `0600`，**无** `pg_textsearch`；`pg_typesafe.version=0.0.1` |
| generator `EXTENSIONS` 含 `pg_textsearch` / stannum `0.5.0` | **是**（相对 HEAD 的已有 hunk，必须保护） |
| LIVE `stannum.version` | `0.1.0` / commit `e163585c…`（与 generator WIP `0.5.0` 不同；`live-existing` 必须保留 LIVE 值） |
| `EXTENSION_PACKAGES` 含 `pg_textsearch` | **是** |
| `EXTENSION_SO_FILES` 含 `pg_textsearch` | **否**（KeyError 风险，见 §0.1.4；此缺失是已有 hunk 的一部分，禁止补上） |
| `.venv` `import pgembed` | **成功** |
| `has_extension("pg_typesafe")` | **True** |
| `has_extension("pg_textsearch")` | **False** |
| 直接 `_legacy_artifacts_exist("pg_textsearch")` | **KeyError** |

当前 pgembed porcelain（WIP 基线，**必须按此钉住**；`git status --porcelain -uall`）：

```text
 M pgbuild/Makefile
 M src/pgembed/__init__.py
 M tools/generate_bundle_metadata.py
?? benchmarks/stannum-vs-pg-textsearch/REPORT.md
?? benchmarks/stannum-vs-pg-textsearch/query_path_probe.py
?? benchmarks/stannum-vs-pg-textsearch/results.json
?? dist-standalone/pgembed_stannum-0.3.0rc2-py3-none-macosx_11_0_arm64.whl
?? standalone-wheelhouse/pgembed_stannum-0.3.0rc2-py3-none-macosx_11_0_arm64.whl
?? tests/test_stannum_vs_pg_textsearch.py
?? tools/bench_stannum_vs_pg_textsearch.py
```

WI-3 启动时把下列字节拷进 `$JEV_EVIDENCE` 并在 post 校验：

| 文件 | SHA-256（2026-10-03 23:12） | post |
|---|---|---|
| `src/pgembed/__init__.py` | `5fed1b1981e39e626dfedc0731bdc0267c6a8947b9398fa248f1d6b729d33c81` | **整文件相等** |
| `pgbuild/Makefile` | `05cef9f643204fc609c4c64ff5910aa23be09cf5f33db417f13ee2d66e151358` | **整文件相等** |
| `tools/generate_bundle_metadata.py` | `5425f4da929aac0e94f0fa9a0fea5ef57c69777165af0c8857cbf987e5c01065` | hash **可以**变，但下方受保护 hunk **字节相等** |

受保护的 generator hunk（只许在文件**其它位置**新增 overlay flags/入口）：

- `EXTENSIONS["stannum"]`：`version="0.5.0"`，`source_commit="d895c8e98d89bcb40dcf374799f2e58df801c7ae"`
- `EXTENSIONS["pg_textsearch"]` 整块：`version="1.5.1"`，`source_commit="fa142d049d08eac45a263f5abc250c8877ae076e"`，`requires_preload=True`
- `__init__.py` 已有的 `EXTENSION_PACKAGES["pg_textsearch"]=None` 与 `get_extension_create_name` 映射；**不得**给 `EXTENSION_SO_FILES` 加该键
- `pgbuild/Makefile` 已有的 `PG_TEXTSEARCH_*` 配方与 `.PHONY` 行

实施：`cp` 三份到 `$JEV_EVIDENCE/*.pre`；post 用 `cmp` 钉 `__init__.py` 与 `pgbuild/Makefile`；`scripts/pgembed-protect-generator-hunks.py` 从 pre/post 各抽出 `stannum`/`pg_textsearch` 的 `dict(`…`)` 块做字面相等。

**预检（WI-3 第一步；不要断言 overlay 前 `has_extension("pg_typesafe")`）：**

```sh
/Users/wxl/Projects/pgembed/.venv/bin/python - <<'PY'
import pgembed
assert pgembed.has_extension("pg_textsearch") is False
print("import_ok", "pg_typesafe", pgembed.has_extension("pg_typesafe"))
PY
```

Overlay **后**：

```sh
/Users/wxl/Projects/pgembed/.venv/bin/python -c \
  'import pgembed; assert pgembed.has_extension("pg_typesafe") is True; assert pgembed.has_extension("pg_textsearch") is False'
```

因模块在 import 时已 `_detect_extensions()`，post 检查必须是 **新 Python 进程**（不要只 `reload` 而不清全局）。上式用新 `-c` 进程即可。

**Source manifest：** D1 的 `canonical_source_sha256.py`。

**WIP manifest（`scripts/pgembed-wip-manifest.py`）**

```python
#!/usr/bin/env python3
"""SHA-256 WIP files. argv: pgembed_root out.jsonl"""
import hashlib, json, os, subprocess, sys
from pathlib import Path
root = Path(sys.argv[1]).resolve()
out = open(sys.argv[2], "w", encoding="utf-8")
porc = subprocess.check_output(["git", "-C", str(root), "status", "--porcelain", "-uall"], text=True)
paths = []
for line in porc.splitlines():
    # "XY path" or "XY orig -> path"
    rel = line[3:]
    if " -> " in rel:
        rel = rel.split(" -> ", 1)[1]
    paths.append(rel)
# always pin these even if clean
for extra in ("pgbuild/Makefile", "src/pgembed/__init__.py",
              "tools/generate_bundle_metadata.py",
              "pgbuild/.postgres-bundle-config.stamp"):
    if extra not in paths:
        paths.append(extra)
def emit(rel, kind, sha, target=None, mode=None):
    rec = {"relpath": rel, "type": kind, "sha256": sha, "target": target,
           "mode": mode}
    out.write(json.dumps(rec, sort_keys=True) + "\n")
seen = set()
for rel in paths:
    p = root / rel
    if rel in seen:
        continue
    seen.add(rel)
    mode = oct(p.lstat().st_mode & 0o777) if p.exists() or p.is_symlink() else None
    if p.is_symlink():
        emit(rel, "symlink", None, os.readlink(p), mode)
    elif p.is_file():
        emit(rel, "file", hashlib.sha256(p.read_bytes()).hexdigest(), None, mode)
    elif p.is_dir():
        emit(rel, "dir", None, None, mode)
        for q in sorted(p.rglob("*")):
            r2 = q.relative_to(root).as_posix()
            if r2 in seen:
                continue
            seen.add(r2)
            m2 = oct(q.lstat().st_mode & 0o777)
            if q.is_symlink():
                emit(r2, "symlink", None, os.readlink(q), m2)
            elif q.is_file():
                emit(r2, "file", hashlib.sha256(q.read_bytes()).hexdigest(), None, m2)
            elif q.is_dir():
                emit(r2, "dir", None, None, m2)
```

Pre/post：除 `tools/generate_bundle_metadata.py` 外，WIP **文件级**记录必须整对象相等。另外必须跑 **hunk 清单**（P1-9），不能只用整文件 hash / porcelain 冒充 diff 保护。

**WIP hunk 清单（`test_d9_wip_hunk_manifest`；`scripts/pgembed-wip-hunk-manifest.py`）**

对每个 tracked dirty 文件跑 `git -C pgembed diff -U0 -- <relpath>`，按 `@@` 切开。比较身份 **只** 是 `(relpath, body_sha256)`。完整 hunk sha256（含 `@@` header）**只写入证据，不参与 pass/fail**。

**Body 规范化（确定性；manifest 与 diff 必须同一函数）：**

```python
import hashlib

def iter_hunks(diff: str):
    """Split `git diff -U0 -- <file>` stdout. File headers ignored."""
    lines = diff.replace("\r\n", "\n").replace("\r", "\n").splitlines(keepends=True)
    i = 0
    while i < len(lines) and not lines[i].startswith("@@"):
        i += 1
    while i < len(lines):
        header = lines[i]
        i += 1
        body_parts = []
        while i < len(lines) and not lines[i].startswith("@@"):
            if lines[i].startswith("diff --git"):
                break
            body_parts.append(lines[i])
            i += 1
        yield header, "".join(body_parts)

def normalize_body(body: str) -> bytes:
    # 去掉 @@ header 后的剩余字节；换行统一 LF；保留 "\\ No newline at end of file" 行
    return body.replace("\r\n", "\n").replace("\r", "\n").encode("utf-8")

def body_sha256(body: str) -> str:
    return hashlib.sha256(normalize_body(body)).hexdigest()

def full_hunk_sha256(header: str, body: str) -> str:
    text = (header + body).replace("\r\n", "\n").replace("\r", "\n")
    return hashlib.sha256(text.encode("utf-8")).hexdigest()
```

JSONL 每行：`{relpath, header, sha256, body_sha256}`。`sha256` = `full_hunk_sha256`（证据）。身份键 = `(relpath, body_sha256)`，按 **multiset** 比较。

**2026-10-03 实测基线（header / 完整 sha256[:16] 只供人读与证据；WI-3 以当时 `body_sha256` 为准，不得再要求完整 sha256 相等）：**

| relpath | hunk header | sha256[:16]（证据） |
|---|---|---|
| `pgbuild/Makefile` | `@@ -113,0 +114,10 @@ PSQL_BM25S_RELOCATE_ICU := ...` | `c70d3f8cb71d9c72` |
| `pgbuild/Makefile` | `@@ -286 +296 @@ STANNUM_PGRX_HOME := ...` | `c3302bc3e5eab3fb` |
| `pgbuild/Makefile` | `@@ -345,0 +356 @@ $(POSTGRES_BUNDLE_CONFIG_STAMP): FORCE` | `bc3dcc0643bd1c0f` |
| `pgbuild/Makefile` | `@@ -361 +372 @@ $(POSTGRES_BUNDLE_CONFIG_STAMP): FORCE` | `9c381f7275e77f27` |
| `pgbuild/Makefile` | `@@ -379,0 +391 @@ pgmq: postgres ...` | `c7a23703a002fb01` |
| `pgbuild/Makefile` | `@@ -503,0 +516,14 @@ endif` | `eb265bbb609150b0` |
| `src/pgembed/__init__.py` | `@@ -61,0 +62 @@ EXTENSION_PACKAGES = {` | `233a59402e60df1d` |
| `src/pgembed/__init__.py` | `@@ -254,0 +256 @@ def get_extension_create_name...` | `abdbebca89dded5d` |
| `tools/generate_bundle_metadata.py` | `@@ -101,3 +101,3 @@ EXTENSIONS: dict...` | `69c94cb7da785b9a` |
| `tools/generate_bundle_metadata.py` | `@@ -109,0 +110,10 @@ EXTENSIONS: dict...` | `afa1f77d7b67cea7` |

规则：

1. WI-3 启动时把上表对应 hunk 写入 `$JEV_EVIDENCE/wip.hunks.pre.jsonl`（每行含完整 `sha256` **与** `body_sha256`）。表中 [:16] 只供人读，**不是**比较键。
2. `pgbuild/Makefile` 与 `src/pgembed/__init__.py`：post 与 pre 的身份 multiset `{(relpath, body_sha256)}` **完全相同**。零新身份。header / 完整 `sha256` 可变（行号漂移）。整文件 `cmp` 仍要过。
3. `tools/generate_bundle_metadata.py`：pre 的每条身份必须仍出现在 post（`pre_identities ⊆ post_identities`）。只允许 **新增** overlay 入口身份：规范化 body 含一行 `def <name>(`，`<name>` 限于 `freeze_overlay_mode` / `read_frozen_mode` / `ensure_share_pgembed` / `reconstruct_pg_typesafe` / `atomic_write` / `build_pre` / `run_overlay` / `local_overlay_cli`。新增 hunk 不得改 `EXTENSIONS["stannum"]` / `EXTENSIONS["pg_textsearch"]` 正文（受保护身份的 `body_sha256` 必须原样出现）。
4. Untracked 文件：pre/post sha256 相等，禁止新增 untracked。
5. **拆分 / 合并（可判定，禁止模糊子串）：**
   - **拆分：** 一条 pre 身份在 post 中消失，换成多条不同 `body_sha256` → FAIL（受保护身份缺失）。
   - **合并：** 多条 pre 身份消失，出现一条拼接后的新 body → FAIL（受保护身份缺失；generator 新增身份不得用合并来「吞掉」受保护 body）。
   - **仅 header/行号漂移：** `body_sha256` 不变 → PASS。完整 hunk hash 不同只记证据。
   - **禁止** 用「post body 是 pre body 的前后缀 / 子串 / 拼接」放行拆分或合并。
6. `scripts/pgembed-protect-generator-hunks.py` 仍抽 `stannum`/`pg_textsearch` dict 块字面相等，作为 hunk 清单的补充，不能替代 hunk 清单。

`scripts/pgembed-wip-hunk-manifest.py`（stdout JSONL）：对 `git diff -U0 -- <file>` 的每个 `@@` hunk 写 `{relpath, header, sha256, body_sha256}`。`scripts/pgembed-wip-hunk-diff.py pre post`：Makefile/`__init__.py` 的 `(relpath, body_sha256)` multiset 相等；generator 的 pre 身份 ⊆ post，且新增身份的 body 含 overlay 入口 `def <name>(`。**不比较**完整 `sha256`。缺失的 pre 身份必须在 stderr 点名（拆分/合并因此失败）。


**Install-tree manifest（`scripts/pgembed-pginstall-manifest.py`）**

```python
#!/usr/bin/env python3
"""Walk PGROOT; JSONL to stdout. Does not follow symlinks. Records dirs."""
import hashlib, json, os, sys
from pathlib import Path
root = Path(sys.argv[1]).resolve()
recs = []
for dirpath, dirnames, filenames in os.walk(root, followlinks=False):
    # 目录 symlink：记录 type=symlink，不下降
    keep = []
    for d in dirnames:
        p = Path(dirpath) / d
        rel = p.relative_to(root).as_posix()
        st = p.lstat()
        if p.is_symlink():
            recs.append({"relpath": rel, "type": "symlink",
                         "sha256": None, "target": os.readlink(p),
                         "mode": oct(st.st_mode & 0o777)})
        else:
            recs.append({"relpath": rel, "type": "dir",
                         "sha256": None, "target": None,
                         "mode": oct(st.st_mode & 0o777)})
            keep.append(d)
    dirnames[:] = keep
    for name in filenames:
        p = Path(dirpath) / name
        rel = p.relative_to(root).as_posix()
        st = p.lstat()
        if p.is_symlink():
            recs.append({"relpath": rel, "type": "symlink",
                         "sha256": None, "target": os.readlink(p),
                         "mode": oct(st.st_mode & 0o777)})
        elif p.is_file():
            recs.append({"relpath": rel, "type": "file",
                         "sha256": hashlib.sha256(p.read_bytes()).hexdigest(),
                         "target": None,
                         "mode": oct(st.st_mode & 0o777)})
for rec in sorted(recs, key=lambda r: r["relpath"]):
    sys.stdout.write(json.dumps(rec, sort_keys=True) + "\n")
```

**Allowlist（`scripts/pgembed-allowlist-diff.py`；`test_d9_parent_dir_allowlist`）**

允许 **内容变化或新增** 的 **文件** relpath 仅：

```
lib/postgresql/typesafe.dylib
lib/postgresql/typesafe.so
share/postgresql/extension/typesafe.control
share/postgresql/extension/typesafe--0.0.1.sql
share/postgresql/extension/typesafe--0.1.0.sql
share/postgresql/extension/typesafe--0.0.1--0.1.0.sql
share/pgembed/build-metadata.json
```

允许 **新建或保持** 的 **目录** relpath 仅（父目录 allowlist）：

```
share/pgembed
```

`share/pgembed`：pre 已有则 post 必须 `type=dir`、`mode=0o755`、`target=null`、非 symlink；pre 没有则 post 可新增且必须 `dir/0755`。任何其它新目录 → exit 1。

**父目录规则（每个 post 条目都要跑）：**

设 `parents(relpath)` = 所有严格前缀目录（`a/b/c` → `a`、`a/b`）。

1. post 删除了 pre 中的 relpath → exit 1
2. post 新增文件：该文件必须在文件 allowlist；其每个 parent 必须 (a) 已在 pre 且 type/mode/target 不变，或 (b) 该 parent 本身在目录 allowlist 且 post 记录合法
3. post 新增目录：该目录必须在目录 allowlist；其 parent 同样适用规则 2
4. 同 relpath 字段变化：仅当该 relpath 在对应 allowlist（文件可变 hash；目录只允许上面的 share/pgembed 规则）
5. 不得出现「文件在 allowlist 但偷偷建了未登记的中间目录」（例如 `lib/postgresql/jev/typesafe.dylib`）

Darwin codesign 只许改同一 `typesafe.dylib`。比较全部字段：`relpath`、`type`、`sha256`、`target`、`mode`。

底稿/pre 对象 **只** 写 `$JEV_EVIDENCE`。禁止 `cp LIVE $PGROOT/.../LIVE.pre-*`。O1 `--pre` 必须是 `$JEV_EVIDENCE/build-metadata.pre.json`。

**`scripts/pgembed-protect-generator-hunks.py`（可执行）：**

```python
#!/usr/bin/env python3
"""Assert stannum/pg_textsearch dict blocks are unchanged. argv: generate.pre.py generate.post.py"""
import re, sys
from pathlib import Path

def block(src: str, name: str) -> str:
    m = re.search(r'"%s": dict\((.*?)\n    \),' % name, src, re.S)
    if not m:
        raise SystemExit("missing EXTENSIONS %s block" % name)
    return m.group(0)

pre, post = Path(sys.argv[1]).read_text(), Path(sys.argv[2]).read_text()
assert block(pre, "stannum") == block(post, "stannum")
assert block(pre, "pg_textsearch") == block(post, "pg_textsearch")
```

---


### D10 / D11（保持）

共存：不因 `pg_extension='jev'` 拒绝安装；另一模块已定义非 placeholder 的 `jev.api_key` 则 `_PG_init` ERROR。  
错误文本：Jev redact body（security delta vs pg-jev 02_errors）；legacy 保持截断 body。

---

## 2. 三个 WI（范围不扩大）

**WI-1** C 引擎 + 共享 transport + D4/D6/D7/D8 合同。文件仍为 `typesafe_http.*`、`typesafe_jev.*`、`typesafe_jev_scan.c`。Makefile 增加 `TYPESAFE_CURL_LIBS` 分支。

**WI-2** SQL 0.1.0、upgrade、`test/jev/sql|expected|mock_api.py|run_python_suite.py`、`04_relations.sql`、README delta。`REGRESS` 仍只有 `typesafe`。

**WI-3** `install-into-pgembed.sh`、`pgembed-jev-check.sh`、manifest 脚本、generator overlay。禁止 stamp/`Makefile`/`__init__.py`/untracked WIP。

---

## 3. Parity matrix（增量修订）

函数（与 oracle-plan 201920 一致，本文件不再写「同前稿」）：

`jev` / `jev_prob` / `jev_choice` / `jev_score` / `jev_score_norm` / `jev_confidence` / `jev_eval` / `jev_stats` / `jev_cache_clear` / `jev_version`（返回 `0.2.0`；`pg_extension.extversion` 为 `0.1.0`）。

`jev_stats` 字段：`requests`、`input_tokens`、`output_tokens`、`rows_evaluated`、`cache_hits`、`api_ms`、`errors`、`retries`、`batches`（保留 0）、`estimated_cost_usd`、`cached_answers`、`in_flight`、`connections`（IDLE `TsConn`）、`session_bytes`、`cancelled_requests`、`last_cancel_stage`（D7；无 cancel 时 `""`）。

GUC 以 D5 表为准（每行都是 delta 或 parity）。功能矩阵中：

- Cache：**config-before-lookup**（delta vs pg-jev lookup 顺序；功能上切换 model/url/key 必 miss）。
- Spend G2：∈{0,1}，违规 batch 不出现。
- `connections`：IDLE `TsConn` 计数。
- FDW/partition/RLS：`04_relations`，无 skip。
- last_request：L1。

---

## 4. Key commands（WI-3）

```bash
JEV_EVIDENCE=/tmp/jev-overlay-evidence-$$
export JEV_EVIDENCE
PGROOT=/Users/wxl/Projects/pgembed/src/pgembed/pginstall
SRC=/Users/wxl/Projects/pg_typesafe
PGEMBED=/Users/wxl/Projects/pgembed
STAMP=$PGEMBED/pgbuild/.postgres-bundle-config.stamp
LIVE=$PGROOT/share/pgembed/build-metadata.json
# fail-fast unique evidence：必须新建空目录；已存在即停
mkdir "$JEV_EVIDENCE"
# JEV_EVIDENCE 必须在 PGROOT 外
case "$JEV_EVIDENCE" in "$PGROOT"/*) echo "evidence inside PGROOT" >&2; exit 1 ;; esac

# 0. 预检 import（当前 LIVE 已存在：只要求 import 成功、pg_textsearch False；把 pg_typesafe 快照下来）
/Users/wxl/Projects/pgembed/.venv/bin/python - <<'PY'
import pgembed
assert pgembed.has_extension("pg_textsearch") is False
print("import_ok", pgembed.has_extension("pg_typesafe"))
PY
/Users/wxl/Projects/pgembed/.venv/bin/python -c 'import pgembed; print(pgembed.has_extension("pg_typesafe"))' \
  | tee "$JEV_EVIDENCE/has_extension.pre.txt"

# 0b. 冻结 WIP 字节与 overlay 模式（必须在 O2 之前；禁止事后按 LIVE 是否存在改模式）
cp "$PGEMBED/src/pgembed/__init__.py" "$JEV_EVIDENCE/init.pre.py"
cp "$PGEMBED/pgbuild/Makefile" "$JEV_EVIDENCE/pgbuild.Makefile.pre"
cp "$PGEMBED/tools/generate_bundle_metadata.py" "$JEV_EVIDENCE/generate.pre.py"
shasum -a 256 "$JEV_EVIDENCE/init.pre.py" "$JEV_EVIDENCE/pgbuild.Makefile.pre" "$JEV_EVIDENCE/generate.pre.py" \
  | tee "$JEV_EVIDENCE/wip.sha256.pre"
git -C "$PGEMBED" status --porcelain -uall | tee "$JEV_EVIDENCE/pgembed.porcelain.pre"
python3 "$PGEMBED/tools/generate_bundle_metadata.py" \
  --local-overlay --freeze-mode \
  --install-prefix "$PGROOT"

# 1. source / WIP / install manifests（pre）
python3 "$SRC/scripts/canonical_source_sha256.py" "$SRC" | tee "$JEV_EVIDENCE/source.sha256"
python3 "$SRC/scripts/pgembed-wip-manifest.py" "$PGEMBED" "$JEV_EVIDENCE/wip.pre.jsonl"
python3 "$SRC/scripts/pgembed-wip-hunk-manifest.py" "$PGEMBED" > "$JEV_EVIDENCE/wip.hunks.pre.jsonl"
python3 "$SRC/scripts/pgembed-pginstall-manifest.py" "$PGROOT" > "$JEV_EVIDENCE/pginstall.pre.jsonl"
cp "$STAMP" "$JEV_EVIDENCE/pgembed.stamp.pre"
# 禁止：把 LIVE 直接 cp 成 build-metadata.pre.json。pre 由 overlay 入口的 build_pre() 写入。

# 2. overlay install（D2：Bash、trap、RPATH、构建 vs runtime curl、同一 PID 上 vmmap 主证据 / lsof 附录）
"$SRC/scripts/install-into-pgembed.sh" --stamp "$STAMP" --source "$SRC"

# 3. metadata overlay：先 O2（只写 $JEV_EVIDENCE/o2-work），再真实写盘。模式文件已冻结。
# 禁止 mkdir o2-work（由 run_overlay O_EXCL 创建）。禁止 proof.py 写盘。
o2_rc=0
JEV_OVERLAY_CRASH_BEFORE_RENAME=1 python3 "$PGEMBED/tools/generate_bundle_metadata.py" \
  --local-overlay \
  --install-prefix "$PGROOT" \
  --pg-typesafe-source-dir "$SRC" \
  --stamp "$STAMP" \
  --output "$JEV_EVIDENCE/o2-work/share/pgembed/build-metadata.json" || o2_rc=$?
test "$o2_rc" -eq 91
unset JEV_OVERLAY_CRASH_BEFORE_RENAME
# 真实 LIVE 必须仍等于 live-initial（或仍不存在）
python3 - "$LIVE" "$JEV_EVIDENCE" <<'PY'
from pathlib import Path
import sys
live, evidence = Path(sys.argv[1]), Path(sys.argv[2])
mode = (evidence / "overlay.mode").read_text(encoding="utf-8").strip()
initial = evidence / "build-metadata.live-initial.json"
if mode == "live-existing":
    assert live.read_bytes() == initial.read_bytes()
else:
    assert not live.exists()
print("o2_did_not_mutate_real_live")
PY
python3 "$PGEMBED/tools/generate_bundle_metadata.py" \
  --local-overlay \
  --install-prefix "$PGROOT" \
  --pg-typesafe-source-dir "$SRC" \
  --stamp "$STAMP"
# 上式走同一 overlay 入口：overlay.mode 已存在则不得重选；pre 已存在则不得 rebuild。禁止 main() unlink。禁止 --output。
python3 "$SRC/scripts/pgembed-metadata-overlay-proof.py" \
  --live "$LIVE" --canonical "$(cat "$JEV_EVIDENCE/source.sha256")" \
  --pre "$JEV_EVIDENCE/build-metadata.pre.json" \
  --mode-file "$JEV_EVIDENCE/overlay.mode"
/Users/wxl/Projects/pgembed/.venv/bin/python -c \
  'import pgembed; assert pgembed.has_extension("pg_typesafe") is True; assert pgembed.has_extension("pg_textsearch") is False'

# 4. 唯一测试入口（D3 harness + mock 协议 + framed PersistentPsql）
KEEP_JEV_LOGS=1 "$SRC/scripts/pgembed-jev-check.sh"

# 5. allowlist + WIP 保护 + stamp
python3 "$SRC/scripts/pgembed-pginstall-manifest.py" "$PGROOT" > "$JEV_EVIDENCE/pginstall.post.jsonl"
python3 "$SRC/scripts/pgembed-allowlist-diff.py" \
  "$JEV_EVIDENCE/pginstall.pre.jsonl" "$JEV_EVIDENCE/pginstall.post.jsonl"
python3 "$SRC/scripts/pgembed-wip-manifest.py" "$PGEMBED" "$JEV_EVIDENCE/wip.post.jsonl"
python3 - "$JEV_EVIDENCE/wip.pre.jsonl" "$JEV_EVIDENCE/wip.post.jsonl" <<'PY'
import json, sys
pre = {json.loads(l)["relpath"]: json.loads(l) for l in open(sys.argv[1])}
post = {json.loads(l)["relpath"]: json.loads(l) for l in open(sys.argv[2])}
allow_src = {"tools/generate_bundle_metadata.py"}
assert set(pre) == set(post)
for k, rec in pre.items():
    if k in allow_src:
        continue
    assert rec == post[k], k
PY
cmp "$JEV_EVIDENCE/init.pre.py" "$PGEMBED/src/pgembed/__init__.py"
cmp "$JEV_EVIDENCE/pgbuild.Makefile.pre" "$PGEMBED/pgbuild/Makefile"
python3 "$SRC/scripts/pgembed-protect-generator-hunks.py" \
  "$JEV_EVIDENCE/generate.pre.py" "$PGEMBED/tools/generate_bundle_metadata.py"
python3 "$SRC/scripts/pgembed-wip-hunk-manifest.py" "$PGEMBED" > "$JEV_EVIDENCE/wip.hunks.post.jsonl"
python3 "$SRC/scripts/pgembed-wip-hunk-diff.py" \
  "$JEV_EVIDENCE/wip.hunks.pre.jsonl" "$JEV_EVIDENCE/wip.hunks.post.jsonl"
cmp "$JEV_EVIDENCE/pgembed.stamp.pre" "$STAMP"
```

当前树（2026-10-03 23:12）LIVE **存在**：步骤 0b 写入 `overlay.mode=live-existing`。bootstrap 只在初始 LIVE 不存在时发生。O2 工作区不得变成 baseline。overlay 入口不得 `unlink(LIVE)`。

禁止：`make -C pgembed/pgbuild all|bundle-metadata|relativize`；`pg-jev/test/run.sh`；真实 API。

---


## 5. 实施边界

- 获准前零 C/SQL 实现。不得改 pgembed 生产文件（`__init__.py`、`pgbuild/Makefile`、已有 generator hunk）。
- 本文闭合九条 P1 **不等于** Oracle 批准。实施门禁证据见表 §0：vmmap-on-postgres-backend 仍是预检；其余为命名断言。
- 测试仅 127.0.0.1 mock、合成 cancel-stage handshake、固定 `test-key`。`.invalid` / 闭端口只作立即错误对照。`57014` 必须来自 `:SQLSTATE` 帧。`ProcDie` 保持 FATAL，且不得 cache 恢复。
- Darwin 加载镜像主证据是 `/usr/bin/vmmap`；`lsof` 只附录。
- 不把绝对路径写入 metadata。
- 无线程、无 PL/Python wrapper。
