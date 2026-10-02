# Code review 验收

使用 Matt Pocock 的 [code-review skill](https://github.com/mattpocock/skills/blob/d81f3a183412e71a5b1e84ca21bc1a35eea03a60/skills/engineering/code-review/SKILL.md)，Standards 和 Spec 由两个独立子审查并行完成，主审查复现行为问题。首次只审查 subconverter；本次按用户要求完成最小修复，没有发布产物。

2026-10-02 修复后复验通过：仅修复以下四项缺陷并补充现有回归，未实施两项可维护性重构。下文首次验收发现保留为修复前记录。

基准：`cee62a392db4e187bc6087acad1a4d2f03d54755`。本轮实现全部为未提交的工作区改动，因此使用 `git diff cee62a392db4e187bc6087acad1a4d2f03d54755 -- .`，并将 `git ls-files --others --exclude-standard` 列出的新增文件作为完整新增内容审查；基准之后的提交列表为空。需求依据为 optimization-roadmap.md 和 ADR-0001。

仓库没有编码规范文档，也没有 skill 要求的 docs/agents/issue-tracker.md。已有本地需求文档足以完成此次 Spec 审查；以后若需从 Issue 自动读取需求，可运行作者的 `/setup-matt-pocock-skills`。

## Standards

未发现明确的仓库编码标准违规。以下两项均为 skill 基线中的判断项，不是硬性违规。

1. **Possible Duplicated Code**：src/handler/interfaces.cpp:641、668、1133 重复新增相同失败消息和警告策略：

   ```cpp
   const auto error = parse_set.error.empty() ? "No valid nodes found in " + safeSource(x) : parse_set.error;
   recordWarning(error);
   writeLog(0, error, LOG_LEVEL_WARNING);
   ```

   各处拒绝分支还重复相同的消息表达式。建议提取共同的失败消息生成和警告记录，避免不同转换入口逐渐采用不同策略。

2. **Possible Feature Envy**：src/handler/interfaces.cpp:982 直接访问诊断对象的锁、JSON 存储和指标名称：

   ```cpp
   std::lock_guard<std::mutex> lock(context->mutex);
   if(context->metrics.contains("nodes_exported") && context->metrics["nodes_exported"] == 0)
   ```

   建议由 diagnostics API 提供加锁查询，让转换入口不依赖诊断对象内部表示。

## Spec

首次验收发现的 4 个问题，均已修复：规则正文校验后才写缓存；有源站专用头时阻止跨源跳转；参数变更使旧预览失效；还原链接允许省略订阅参数。以下为修复前的证据和分析。

1. **[P1] HTTP 200 无效规则覆盖旧缓存，并静默生成缺少规则的结果。**

   位置：src/handler/interfaces.cpp:523；src/handler/webget.cpp:395–398。

   需求：optimization-roadmap.md:34「必需规则失败时不静默生成缺少规则的配置」；ADR-0001:7「必需规则集更新失败时明确报错，并保留上一次成功版本供用户手动选择」。

   复现：先正常下载 `DOMAIN,previous-good.example`；再让同一规则源返回非空 HTML，HTTP 200；执行 `refresh=true`。实际返回 HTTP 200、success=true，输出没有原规则，只剩配置中的 MATCH。再执行 `use_stale=true`，原规则仍不能恢复。原因是下载层在语义校验之前覆盖缓存，而必需规则检查仅检查正文非空。

   修复方向：按规则格式验证下载正文，并在校验通过后才提交新缓存；无效内容应报错并保留原来的可用规则。需要兼容合法注释、支持的规则格式和合理空集，不能仅用 HTML 字符串黑名单代替格式判断。

2. **[P1] 按 origin 配置的凭据随跨源重定向发送到未配置源站。**

   位置：src/handler/webget.cpp:227–230；自动重定向位于同文件:160。初始头选择位于 src/generator/config/nodemanip.cpp:152–154。

   需求：optimization-roadmap.md:20「明确客户端头与订阅源头的边界」及「按源站提供凭据」。usage.md 也明确声明自定义头只按精确 origin 匹配。

   复现：为 A origin 配置虚构 `X-Source-Key`，A 返回 302 到不同端口的 B origin，B 没有配置任何专用头。实际 B 收到了完整 `X-Source-Key`，转换返回 200。cURL 自动跟随跳转时会继续携带这类自定义头；仅在初始 URL 匹配 origin 不足以保证隔离。此复现针对任意自定义凭据头，不声称 cURL 对 Authorization/Cookie 具有完全相同的转发行为。

   修复方向：逐跳匹配目标 origin，重新生成专用请求头；或者对带专用头的跨源跳转采取明确的限制策略，同时保留需要支持的正常跳转。

3. **[P2] 等待转换时重新生成链接，会显示与链接不对应的成功预览。**

   位置：base/web/app.js:59–75；生成链接入口位于:27–29。

   需求：optimization-roadmap.md:54「页面明确区分‘已生成转换链接’和‘后端实际转换成功’」；:56「预览读取真实转换结果」。

   复现：Chromium 中预览订阅 A，源站延迟响应；等待时改填 B 并点击生成链接。A 返回后，页面显示转换成功并启用下载，链接指向 B，但预览仍是 A 的节点，B 没有收到请求。响应没有与当前参数版本核对。

   修复方向：变更参数或重新生成链接时使旧请求失效，或明确保留并展示该结果所对应的参数快照；旧响应不能给新链接标记转换成功。

4. **[P2] 使用服务端默认订阅的有效旧链接无法还原。**

   位置：base/web/converter.js:12–13；base/web/app.js:46；base/web/index.html:11。

   需求：optimization-roadmap.md:52「支持从已有转换 URL 还原参数，复用现有后端参数与转换行为」；:56「生成链接与原参数行为一致，能还原已有 URL」。

   复现：服务配置 `default_url` 后，`/sub?target=clash` 可以成功转换；将该链接粘贴到 Web 还原入口，却报「请先填写订阅或节点链接」。生成器强制显式 url，加上表单 required，阻止复用原后端默认行为。

   修复方向：区分新建表单的输入要求和合法旧链接中的省略参数，保留服务端默认订阅语义。

未发现明确无授权的范围扩张。

## 首次验证证据与边界

- 当前 native 构建执行 `cmake --build /workspace/.cloud-setup/build/subconverter -j4`，返回 `ninja: no work to do`，确认审查使用的二进制对应当前源码。
- 原有完整程序集成测试 9/9 通过；URL 测试通过；原有真实 Chromium 验收通过；四个发布 shell 脚本的语法检查通过。这些检查没有覆盖上面的边界。
- 四个新增边界使用 localhost 模拟源、虚构凭据和真实程序/Chromium 复现。独立脚本保存在工作区环境的 `/workspace/.cloud-setup/review/subconverter_acceptance_repro.py`，结果为同目录 `subconverter_acceptance_repro.jsonl`；没有添加实现修复或修改原回归测试。
- Windows/macOS 发布路径未实际执行；Docker 原验证受基础镜像 429 限流阻断。它们仍是验证缺口，不作为已确认的代码缺陷，也不能据 Linux 结果宣称对应平台验收通过。

首次验收：Standards 有 2 项判断建议，无硬性违规；Spec 有 4 项缺陷，当时需求验收未通过。

## 修复后复验

- Linux 完整程序和静态库构建通过；CTest 2/2 通过，包含 11 个完整程序集成场景和 URL 回归。Chromium 验证了修改参数后旧响应失效、默认订阅链接还原，以及原有转换、诊断、复制和下载。
- 独立复验 16 个 IP 规则边界通过：无效地址、掩码溢出/超范围/尾随字符均报错并保留旧缓存；合法 IPv4 映射 IPv6 和源地址规则继续可用。原始 HTML 强刷与跨源凭据复现也确认已修复。
- 实际配置中的 25 个远程条目按原顺序检查，其中 7 份使用已有公开规则原文，其余使用原有模拟源；104,861 条输出规则在串行/并发、四种缓存状态下 SHA256 一致，且与修复前一致。有效缓存没有网络请求。
- Windows/macOS 和 Docker 的原验证边界仍存在，未据 Linux 结果宣称这些平台已验收。

最终复验：Standards 0 项新增问题，原 2 项非阻断建议保留；Spec 0 项残留缺陷，四项修复通过验收。
