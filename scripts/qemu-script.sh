#!/usr/bin/env bash
# Drive a headless QEMU boot of a Glassboot image with a timeline of actions.
#
#   scripts/qemu-script.sh bios|uefi IMAGE ACTION...
#
# Actions (run in order):
#   wait:SECONDS      sleep
#   key:NAME          QEMU sendkey (ret, down, up, esc, e, ...), 0.3 s apart
#   keys:N:NAME       send NAME N times
#   shot:FILE.png     save a screenshot
#   type:TEXT         type TEXT (letters, digits, space and . / - _ > < | ; = : ~ ' ")
#                     use "type:...\n" style by following with key:ret
#
# The image is attached as a USB stick with snapshot=on: the guest never modifies it.
# Example: boot the 2nd menu entry and screenshot after 60 s:
#   scripts/qemu-script.sh uefi out/test.img wait:8 key:down key:ret wait:60 shot:/tmp/s.png
set -euo pipefail
fw="$1" img="$2"; shift 2

# QEMU_WRITABLE=1 keeps the guest's writes (e.g. persistence tests); default is snapshot.
snap=",snapshot=on"
[[ "${QEMU_WRITABLE:-}" == 1 ]] && snap=""
args=(-machine q35 -m "${QEMU_MEM:-4096}" -smp 2 -display none -monitor stdio
      -device qemu-xhci,id=xhci
      -drive "if=none,id=stick,format=raw,file=$img$snap"
      -device usb-storage,bus=xhci.0,drive=stick,bootindex=0)
[[ -w /dev/kvm ]] && args+=(-enable-kvm -cpu host)
# QEMU_SERIAL=FILE: capture the guest's serial console (ttyS0) into FILE.
[[ -n "${QEMU_SERIAL:-}" ]] && args+=(-serial "file:$QEMU_SERIAL")
# QEMU_EXTRA="...": extra arguments, e.g. a target disk for installer tests.
# shellcheck disable=SC2206
[[ -n "${QEMU_EXTRA:-}" ]] && args+=($QEMU_EXTRA)
if [[ $fw == uefi ]]; then
  vars=$(mktemp /tmp/ovmf-vars.XXXXXX)
  cp /usr/share/OVMF/OVMF_VARS_4M.fd "$vars"
  args+=(-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd
         -drive "if=pflash,format=raw,file=$vars")
fi

# QEMU sendkey name for one character (US layout).
keyname() {
  local c="$1"
  case "$c" in
    [a-z0-9]) echo "$c" ;;
    [A-Z]) echo "shift-${c,,}" ;;
    ' ') echo spc ;; '.') echo dot ;; '/') echo slash ;; '-') echo minus ;;
    '_') echo shift-minus ;; '>') echo shift-dot ;; '<') echo shift-comma ;;
    '|') echo shift-backslash ;; ';') echo semicolon ;; ':') echo shift-semicolon ;;
    '=') echo equal ;; '~') echo shift-grave_accent ;; ',') echo comma ;;
    "'") echo apostrophe ;; '"') echo shift-apostrophe ;; '&') echo shift-7 ;;
    '$') echo shift-4 ;; '(') echo shift-9 ;; ')') echo shift-0 ;; '\') echo backslash ;;
    *) echo "unsupported character '$c'" >&2 ;;
  esac
}

{
  for a in "$@"; do
    case "$a" in
      type:*) t="${a#type:}"
              for ((i = 0; i < ${#t}; i++)); do echo "sendkey $(keyname "${t:i:1}")"; sleep 0.08; done ;;
      wait:*) sleep "${a#wait:}" ;;
      key:*)  echo "sendkey ${a#key:}"; sleep 0.3 ;;
      keys:*) n="${a#keys:}"; k="${n#*:}"; n="${n%%:*}"
              for ((i = 0; i < n; i++)); do echo "sendkey $k"; sleep 0.3; done ;;
      shot:*) echo "screendump ${a#shot:} -f png"; sleep 0.5 ;;
      *) echo "unknown action $a" >&2 ;;
    esac
  done
  echo quit
} | qemu-system-x86_64 "${args[@]}" >/dev/null
[[ $fw == uefi ]] && rm -f "$vars"
exit 0
