# 日常使用和诊断

发布包包含程序、配置示例、模板、规则和 `web/`。首次启动复制一个配置示例为实际配置；已有配置不会被覆盖。配置优先级为命令行 `-f`，否则依次查找 pref.toml、pref.yml、pref.ini。相对文件按配置目录解析。

## Windows、本机与局域网

先在 PowerShell 中检查：

```powershell
.\subconverter.exe -f .\pref.toml --check
.\subconverter.exe -f .\pref.toml
```

`--check` 只检查配置并输出 JSON，包含实际配置路径、监听地址、端口、版本/源码标识、三个下载代理和并发/超时设置；不会检查端口占用或下载订阅。正常启动才绑定端口，冲突返回非零退出码并报告原因。

默认监听 `127.0.0.1:25500`，浏览器打开 `http://127.0.0.1:25500/`。自己的局域网设备需要在 `[server]` 中设 `listen = "0.0.0.0"`，并通过 `http://本机局域网IP:25500/` 访问；生成链接使用页面访问的地址。Windows 防火墙只按实际局域网需要开放端口。

现有配置启用页面需设 `serve_file_root = "web"`；INI 写 `serve_file_root=web`，YAML 写 `serve_file_root: web`。页面支持订阅合并、目标格式、Clash.ini 预设、自定义配置、参数还原、复制链接、实际转换预览和下载结果。它不会保存输入到浏览器存储；“生成链接”与“转换成功”分别显示。复制出的链接及预览结果仍包含使用所需的订阅/节点凭据。

## 缓存和下载

TOML `[advanced]` 示例（INI 用相同键，YAML 放在 advanced 下）：

```toml
enable_cache = true
cache_subscription = 60
cache_config = 300
cache_ruleset = 21600
async_fetch_ruleset = true
max_parallel_downloads = 4
download_timeout = 15
connect_timeout = 5
serve_cache_on_fetch_fail = false
subscription_source_headers = '{}'
```

时间单位为秒。下载并发夹在 1–32；总超时夹在 1–300，连接超时不超过总超时。修改并发设置后重载配置即可生效，无需重启；提高上限时线程池按需扩容，降低上限时已开始的下载继续完成，后续下载遵守新上限。`async_fetch_ruleset=false` 保留串行下载对照模式。下载最多尝试两次，只重试 GET/HEAD 的传输失败；HTTP 403 等源站拒绝不会自动重试。

规则每次转换检查缓存有效期；有效缓存直接复用，过期等待更新。`refresh=true` 强刷本次使用的远程资源。必需规则下载失败返回错误，不输出缺少规则的配置，也不会用失败正文覆盖旧缓存。只有明确添加 `use_stale=true`（或选中页面中的手动选项），才允许读取已有完整规则缓存；缺少旧缓存时仍报错。两项同时启用时先强刷，失败后才使用获准的旧规则。它读取每个规则文件最近一次完整下载的版本，不代表一份历史转换的全局快照；原始订阅和外部配置仍按各自有效期下载。新缓存使用 v2 格式，第一次升级不会复用旧格式。

`serve_cache_on_fetch_fail` 控制订阅、外部配置和模板下载失败时是否回退已有缓存；转换请求的必需规则始终采用上面的显式策略。`skip_failed_links=true` 时允许其他订阅继续转换，诊断 warnings 会列出被跳过的源；全无节点仍返回错误。当前默认示例均启用缓存与有上限的并发，已有配置须自行设置。

离线生成 `-g` 采用相同的规则缓存策略；在 `generate.ini` 的产物节中设置 `refresh=true`、`use_stale=true`。生成失败保留已有产物，并返回非零退出码；批量生成会继续处理其他产物，但任一产物失败仍返回非零退出码。

源站自定义头只按精确 origin 匹配（协议、主机、非默认端口），可配置 UA 或凭据：

```toml
subscription_source_headers = '{"https://example.com":{"User-Agent":"my-client","Authorization":"Bearer YOUR_SOURCE_TOKEN"}}'
```

