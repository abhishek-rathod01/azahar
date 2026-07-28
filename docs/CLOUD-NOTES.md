# Cloud environment setup for azahar

The Claude Code cloud **Setup script** cannot live in the repo - it is attached
to the environment, not the clone. Paste the block below into the environment's
"Setup script" field at claude.ai/code (environment selector -> settings icon).

Everything that *can* live in the repo already does: `.claude/settings.json`
(permissions + SessionStart hook) and `scripts/claude-setup.sh` (dependency
install, gated on `CLAUDE_CODE_REMOTE`), so local and cloud behave the same.

Recommended environment name: `azahar`
Network access: Trusted (default)

```bash
#!/bin/bash
set -u   # deliberately NOT -e: a non-zero exit blocks the session from starting

apt update && apt install -y \
  libsdl2-dev libpng-dev zlib1g-dev libedit-dev libelf-dev qtbase5-dev || true

exit 0
```

Keep total setup-script runtime under about five minutes so the environment
cache can build. Append `|| true` to anything non-critical - a non-zero exit
stops the session from starting at all.
