#!/system/bin/sh
# Device-wide workaround; remove this module and reboot to undo.
resetprop ro.build.version.incremental 1692715005
log -t vd_prop "post-fs-data: incremental=$(getprop ro.build.version.incremental)"
