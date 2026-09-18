#!/system/bin/sh
MODDIR=${0%/*}
BIN="$MODDIR/bin/clipsync"
CONF="$MODDIR/clipsync.conf"

# Wait for Android to finish booting before touching binder/clipboard.
while [ "$(getprop sys.boot_completed)" != "1" ]; do
    sleep 2
done

if [ ! -f "$CONF" ]; then
    cp "$MODDIR/clipsync.conf.example" "$CONF"
fi

# Native binary drops its effective UID to 2000 internally (needed for
# Android clipboard package ownership). Logs go to /data/local/tmp.
exec "$BIN" --config "$CONF" >> /data/local/tmp/clipsync.log 2>&1
