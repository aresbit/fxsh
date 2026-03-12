# fx-superpowers 新手快速上手（中文）

这份文档面向第一次接触 `fxsh` 的同学，目标是让你从 0 开始把工具跑通。

## 1. 你会得到什么

完成本文后，你可以：

- 编译 `fxsh` 二进制
- 构建 `fx-superpowers` 启动器
- 初始化一个工作目录
- 安装 skill / command
- 执行 update
- 渲染模板变量

## 2. 环境要求

最小要求：

- `sh`
- `make`
- `cc`（clang 或 gcc）
- `git`（如果你要从 git URL 安装）

检查命令：

```sh
command -v sh
command -v make
command -v cc
command -v git
```

只要命令能返回路径即可。

## 3. 一次性编译（在仓库根目录）

```sh
cd /data/data/com.termux/files/home/yyscode/dev0304/fxsh
make
make -C apps/fx-superpowers launcher
```

验证二进制：

```sh
./bin/fxsh --version
apps/fx-superpowers/bin/fx-superpowers --help
```

## 4. 第一次跑通（复制即可）

### 4.1 初始化工作区

```sh
apps/fx-superpowers/bin/fx-superpowers init --root .tmp/demo
apps/fx-superpowers/bin/fx-superpowers status --root .tmp/demo
```

你应该看到：

- `initialized: true`
- `skills_dir: true`
- `commands_dir: true`

### 4.2 安装一个本地 skill

先准备一个最小 skill：

```sh
mkdir -p .tmp/skill-local
cat > .tmp/skill-local/manifest.json <<'EOF'
{"name":"demo-local","version":"0.1.0","entry":"SKILL.md","description":"demo"}
EOF
cat > .tmp/skill-local/SKILL.md <<'EOF'
# Demo Local Skill
EOF
```

安装：

```sh
apps/fx-superpowers/bin/fx-superpowers install skill .tmp/skill-local --root .tmp/demo --json
apps/fx-superpowers/bin/fx-superpowers list --root .tmp/demo --json
```

### 4.3 安装一个本地 command 文件

```sh
mkdir -p .tmp/cmd-local
cat > .tmp/cmd-local/review.md <<'EOF'
# review command
EOF

apps/fx-superpowers/bin/fx-superpowers install command .tmp/cmd-local/review.md --root .tmp/demo --json
ls -la .tmp/demo/.codex/commands
```

### 4.4 更新（update）

修改 skill 源文件后执行：

```sh
cat > .tmp/skill-local/SKILL.md <<'EOF'
# Demo Local Skill v2
EOF

apps/fx-superpowers/bin/fx-superpowers update --root .tmp/demo --json
```

干跑（不落盘）：

```sh
apps/fx-superpowers/bin/fx-superpowers update --root .tmp/demo --dry-run --json
```

## 5. 模板渲染（render）

### 5.1 行内模板

```sh
apps/fx-superpowers/bin/fx-superpowers render 'A={{ARGUMENTS}} R={{ROOT}} D={{DATE}}' --root .tmp/demo --arguments 'foo bar'
```

### 5.2 文件模板

```sh
cat > .tmp/demo.tpl <<'EOF'
arg={{ARGUMENTS}}
root={{ROOT}}
date={{DATE}}
EOF

apps/fx-superpowers/bin/fx-superpowers render .tmp/demo.tpl --root .tmp/demo --arguments 'hello'
```

允许的 token 只有 3 个：

- `{{ARGUMENTS}}`
- `{{ROOT}}`
- `{{DATE}}`

出现其它 token（例如 `{{WHO}}`）会报错。

## 6. 常用参数速查

- `--root <path>`：指定工作目录根
- `--scope workspace|user`：安装到项目级或用户级
- `--json`：机器可读输出
- `--dry-run`：预演，不落盘
- `--on-conflict skip|overwrite|backup`：冲突策略
- `--arguments "<text>"`：render 参数

## 7. 常见问题

### Q1: `install skill` 提示 `manifest_ok: false`

原因：源目录缺少 `manifest.json`。  
修复：补齐 `manifest.json` 后重试。

### Q2: 为什么 `list` 看不到 `.bak.*`？

设计如此：备份目录不会出现在技能列表中，避免干扰正常结果。

### Q3: `--dry-run` 是否会写文件？

不会。用于预演结果。

## 8. 清理演示目录

```sh
rm -rf .tmp/demo .tmp/skill-local .tmp/cmd-local .tmp/demo.tpl
```

