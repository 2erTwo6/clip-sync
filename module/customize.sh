#!/system/bin/sh
# KernelSU/Magisk runs this after extracting the module.  Be explicit about
# permissions so the native binary is always executable.
[ -n "$MODPATH" ] && chmod 755 "$MODPATH/bin/clipsync" 2>/dev/null || true
[ -n "$MODPATH" ] && chmod 755 "$MODPATH/service.sh" 2>/dev/null || true