INI 中 JSON 字符串不加外层单引号，YAML 可用单引号包裹。入站请求只继承 User-Agent，自定义源站头覆盖它；入站 Authorization、Cookie、Connection 和任意扩展头不再转发。默认 UA 保留 `subconverter/… cURL/…`，不发送先前触发 403 的内部版本头。缓存按 URL、代理及实际请求头区分。

## 代理与诊断接口

`proxy_subscription`、`proxy_config`、`proxy_ruleset` 分别用于订阅、配置/模板、规则。`NONE` 或空字符串明确直连；`SYSTEM` 读取 Windows 当前用户的静态 WinINET 代理，Linux 依次取首个非空 all_proxy/ALL_PROXY/http_proxy/HTTP_PROXY/https_proxy/HTTPS_PROXY；显式地址直接使用。选定代理不会被 NO_PROXY 隐式绕过。Windows 支持统一代理及 https/http/socks 分项，优先 https、http、socks；PAC 自动配置和系统绕过列表尚未实现。SYSTEM 无可用静态代理时直连。系统 CA 和 TLS 下载校验策略留待单独迁移，现有下载层仍沿用原项目的证书校验行为。

- `/status`：运行状态和实际绑定的监听端点，代理地址脱敏。重载配置不会重新绑定 socket；新配置中的监听地址和端口可通过 `--check` 查看。
- `/sub?...`：保持转换正文，响应头增加 X-Request-ID。
- `/diagnose?...`：使用相同转换参数，返回 success、status_code、request_id、phases、downloads、warnings、metrics，以及成功时的实际 output。失败使用 HTTP 4xx/5xx，并附 JSON 原因。
- `/refreshrules`：按原有 Token 权限强刷服务端默认规则，等待完整更新后响应；不刷新某个请求指定的外部配置规则。

下载记录区分 HTTP 状态和 cURL 传输错误，包含源站标识、代理、缓存状态、字节和耗时。普通日志省略请求/响应头值与源 URL 路径、查询串；详细日志也只输出结构化下载元数据，不输出原始 cURL 调试文本。诊断中的 output 本身是实际配置；排障分享时可以删除 output 字段。各阶段可相互包含，例如规则处理属于导出的一部分，不能把所有阶段时间相加当成总时间。

## 构建和验证

需要 C++20 工具链、cURL >= 7.62、PCRE2、yaml-cpp、RapidJSON、toml11、QuickJS/libcron。发布脚本的源码依赖固定在 `scripts/dependencies.sh`，子模块跟随父提交；系统软件包仍由平台仓库解析。发布默认使用 checkout 内的规则，不在构建中自动拉取新的规则快照。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
ctest --test-dir build --output-on-failure
python3 tests/integration.py --binary build/subconverter
node tests/url.test.js
```

完整依赖安装可参考三个 release 脚本。它们在打包前运行完整程序的本地模拟订阅测试；失败会阻止上传包。浏览器检查为可选项，需要 Playwright 和 Chromium：`python3 tests/browser.py --binary build/subconverter --chromium /path/to/chromium`。

Docker 使用当前 checkout，基础镜像采用 Debian bookworm，取代旧 Alpine 3.16 构建路径；源码依赖仍固定，系统包信息保存在镜像的 build-dependencies.txt。镜像构建时运行同一组本地模拟转换测试。构建与本机端口映射示例：

```sh
docker build -f scripts/Dockerfile --build-arg BUILD_COMMIT="$(git rev-parse HEAD)" -t subconverter:local .
docker run --rm -p 127.0.0.1:25500:25500 subconverter:local
```

容器内默认监听 0.0.0.0，局域网发布时自行选主机端口绑定地址。Docker CI 发布到 `ghcr.io/当前仓库`，使用 GitHub 的 packages 权限；不再克隆上游源码或使用上游 Docker Hub 名称。本轮尚未推送或运行发布流水线。
