# Deploy Branches

本项目使用两个协作分支：

- `api`: 后端分支。完整保存项目代码，但后端人员只修改后端文件。
- `ui`: 前端分支。完整保存项目代码，但前端人员只修改 `public/` 目录。

服务器部署时不要简单整仓覆盖生产目录。后端部署只覆盖后端文件，不覆盖 `public/`；前端部署只覆盖 `public/`，不覆盖 `server.js` 和数据库。

## api 分支部署说明

`api` 分支用于部署后端服务、依赖和接口文档。

允许部署到服务器的文件：

- `server.js`
- `package.json`
- `package-lock.json`
- `server-time-sync/`
- `docs/api.md`
- `docs/frontend-backend-boundary.md`
- `docs/deploy-branches.md`

谨慎部署：

- `db/`: 只有明确要初始化、迁移或替换数据库时才处理。生产数据库应先备份。

禁止在 api 部署中覆盖：

- `public/`
- 前端静态资源和页面文件

示例命令：

```bash
cd /opt/ESP-server
git fetch origin api

# 只更新后端文件，避免覆盖 public/
git checkout origin/api -- server.js package.json package-lock.json server-time-sync docs/api.md docs/frontend-backend-boundary.md docs/deploy-branches.md

npm install
pm2 restart esp-server
pm2 logs esp-server --lines 100
```

如果服务器没有使用 PM2，可用 systemd：

```bash
sudo systemctl restart esp-server
sudo journalctl -u esp-server -n 100 -f
```

## ui 分支部署说明

`ui` 分支用于部署前端页面、样式和浏览器端脚本。

允许部署到服务器的文件：

- `public/index.html`
- `public/styles.css`
- `public/app.js`

禁止在 ui 部署中覆盖：

- `server.js`
- `package.json`
- `package-lock.json`
- `db/`
- `server-time-sync/`

示例命令：

```bash
cd /opt/ESP-server
git fetch origin ui

# 只更新前端目录，避免覆盖后端服务文件
git checkout origin/ui -- public/

pm2 restart esp-server
pm2 logs esp-server --lines 100
```

如果只是静态资源变化，且 Express 正在从 `public/` 直接提供文件，通常不需要 `npm install`。如需确认服务仍正常：

```bash
curl http://localhost:3000/dashboard
curl http://localhost:3000/sensor/latest
```

## 首次部署或依赖变化

后端首次部署或 `package.json` / `package-lock.json` 变化时运行：

```bash
cd /opt/ESP-server
npm install
npm start
```

生产环境建议使用进程管理器：

```bash
pm2 start server.js --name esp-server
pm2 save
pm2 logs esp-server --lines 100
```

## 日志和健康检查

常用检查命令：

```bash
curl http://localhost:3000/api/time/status
curl http://localhost:3000/sensor/latest
pm2 status
pm2 logs esp-server --lines 100
```

如服务使用 systemd：

```bash
sudo systemctl status esp-server
sudo journalctl -u esp-server -n 100 -f
```

## 部署前检查

部署前建议确认本地分支只包含对应职责内的变更：

```bash
git status --short
git diff --name-only origin/main...HEAD
```

`api` 分支不应包含 `public/` 改动。`ui` 分支不应包含 `server.js`、依赖、数据库或后端目录改动。
