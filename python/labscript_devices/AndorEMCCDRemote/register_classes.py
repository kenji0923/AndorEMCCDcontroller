"""Registers the AndorEMCCDRemote BLACS tab with labscript_devices.

labscript_utils executes this file standalone (no package context), so the
import path is spelled out. Change ``user_devices`` if the device lives in
another package (e.g. ``labscript_devices`` when vendored there).
"""

from labscript_devices import register_classes

register_classes(
    "AndorEMCCDRemote",
    BLACS_tab="user_devices.AndorEMCCDRemote.blacs_tabs.AndorEMCCDRemoteTab",
    runviewer_parser=None,
)
