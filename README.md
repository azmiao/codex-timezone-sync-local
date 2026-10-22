# Codex 离线时区同步（codex-timezone-sync-local）

用于 [CLIProxyAPI（CPA）](https://github.com/router-for-me/CLIProxyAPI) 的 Codex 请求时区校正插件。在管理面板里选两个时区即可：**本地时区**是请求里原本携带的时区，**目标时区**是要写入请求的时区。

**当前版本：v0.7.0。插件 ID：`codex-timezone-sync-local`。目标平台：Linux amd64 / arm64、macOS amd64 / arm64、Windows amd64。** 采用 CPA C ABI v1。

> v0.7.0 起插件更名为 **Codex 离线时区同步**，插件 ID 从 `codex-timezone-sync` 改为 `codex-timezone-sync-local`，用于和早期依赖宿主机辅助服务的版本明确区分。两者可以并存安装，互不覆盖。

## 设计取舍

v0.5.x 依赖宿主机上的 Python 探测脚本、Unix Socket 辅助服务、`*.state` 状态文件和 `ipinfo.io` 外网查询，才能在请求里写入时区。v0.6.0 起改为：

- **内置离线 IANA 时区表**：597 个 IANA 时区随插件编译进动态库，运行时零外部依赖。
- **两个下拉框**：管理面板直接选择本地时区与目标时区，配置存在一个小配置文件里。
- **无网络、无辅助服务、无状态文件**：插件不发起任何外部请求，也不需要宿主机跑任何进程。

## 功能

- 把请求中的 `<timezone>` 与 `<current_date>` 改写为目标时区及其当天日期。
- 请求里没有环境上下文时，按目标时区补写一份 `<environment_context>`。
- 支持 Responses `instructions`、原生 `input`、Chat Completions `messages` 三种载体。
- 支持夏令时：日期与偏移按目标时区的真实规则计算，包含南半球与斋月等非典型规则。
- 中文管理面板：时区下拉框、当前时间对照、启用开关、最近改写记录、自检；面板跟随 CPA 管理界面的浅色/深色主题。
- 除被改写的文本外，请求其余字节原样保留（不重新序列化 JSON）。
- 全链路 *fail-open*：时区不可用、请求体异常时一律放行原始请求。

> 注意：插件只校正请求内容，不改变浏览器指纹、OAuth 登录地点、HTTP 头或主机系统时区。

## 从 CPA 插件商店安装

在 CPA 中打开「插件商店」，搜索 **Codex 离线时区同步** 或 **codex-timezone-sync-local**。若官方收录尚未合并，可先添加本仓库提供的第三方商店地址：

```text
https://raw.githubusercontent.com/azmiao/codex-timezone-sync-local/main/registry.json
```

或者在 CPA 配置中指定：

```yaml
plugins:
  enabled: true
  store-sources:
    - "https://raw.githubusercontent.com/azmiao/codex-timezone-sync-local/main/registry.json"
  configs:
    codex-timezone-sync-local:
      enabled: true
```

Release 提供 `linux_amd64`、`linux_arm64`、`darwin_amd64`、`darwin_arm64`、`windows_amd64` 五种动态库 ZIP 和 `checksums.txt`。ZIP 根目录只有动态库本身，安装后无需任何额外组件。

## 从 v0.6.x 升级

插件 ID 变了，宿主会把它当成一个全新的插件：

1. 在 CPA 管理界面卸载旧的 `codex-timezone-sync`，避免两个插件同时改写同一批请求。
2. 安装 `codex-timezone-sync-local`，重新选择本地与目标时区（旧配置文件不会自动迁移）。
3. 旧数据文件 `codex-timezone-sync.config` 可直接删除。

## 配置

打开 CPA 管理界面的「Codex 离线时区同步」插件菜单，在「时区设置」里选择：

| 字段 | 含义 |
| --- | --- |
| 本地时区 | 请求中原本携带的时区。匹配到就替换掉，默认 `Asia/Shanghai` |
| 目标时区 | 要写入请求的时区，默认 `America/Los_Angeles` |
| 启用时区改写 | 关闭后插件完全不改动请求 |
| 记录最近改写 | 关闭后不再记录活动列表（仅内存，不落盘） |

配置文件位于 CPA 数据目录下的 `codex-timezone-sync-local.config`，可用环境变量 `CODEX_TZ_CONFIG_FILE` 覆盖路径。文件内容形如：

```text
enabled=1
logging=1
local_zone=Asia/Shanghai
target_zone=America/Los_Angeles
```

写入采用临时文件 + `rename` 的原子替换，权限 `0600`。文件里出现未知时区名时回退到默认值，不会让插件不可用。

## 管理 API

相关路由必须使用 CPA 管理密钥，静态面板本身不公开任何统计或请求日志。

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/plugins/codex-timezone-sync-local/status` | 当前设置、两侧时区的时间/偏移 |
| PUT | `/plugins/codex-timezone-sync-local/settings` | 保存设置 |
| GET | `/plugins/codex-timezone-sync-local/timezones` | 时区下拉框候选（597 项） |
| GET | `/plugins/codex-timezone-sync-local/events` | 最近 20 条改写记录 |
| POST | `/plugins/codex-timezone-sync-local/events/clear` | 清空记录 |
| POST | `/plugins/codex-timezone-sync-local/selftest` | 用样例请求演练改写 |

## 数据与隐私

插件**不落盘保存任何请求内容**。最近改写记录只保留时间、结果、原时区、目标时区和日期五个字段，存在内存里，最多 20 条，CPA 重启即清空。v0.5.x 的失败请求体归档功能已移除。

## 源码编译与测试

Linux/macOS 需要 `cc`、`make` 和 `python3`：

```bash
make clean
make test          # 插件契约测试
make test-tz       # 时区表校验（需 pip install tzdata）
```

- `test_plugin.py`：通过 C ABI 调用已编译动态库，覆盖注册、面板、设置校验、三类请求载体改写、时区替换、活动记录与自检。
- `test_timezone_rules.py`：把 `timezone.c` 的解析逻辑在 Python 中复刻一遍，与官方 `tzdata` 在 2024–2045 全窗口逐时区比对偏移与缩写，并检查每个转换点前后 1 分钟。

时区表由 `tools/gen_tz_table.py` 生成并提交在仓库里，运行时和构建时都不需要 tzdata：

```bash
uv run --with tzdata python tools/gen_tz_table.py
```

生成器对每个时区先校验 TZif 尾部 POSIX 规则能否覆盖整个窗口；不能覆盖的（如摩洛哥斋月夏令时、2026 年阿尔伯塔永久 CST）改为写入显式转换表，因此 597 个时区在窗口内都是精确的。

发布 Workflow 在五个平台上原生编译并运行上述测试，以 `v0.7.0` 等 Git tag 发布 GitHub Release：

```text
codex-timezone-sync-local_0.7.0_linux_amd64.zip
codex-timezone-sync-local_0.7.0_linux_arm64.zip
codex-timezone-sync-local_0.7.0_darwin_amd64.zip
codex-timezone-sync-local_0.7.0_darwin_arm64.zip
codex-timezone-sync-local_0.7.0_windows_amd64.zip
checksums.txt
```

## 兼容性与限制

面向 CLIProxyAPI 内置 C ABI 插件宿主，不适用于不支持插件的旧版 CPA。不同 CPA 分支对管理界面、拦截器回调及插件商店的支持可能不同。插件不是地理位置伪装工具，不提供 OAuth 凭据，也不向第三方转发任何请求内容。

本项目以 [MIT License](LICENSE) 授权。使用前请遵守适用法律及上游服务条款。
