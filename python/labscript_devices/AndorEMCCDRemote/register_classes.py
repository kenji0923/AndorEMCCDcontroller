"""Registers the AndorEMCCDRemote BLACS tab with labscript_devices.

labscript_utils executes this file standalone (no package context), so the
FanLab import path is spelled out. Adjust it if installed in another package.
"""

from labscript_devices import register_classes

register_classes(
    "AndorEMCCDRemote",
    BLACS_tab="user_devices.fanlab_devices.AndorEMCCDRemote.blacs_tabs.AndorEMCCDRemoteTab",
    runviewer_parser=None,
)
