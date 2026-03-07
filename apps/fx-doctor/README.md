# fx-doctor

`fx-doctor` is a local AI engineering assistant CLI built around `fxsh`.

The stable demo path is a hybrid:

- shell launcher handles repo probing and command execution
- `fxsh` formats the final text / JSON / prompt outputs
- the app stays multi-file and ships with a dedicated `Makefile`

## What It Does

- detect common local project stacks
- run read-only repo health checks
- summarize release readiness
- emit text, JSON, or LLM handoff prompt output
- support `--root`, `--no-run`, and `--write`

## Structure

- `src/main.fxsh` output formatter and mode switcher
- `src/fxdoctor/stringx.fxsh` shared string / JSON escaping helpers
- `src/fxdoctor/cli.fxsh` CLI env helpers
- `src/fxdoctor/detect.fxsh` project detection helpers
- `src/fxdoctor/git.fxsh` shell command helpers
- `src/fxdoctor/model.fxsh` language-feature showcase module
- `scripts/fx-doctor` thin argv launcher and runtime driver
- `Makefile` launcher build/check entrypoint

## Build

From the repository root:

```sh
make
make -C apps/fx-doctor
```

This produces:

- `apps/fx-doctor/bin/fx-doctor`

## Usage

```sh
apps/fx-doctor/bin/fx-doctor
apps/fx-doctor/bin/fx-doctor --json
apps/fx-doctor/bin/fx-doctor --prompt
apps/fx-doctor/bin/fx-doctor --root . --write build/fx-doctor-report.txt
apps/fx-doctor/bin/fx-doctor --no-run
```

## Note

The current public demo path intentionally prefers the launcher-backed interpreter flow over `native-codegen`, because that path is more stable on real multi-file CLI workloads in `fxsh 1.0`.
