# fx-superpowers

`fx-superpowers` 是一个基于 `fxsh` 的 clean-room CLI 项目，用于复现 superpowers 的核心能力。

当前阶段：MVP 骨架（命令路由与模块结构已建立）。

## 新手文档

- 中文快速开始：[docs/quickstart_zh.md](/data/data/com.termux/files/home/yyscode/dev0304/fxsh/apps/fx-superpowers/docs/quickstart_zh.md)

## 目录

- `src/main.fxsh` 入口与命令路由
- `src/fxsuperpowers/*.fxsh` 核心模块
- `scripts/fx-superpowers` 薄启动器
- `Makefile` 本地构建/检查入口

## 使用

从仓库根目录执行：

```sh
make -C apps/fx-superpowers launcher
apps/fx-superpowers/bin/fx-superpowers --help
apps/fx-superpowers/bin/fx-superpowers status
apps/fx-superpowers/bin/fx-superpowers init --root .tmp/demo
apps/fx-superpowers/bin/fx-superpowers install skill /path/to/skill-dir --root .tmp/demo
apps/fx-superpowers/bin/fx-superpowers install command /path/to/review.md --root .tmp/demo
apps/fx-superpowers/bin/fx-superpowers update --root .tmp/demo --dry-run
apps/fx-superpowers/bin/fx-superpowers render /path/to/template.md --root .tmp/demo --arguments "foo bar"
```
