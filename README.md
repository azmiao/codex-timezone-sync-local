# Codex 离线时区同步

用于 [CLIProxyAPI（CPA）](https://github.com/router-for-me/CLIProxyAPI) 的 Codex 请求时区校正插件。内置离线 IANA 时区表，在管理面板选两个下拉框，即可把请求里的时区与日期改写为目标时区。

**v0.7.3 · 插件 ID `codex-timezone-sync-local` · Linux / macOS / Windows（amd64 + arm64）· CPA C ABI v1**

## 修改自原仓库

本插件修改自 [LonelyHerbivore/codex-timezone-sync](https://github.com/LonelyHerbivore/codex-timezone-sync)（v0.5.1），保留「校正 Codex 请求时区」这一目标，实现整体重写。

原版依靠宿主机上的 Python 探测脚本与 Unix Socket 辅助服务、`*.state` 状态文件，以及 `ipinfo.io` 外网查询来确定时区。本版把这些全部去掉：

| 主要变更 | 说明 |
| --- | --- |
| 内置离线时区表 | 554 个 IANA 时区编译进动态库，运行时零外部依赖，不联网 |
| 双下拉框配置 | 直接选「本地时区」与「目标时区」，不再按出口 IP 自动探测 |
| 去掉宿主机依赖 | 无需辅助进程、无需 Unix Socket、无需状态文件 |
| 面板轻量化 | 只保留时区选择、时间对照、启用开关、最近改写与自检 |
| 跟随主题 | 面板跟随 CPA 管理界面的浅色 / 深色主题 |
| 插件 ID 独立 | 改为 `codex-timezone-sync-local`，可与原版并存，互不覆盖 |

原版的请求统计、模型一致性监控与失败请求日志归档未保留。

> 原仓库未声明许可证；本仓库仅对自身改动以 [MIT](LICENSE) 授权。

## v0.7.3 变更

修复 v0.7.1 之后仍然存在的「重启后设置回到默认值」问题。

根因在宿主加载插件的方式：CPA 会把插件动态库**先复制到临时目录**（`/tmp/cliproxy-pluginhost/pid-N/`）再加载。v0.7.1 改用「插件动态库所在目录」，取到的正是这个临时目录，配置因此写在容器内的 `/tmp` 下——保存当时看起来正常，容器一重启 `/tmp` 被清空，设置就回到默认值。

现在改为按顺序探测候选目录，取第一个「存在、可写、且不在易失存储里」的：

1. 环境变量 `CODEX_TZ_CONFIG_DIR` / `CODEX_TZ_CONFIG_FILE`（每次读取都重新求值）
2. `<宿主可执行文件目录>/plugins` —— 容器部署下即 `/CLIProxyAPI/plugins`，对应宿主机的 `plugins-dir` 挂载卷
3. `<宿主可执行文件目录>/data`
4. 插件动态库所在目录（仅当宿主原地加载时有效）
5. `/CLIProxyAPI/plugins`、`/CLIProxyAPI/data`（宿主改用 helper 进程加载时，可执行文件路径也会指向临时目录，此时靠容器固定布局兜底）
6. `$XDG_CONFIG_HOME`、`$HOME/.config`

其中易失存储指 `/tmp`、`/var/tmp`、`/dev/shm`、`/run/user`、macOS `/var/folders` 与 Windows `%TEMP%`。

配套改动：

- 首次运行时自动把旧位置（临时目录里）的配置迁移到新位置，迁移成功后才删除旧文件，升级后无需重新配置。
- 管理面板底部显示配置文件的实际路径；目录不可写或路径落在临时目录时给出警告，不必等重启才发现设置会丢。
- `status` 接口新增 `config_writable` 字段。
- 修复 Windows 下 `plugin_getenv` 只能读取两个固定变量名的问题，现在任意变量名都可用。
- 修复并发请求下配置路径缓冲区被互相覆盖的问题。

## v0.7.2 变更

下拉框改为默认展示全部时区，并支持按国家、城市、中文名搜索：

- 打开下拉框直接列出全部 554 个时区，不再只显示前 60 条，底部提示总数或当前匹配数。
- 新增 348 条别名，覆盖 105 个时区。搜 `japan`、`日本`、`usa`、`canada` 这类不出现在 IANA id 里的名称也能命中；跨多时区的国家会一次列出全部时区。
- 容忍 1–2 个拼写错误，`japen` → `Asia/Tokyo`、`germny` → `Europe/Berlin`。
- 亮色主题改用宿主的纯白配色（`#ffffff`）。CPA 的 `auto` 主题在浅色系统下解析为纯白而非暖纸色，旧版面板因此比周围页面偏黄；暖纸色仍保留给宿主显式选择的 `light` 主题。

## v0.7.1 变更

修复容器部署下「重启后设置回到默认值」的问题。旧版把配置固定写到 `/CLIProxyAPI/data`，该目录在多数 compose 部署里没有挂载，因此容器一重建文件就丢失。改为按可执行文件位置探测可写目录，面板底部也会显示实际路径。（当时选定的「插件动态库所在目录」仍不持久，v0.7.3 进一步修正。）

## 功能

- 改写请求中的 `<timezone>` 与 `<current_date>`；缺少环境上下文时补写一份 `<environment_context>`。
- 支持 Responses `instructions`、原生 `input`、Chat Completions `messages` 三种载体。
- 按目标时区真实规则计算日期与偏移，含南半球、斋月等非典型夏令时。
- 除被改写的文本外，请求其余字节原样保留（不重新序列化 JSON）。
- 全链路 *fail-open*：时区不可用或请求体异常时一律放行原始请求。

> 插件只校正请求内容，不改变浏览器指纹、OAuth 登录地点、HTTP 头或主机系统时区。

## 安装

推荐用商店源：

```text
https://raw.githubusercontent.com/azmiao/codex-timezone-sync-local/main/registry.json
```

或在 CPA 配置中直接指定：

```yaml
plugins:
  enabled: true
  store-sources:
    - "https://raw.githubusercontent.com/azmiao/codex-timezone-sync-local/main/registry.json"
  configs:
    codex-timezone-sync-local:
      enabled: true
```

也可从 [Releases](https://github.com/azmiao/codex-timezone-sync-local/releases) 下载对应平台 ZIP，解压到 `plugins/<goos>/<goarch>/`——ZIP 根目录就是动态库本身。

> 若已装过原版或 v0.6.x，请先卸载旧插件。插件 ID 已变更，旧配置文件不会自动迁移。

## 配置

| 字段 | 默认值 | 含义 |
| --- | --- | --- |
| 本地时区 | `Asia/Shanghai` | 请求中原本携带的时区，匹配到就替换掉 |
| 目标时区 | `America/Los_Angeles` | 要写入请求的时区 |
| 启用时区改写 | 开 | 关闭后完全不改动请求 |
| 记录最近改写 | 开 | 仅内存保留 20 条，不落盘 |

配置文件 `codex-timezone-sync-local.config` **默认写在 `<宿主可执行文件目录>/plugins`**（容器部署下即 `/CLIProxyAPI/plugins`，对应宿主机的 `plugins-dir` 挂载卷），容器重建后设置仍然保留。可用环境变量 `CODEX_TZ_CONFIG_DIR` 指定目录，或 `CODEX_TZ_CONFIG_FILE` 指定完整路径；面板底部会显示当前实际使用的文件路径。

写入走临时文件 + `rename` 原子替换，权限 `0600`；文件里出现未知时区名时回退默认值。

## 管理 API

前缀 `/v0/management/plugins/codex-timezone-sync-local/`，均需 CPA 管理密钥：

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `status` | 当前设置与两侧时区时间 / 偏移 |
| PUT | `settings` | 保存设置 |
| GET | `timezones` | 时区候选（554 项） |
| GET | `events` | 最近 20 条改写记录 |
| POST | `events/clear` | 清空记录 |
| POST | `selftest` | 用样例请求演练改写 |

## 编译与测试

```bash
make test      # 插件契约测试
make test-tz   # 时区表校验（需 pip install tzdata）
```

- `test_plugin.py`：通过 C ABI 调用编译产物，覆盖注册、面板、三类请求载体改写、设置校验与自检。
- `test_timezone_rules.py`：与官方 `tzdata` 在 2024–2045 全窗口逐时区比对偏移与缩写。

时区表由 `tools/gen_tz_table.py` 生成并提交，构建与运行都不需要 tzdata：

```bash
uv run --with tzdata python tools/gen_tz_table.py
```

发布流程在五个平台原生编译并跑测试，以 `v0.7.3` 这类 tag 发布 Release，附 `checksums.txt`。
