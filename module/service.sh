#!/system/bin/sh
MODDIR=${0%/*}
BIN="$MODDIR/bin/clipsync"
CONF="$MODDIR/clipsync.conf"

# Some KernelSU/Magisk installers do not preserve the executable bit when
# extracting the module.  Repair it here so the service survives reboots.
chmod 755 "$BIN" 2>/dev/null || true

# Wait for Android to finish booting before touching binder/clipboard.
while [ "$(getprop sys.boot_completed)" != "1" ]; do
    sleep 2
done

if [ ! -f "$CONF" ]; then
    cp "$MODDIR/clipsync.conf.example" "$CONF"
fi

# Migrate the old 300 ms default.  Keep this narrowly scoped so a user who
# intentionally chooses some other value is not affected.
if grep -q '^poll_ms=300[[:space:]]*$' "$CONF" 2>/dev/null; then
    sed -i 's/^poll_ms=300[[:space:]]*$/poll_ms=3000/' "$CONF"
fi

# Native binary drops its effective UID to 2000 internally (needed for
# Android clipboard package ownership). Logs go to /data/local/tmp.
exec "$BIN" --config "$CONF" >> /data/local/tmp/clipsync.log 2>&1
