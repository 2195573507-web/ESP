# Frontend / Backend Boundary

本文件约束 ESP-server 项目的前后端分工。无论在 `api` 分支还是 `ui` 分支，仓库都保留完整项目代码，但开发人员只修改自己职责内的文件。

## 当前目录职责归类

- `public/`: 前端页面、样式和浏览器端脚本。
- `server.js`: 后端 Express 服务入口。
- `server-time-sync/`: 后端时间同步模块。
- `package.json` / `package-lock.json`: 后端 Node.js 运行和依赖文件。
- `db/`: SQLite 数据库相关文件。
- `docs/api.md`: 前后端和 ESP 设备之间的 API 契约。
- `docs/frontend-backend-boundary.md` / `docs/deploy-branches.md`: 项目协作和部署分支说明，日常开发不要随意修改。

## 前端人员只修改

前端人员只修改 `public/` 目录下的页面文件：

- `public/index.html`: 页面结构和 DOM 节点。
- `public/styles.css`: 页面样式、布局、响应式规则。
- `public/app.js`: 浏览器端状态、交互、接口调用、数据展示逻辑。

`public/app.js` 中的 API 地址应集中放在顶部 `API_CONFIG` 对象里。后端接口路径变化时，前端优先修改 `API_CONFIG`，不要在业务函数里分散写死 URL。

## 后端人员只修改

后端人员只修改服务端、依赖、数据库和接口文档相关文件：

- `server.js`: Express 服务、路由、请求处理、数据库读写。
- `package.json`: 后端运行脚本和依赖声明。
- `package-lock.json`: 依赖锁定文件；只有依赖确实变化时才修改。
- `db/`: 数据库文件、数据库初始化或迁移相关内容。
- `docs/api.md`: HTTP API 路径、请求字段、响应字段和兼容说明。

后端调整接口字段、数据库字段或 ESP 上传/读取协议前，必须先更新 `docs/api.md`。

## 前端禁止修改

前端人员不得修改以下文件或目录：

- `server.js`
- `package.json`
- `package-lock.json`
- `db/`
- `server-time-sync/`
- `docs/api.md`
- `docs/frontend-backend-boundary.md`
- `docs/deploy-branches.md`
- 其他服务端配置或后端目录

前端不得直接访问数据库文件，也不得通过浏览器端代码读写 SQLite。前端只能通过 `docs/api.md` 中记录的 HTTP API 获取或提交数据。

## 后端禁止修改

后端人员不得修改以下前端页面文件：

- `public/index.html`
- `public/styles.css`
- `public/app.js`
- `docs/frontend-backend-boundary.md`
- `docs/deploy-branches.md`

后端不得把页面样式、布局、交互动画、DOM 拼装等前端展示逻辑写进 `server.js`。`server.js` 只负责提供静态文件、API、数据读写和健康检查。

## 共同约定

- 接口字段变更必须先改 `docs/api.md`，再改后端实现，最后由前端按文档适配。
- ESP 通信协议不得为页面展示临时改名或临时增删字段。
- 数据库结构或写入逻辑变化必须由后端负责，前端不能绕过 HTTP API。
- `api` 分支用于后端开发和部署，`ui` 分支用于前端开发和部署；两个分支都保留完整项目代码，但部署时按 `docs/deploy-branches.md` 的文件范围覆盖服务器。
