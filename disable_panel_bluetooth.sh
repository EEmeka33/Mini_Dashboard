#!/usr/bin/env bash
###############################################################################
# Retire le widget Bluetooth de la barre des taches Pi OS.
#
# Pourquoi : ce widget surveille tous les appareils BlueZ et, des qu'un
# telephone passe a "Paired", il ouvre sa propre fenetre GTK
# ("Pairing successful" / "Connection successful") qui passe devant le
# dashboard et le fait sortir du plein ecran. Le dashboard ne peut pas
# l'empecher : il faut retirer le widget.
#
# Le Bluetooth continue de fonctionner normalement (bluetoothd est un
# service systeme) ; seule l'icone de la barre des taches disparait.
# L'appairage se fait par le bouton "Pair" du dashboard.
#
# Usage :   bash disable_panel_bluetooth.sh            (retire le widget)
#           bash disable_panel_bluetooth.sh --restore  (remet la config d'origine)
###############################################################################
set -euo pipefail

WF_USER="$HOME/.config/wf-panel-pi/wf-panel-pi.ini"     # Wayland (Bookworm par defaut)
WF_SYS="/etc/xdg/wf-panel-pi/wf-panel-pi.ini"
LX_USER="$HOME/.config/lxpanel-pi/panels/panel"          # X11 (lxpanel-pi)
LX_SYS="/etc/xdg/lxpanel-pi/panels/panel"
# Liste par defaut de wf-panel-pi si aucune config ne la definit
WF_DEFAULT_RIGHT="tray power ejecter updater spacing2 connect spacing2 bluetooth spacing2 netman spacing2 volumepulse spacing2 clock spacing2 batt spacing2 squeek"

restart_panel() {
    if pgrep -x wf-panel-pi >/dev/null; then
        pkill -x wf-panel-pi || true      # relance automatiquement par labwc/wayfire
        echo "wf-panel-pi redemarre."
    fi
    if pgrep -x lxpanel-pi >/dev/null; then
        lxpanelctl-pi restart 2>/dev/null || { pkill -x lxpanel-pi || true; }
        echo "lxpanel-pi redemarre."
    fi
}

if [ "${1:-}" = "--restore" ]; then
    for f in "$WF_USER" "$LX_USER"; do
        if [ -f "$f.before-dashboard" ]; then
            mv "$f.before-dashboard" "$f"; echo "Restaure : $f"
        elif [ -f "$f.created-by-dashboard" ]; then
            rm -f "$f" "$f.created-by-dashboard"; echo "Supprime : $f"
        fi
    done
    restart_panel
    exit 0
fi

backup() {   # sauvegarde une seule fois l'etat d'origine
    local f="$1"
    if [ -f "$f" ]; then
        [ -f "$f.before-dashboard" ] || cp "$f" "$f.before-dashboard"
    else
        touch "$f.created-by-dashboard"
    fi
}

changed=0

# ---------- wf-panel-pi (Wayland) ----------
if command -v wf-panel-pi >/dev/null || [ -f "$WF_SYS" ] || [ -f "$WF_USER" ]; then
    mkdir -p "$(dirname "$WF_USER")"
    backup "$WF_USER"
    python3 - "$WF_USER" "$WF_SYS" "$WF_DEFAULT_RIGHT" <<'PY'
import configparser, os, sys
user, system, default_right = sys.argv[1], sys.argv[2], sys.argv[3]

def load(path):
    cp = configparser.RawConfigParser(strict=False)
    cp.optionxform = str                      # garder la casse des cles
    if os.path.exists(path):
        cp.read(path)
    return cp

u, s = load(user), load(system)
right = None
for cp in (u, s):
    if cp.has_option("panel", "widgets_right"):
        right = cp.get("panel", "widgets_right")
        break
if right is None:
    right = default_right

tokens = right.split()
out = []
for i, t in enumerate(tokens):
    if t == "bluetooth":
        # enleve aussi l'espacement qui suit pour ne pas laisser de trou double
        if out and out[-1].startswith("spacing"):
            out.pop()
        continue
    out.append(t)

if not u.has_section("panel"):
    u.add_section("panel")
u.set("panel", "widgets_right", " ".join(out))
with open(user, "w") as f:
    u.write(f)
print("wf-panel-pi : widget bluetooth retire (%s)" % user)
PY
    changed=1
fi

# ---------- lxpanel-pi (X11) ----------
src=""
[ -f "$LX_USER" ] && src="$LX_USER"
[ -z "$src" ] && [ -f "$LX_SYS" ] && src="$LX_SYS"
if [ -n "$src" ]; then
    mkdir -p "$(dirname "$LX_USER")"
    backup "$LX_USER"
    python3 - "$src" "$LX_USER" <<'PY'
import re, sys
src, dst = sys.argv[1], sys.argv[2]
text = open(src).read()
# Supprime le bloc  Plugin { type=bluetooth ... }  (blocs imbriques geres)
out, i, removed = [], 0, 0
pat = re.compile(r'Plugin\s*\{')
while True:
    m = pat.search(text, i)
    if not m:
        out.append(text[i:]); break
    depth, j = 1, m.end()
    while depth and j < len(text):
        depth += {'{': 1, '}': -1}.get(text[j], 0); j += 1
    block = text[m.start():j]
    out.append(text[i:m.start()])
    if re.search(r'^\s*type\s*=\s*bluetooth\s*$', block, re.M):
        removed += 1
    else:
        out.append(block)
    i = j
open(dst, "w").write("".join(out))
print("lxpanel-pi : %d widget(s) bluetooth retire(s) (%s)" % (removed, dst))
PY
    changed=1
fi

[ "$changed" = 1 ] || echo "Aucun panneau Pi OS detecte, rien a faire."
restart_panel
echo "Termine. Pour annuler : bash $0 --restore"
