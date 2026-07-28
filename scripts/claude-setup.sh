#!/usr/bin/env bash
# Claude Code SessionStart hook -- runs in BOTH local and cloud sessions.
#
# Design rule: this script must never break a local Windows session.
# Everything that installs anything is gated behind CLAUDE_CODE_REMOTE=true,
# which Claude Code sets only inside a cloud session. Locally it prints a
# one-line orientation banner and exits 0.
#
# Docs: https://code.claude.com/docs/en/claude-code-on-the-web#setup-scripts-vs-sessionstart-hooks

set -u
cd "${CLAUDE_PROJECT_DIR:-.}" || exit 0

is_cloud() { [ "${CLAUDE_CODE_REMOTE:-}" = "true" ]; }
have()     { command -v "$1" >/dev/null 2>&1; }

if ! is_cloud; then
  echo "[claude-setup] local session -- no cloud provisioning needed."
  exit 0
fi

echo "[claude-setup] cloud session detected; preparing dependencies..."

# ---- Python -----------------------------------------------------------------
if [ -f requirements.txt ]; then
  pip install --quiet -r requirements.txt || echo "[claude-setup] warn: pip install failed"
elif [ -f pyproject.toml ]; then
  pip install --quiet -e . || echo "[claude-setup] warn: pip editable install failed"
fi

# ---- Node -------------------------------------------------------------------
if [ -f package-lock.json ]; then
  npm ci --silent || npm install --silent || echo "[claude-setup] warn: npm install failed"
elif [ -f package.json ]; then
  npm install --silent || echo "[claude-setup] warn: npm install failed"
fi

# ---- .NET -------------------------------------------------------------------
# The cloud image does NOT ship the .NET SDK. Install it in the *environment
# setup script* (see docs/CLOUD-NOTES.md), not here -- setup-script output is
# cached, this hook is not, and a fresh SDK download every session is slow.
if ls ./*.sln >/dev/null 2>&1 || find . -maxdepth 3 -name '*.csproj' -print -quit | grep -q .; then
  if have dotnet; then
    dotnet restore --nologo >/dev/null 2>&1 || echo "[claude-setup] warn: dotnet restore failed"
  else
    echo "[claude-setup] note: no dotnet on PATH. Add the SDK install to the cloud"
    echo "               environment's Setup script -- see docs/CLOUD-NOTES.md."
  fi
fi

# ---- Persist a marker other tooling can read --------------------------------
if [ -n "${CLAUDE_ENV_FILE:-}" ]; then
  echo "CLAUDE_PROJECT_PREPARED=1" >> "$CLAUDE_ENV_FILE"
fi

echo "[claude-setup] done."
exit 0
