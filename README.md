# subconverter

用于在多种代理订阅格式之间转换的本地服务。本仓库基于
[tindy2013/subconverter](https://github.com/tindy2013/subconverter)，并提供 Windows x64 发布包。

[![Build Status](https://github.com/yxymeng/subconverter/actions/workflows/build.yml/badge.svg)](https://github.com/yxymeng/subconverter/actions)
[![GitHub release](https://img.shields.io/github/release/yxymeng/subconverter.svg)](https://github.com/yxymeng/subconverter/releases)
[![GitHub license](https://img.shields.io/github/license/yxymeng/subconverter.svg)](https://github.com/yxymeng/subconverter/blob/master/LICENSE)

## 本次发布修复

### 原始问题

部分订阅服务会拒绝转换器自动添加的内部请求头：

```http
SubConverter-Request: 1
SubConverter-Version: <version>
```

这类服务会返回 HTTP 403。订阅链接本身仍然可以返回 Base64 内容，但 subconverter 在下载失败后只能看到空内容，因此日志可能显示：

```text
The following link doesn't contain any valid node info
```

这个提示不一定表示订阅内容不是 Base64，也不一定表示节点格式错误，首先应检查下载请求的状态码和代理链路。

### 修复内容

`src/handler/webget.cpp` 已做以下处理：

1. 不再向普通订阅请求添加 `SubConverter-Request` 和 `SubConverter-Version` 两个内部请求头。
2. 保留调用方明确传入的请求头。
3. 调用方没有传入 `User-Agent` 时，继续使用 subconverter 默认的 User-Agent。

这样既不会暴露会触发服务端拦截的内部标记，也不会影响需要自定义请求头的调用。发布包中的 Windows x64 程序已按相同逻辑修复并验证可正常转换 Trojan 订阅。

## 支持的格式

### 常见输入订阅格式

支持解析 Clash/ClashR、Surge 2-5、Quantumult、Quantumult X、Loon、Surfboard、SSD、SSR、V2Ray、Shadowsocks Android，以及 Base64 编码的单节点链接列表。

也可以直接传入以下单节点链接：

| 节点类型 | 常见链接或配置类型 |
| --- | --- |
| Shadowsocks | `ss://` |
| ShadowsocksR | `ssr://` |
| VMess | `vmess://`、V2Ray JSON |
| Trojan | `trojan://` |
| Snell | Surge 配置中的 `snell` |
| HTTP / HTTPS / SOCKS5 | 配置文件或 Telegram 风格链接 |
| WireGuard | Clash、Surge 等配置中的 WireGuard |
| Hysteria / Hysteria2 | Clash 配置、`hysteria2://` 或 `hy2://` |
| AnyTLS | Clash 配置、`anytls://` |

VLESS、TUIC 等没有列出的协议不应被视为当前版本的独立支持类型；如果目标格式不支持某种节点，可以使用 `fdn=true` 过滤不兼容节点。

### 输出目标

| 目标格式 | `target` 参数 |
| --- | --- |
| Clash | `clash` |
| ClashR | `clashr` |
| Quantumult | `quan` |
| Quantumult X | `quanx` |
| Loon | `loon` |
| Surfboard | `surfboard` |
| Surge 2 / 3 / 4 / 5 | `surge&ver=2`、`surge&ver=3`、`surge&ver=4`、`surge&ver=5` |
| V2Ray | `v2ray` |
| Trojan | `trojan` |
| Mellow | `mellow` |
| sing-box | `singbox` |
| Shadowsocks | `ss` |
| Shadowsocks Android | `sssub` |
| SSD | `ssd` |
| SSR | `ssr` |
| 单链接列表 | `mixed` |
| 根据 User-Agent 自动判断 | `auto` |

`mixed` 输出的是由单节点链接组成的 Base64 订阅；`auto` 会根据请求方的 User-Agent 选择输出格式。

## Windows 部署

1. 从 [Releases](https://github.com/yxymeng/subconverter/releases) 下载 Windows x64 压缩包，完整解压到一个固定目录。不要只复制 `subconverter.exe`，因为 `base`、`config`、`profiles`、`rules` 和 `snippets` 目录也会被使用。
2. 首次使用时，可将 `pref.example.toml` 复制为 `pref.toml`，再按需修改。配置文件必须放在 `subconverter.exe` 同一目录。
3. 仅本机使用时，建议在 `[server]` 中设置：

   ```toml
   [server]
   listen = "127.0.0.1"
   port = 25500
   ```

   如果需要让局域网其他设备访问，再将 `listen` 改为局域网地址或 `0.0.0.0`，并自行配置防火墙和访问控制。
4. 双击 `subconverter.exe` 启动服务，或者在 PowerShell 中执行：

   ```powershell
   .\subconverter.exe
   ```

5. 浏览器访问 `http://127.0.0.1:25500/version`。看到版本信息即表示服务已经启动。关闭程序窗口会停止服务。

### 订阅下载代理

如果本机网络需要通过 Clash Verge、Clash 或其他 HTTP/SOCKS 代理访问订阅源，请在 `pref.toml` 中配置对应项目。以 HTTP 代理为例：

```toml
proxy_subscription = "http://127.0.0.1:7897"
proxy_config = "http://127.0.0.1:7897"
proxy_ruleset = "http://127.0.0.1:7897"
```

`proxy_subscription` 控制原始订阅下载；`proxy_config` 控制外部配置下载；`proxy_ruleset` 控制规则集下载。端口必须替换成代理软件实际监听的 HTTP 或 mixed 端口。没有代理需求时可以使用 `NONE`，使用系统代理时可以使用 `SYSTEM`。

## 调用接口

基本接口格式如下，`url` 和 `config` 的值需要先进行 URL 编码：

```text
http://127.0.0.1:25500/sub?target=clash&url=%URL%&config=%CONFIG%
```

PowerShell 示例：

```powershell
$subscription = [uri]::EscapeDataString('https://example.com/subscribe?token=YOUR_TOKEN')
$config = [uri]::EscapeDataString('https://example.com/Clash.ini')
$convertUrl = "http://127.0.0.1:25500/sub?target=clash&insert=true&emoji=true&udp=true&clash.doh=true&new_name=true&filename=converted.yaml&url=$subscription&config=$config"
$convertUrl
```

然后把输出的 URL 添加到 Clash Verge 的订阅中。`127.0.0.1` 只对当前电脑有效；如果 Clash Verge 在另一台设备运行，需要使用部署 subconverter 的电脑的局域网 IP，并相应调整 `listen` 和防火墙设置。

常用参数：

| 参数 | 作用 |
| --- | --- |
| `target` | 输出格式，例如 `clash`、`singbox`、`surge&ver=4` |
| `url` | 原始订阅或节点链接，必须 URL 编码 |
| `config` | 外部分组和规则配置，可选，必须 URL 编码 |
| `insert` | 是否启用配置中的插入节点 |
| `emoji` | 是否添加节点 Emoji |
| `udp` | 是否为支持的节点开启 UDP |
| `new_name` | 是否使用 Clash 新字段名称 |
| `filename` | 输出文件名 |
| `fdn` | 是否过滤目标格式不支持的节点 |

合并多份订阅时，先用 `|` 连接原始 URL，再对整个字符串进行一次 URL 编码。

## 故障排查

1. `127.0.0.1:25500/version` 无法访问：检查程序是否启动、端口是否被占用，以及 `pref.toml` 中的 `[server]` 配置。
2. 日志出现 `doesn't contain any valid node info`：先检查原始订阅请求是否返回 200，再检查 `proxy_subscription`。这个日志也可能是下载失败后的泛化提示。
3. 原始链接在浏览器有内容，但转换失败：浏览器可能使用了系统代理，而 subconverter 没有自动继承该代理。将 `proxy_subscription` 设置为代理软件的 HTTP/mixed 端口后重试。
4. 仍然没有节点：确认订阅 URL 只编码一次、订阅没有过期，并用 `target=clash` 先进行最小化测试。

## 安全注意事项

- 不要把真实订阅 Token、`pref.toml`、`cache` 目录或带 Token 的 `gistconf.ini` 提交到 GitHub。
- 只在本机使用时优先监听 `127.0.0.1`，不要无必要地暴露到公网。
- 如果必须提供远程访问，请开启 API 模式并设置访问 Token，同时限制防火墙来源。
- `/sub` URL 中通常包含订阅 Token，不要把完整 URL 粘贴到公开 issue、日志或聊天记录中。

## Docker

Docker 部署和自定义配置说明请参见 [README-docker.md](README-docker.md)。Windows 本地用户直接使用 Releases 中的压缩包即可。

## 自动上传 Gist

如需使用 `upload=true`，先在程序目录的 `gistconf.ini` 中配置 GitHub Personal Access Token，再将 `upload=true` 添加到调用 URL。Token 只应保存在本地配置文件中，不要提交到仓库。

## 致谢

- [tindy2013/subconverter](https://github.com/tindy2013/subconverter)
- [asdlokj1qpi233/subconverter](https://github.com/asdlokj1qpi233/subconverter)

本项目遵循原项目的开源许可证，详见 [LICENSE](LICENSE)。
